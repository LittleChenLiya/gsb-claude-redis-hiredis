/*
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2011, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"
#include <string.h>
#include <stdlib.h>
#ifndef _MSC_VER
#include <unistd.h>
#include <strings.h>
#endif
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <math.h>

#include "alloc.h"
#include "read.h"
#include "sds.h"
#include "win32.h"

#ifndef HIREDIS_FLOAT_STRTOD
/* RESP3 doubles are parsed with ffc (pure-C99 single header) by default: it is
 * several times faster than strtod() and locale-independent (strtod honours
 * LC_NUMERIC, which misparses the always-'.'-separated RESP3 doubles in a
 * non-'.' locale). Define HIREDIS_FLOAT_STRTOD to fall back to strtod() for the
 * default parsing path. The strict numeric path always uses ffc, so it stays
 * locale-independent regardless of this define. */
#define FFC_IMPL   /* emit ffc's implementation in this (sole) translation unit */
#else
/* Even when the default path uses strtod(), strict numeric mode needs the
 * locale-independent ffc parser, so compile it in either way. */
#define FFC_IMPL
#endif
/* ffc.h is a vendored third-party single header. Its public entry points are
 * `extern inline` and call file-local `static` helpers; that is well-defined for
 * our single-TU include but trips Clang's (default-on) -Wstatic-in-inline, which
 * hiredis's -Werror turns into a build failure on Apple Clang. Silence it just
 * for this header. GCC has no such warning. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wstatic-in-inline"
#endif
#include "ffc.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

/* Initial size of our nested reply stack and how much we grow it when needd */
#define REDIS_READER_STACK_SIZE 9

static void __redisReaderSetError(redisReader *r, int type, const char *str) {
    size_t len;

    if (r->reply != NULL && r->fn && r->fn->freeObject) {
        r->fn->freeObject(r->reply);
        r->reply = NULL;
    }

    /* Clear input buffer on errors. */
    sdsfree(r->buf);
    r->buf = NULL;
    r->pos = r->len = 0;

    /* Reset task stack. */
    r->ridx = -1;

    /* Set error. */
    r->err = type;
    len = strlen(str);
    len = len < (sizeof(r->errstr)-1) ? len : (sizeof(r->errstr)-1);
    memcpy(r->errstr,str,len);
    r->errstr[len] = '\0';
}

static size_t chrtos(char *buf, size_t size, char byte) {
    size_t len = 0;

    switch(byte) {
    case '\\':
    case '"':
        len = snprintf(buf,size,"\"\\%c\"",byte);
        break;
    case '\n': len = snprintf(buf,size,"\"\\n\""); break;
    case '\r': len = snprintf(buf,size,"\"\\r\""); break;
    case '\t': len = snprintf(buf,size,"\"\\t\""); break;
    case '\a': len = snprintf(buf,size,"\"\\a\""); break;
    case '\b': len = snprintf(buf,size,"\"\\b\""); break;
    default:
        if (isprint(byte))
            len = snprintf(buf,size,"\"%c\"",byte);
        else
            len = snprintf(buf,size,"\"\\x%02x\"",(unsigned char)byte);
        break;
    }

    return len;
}

static void __redisReaderSetErrorProtocolByte(redisReader *r, char byte) {
    char cbuf[8], sbuf[128];

    chrtos(cbuf,sizeof(cbuf),byte);
    snprintf(sbuf,sizeof(sbuf),
        "Protocol error, got %s as reply type byte", cbuf);
    __redisReaderSetError(r,REDIS_ERR_PROTOCOL,sbuf);
}

static void __redisReaderSetErrorOOM(redisReader *r) {
    __redisReaderSetError(r,REDIS_ERR_OOM,"Out of memory");
}

/* Cumulative byte offset of the start of the field currently being parsed.
 * This is the number of bytes fed to the reader so far minus the bytes still
 * buffered (both unread and partially consumed). It is stable across chunked
 * feeds: the same logical field reports the same offset no matter how the
 * input was split. Must be called before readLine() advances r->pos. */
static size_t redisReaderFieldOffset(redisReader *r) {
    return r->totalbytes - (r->len - r->pos);
}

/* Set a strict-mode protocol error. The message carries the cumulative input
 * byte offset of the offending field so callers can locate it in the stream. */
static void __redisReaderSetErrorStrict(redisReader *r, const char *kind,
        size_t offset)
{
    char msg[160];

    snprintf(msg,sizeof(msg),"%s at byte %zu",kind,offset);
    __redisReaderSetError(r,REDIS_ERR_PROTOCOL,msg);
}

/* Validate the strict finite-double grammar:
 *
 *   [ '-' ] ( digits [ '.' [ digits ] ] | '.' digits ) [ ( 'e' | 'E' ) [ '-' ] digits ]
 *
 * with at least one digit overall and a full consume of the input. No leading
 * '+' (matching the default ffc parser), no whitespace, no trailing junk, and
 * no locale dependency (the decimal separator is always '.'). The explicit
 * inf/nan tokens are handled separately by strictParseDouble(). */
static int strictDoubleGrammar(const char *s, size_t len) {
    const char *p = s;
    const char *e = s + len;
    int digits = 0;

    if (p < e && *p == '-') p++;

    while (p < e && *p >= '0' && *p <= '9') { p++; digits++; }

    if (p < e && *p == '.') {
        p++;
        while (p < e && *p >= '0' && *p <= '9') { p++; digits++; }
    }

    if (digits == 0) return REDIS_ERR;

    if (p < e && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < e && *p == '-') p++;
        int edigits = 0;
        while (p < e && *p >= '0' && *p <= '9') { p++; edigits++; }
        if (edigits == 0) return REDIS_ERR;
    }

    return (p == e) ? REDIS_OK : REDIS_ERR;
}

/* Strictly parse a RESP3 double. Only the explicit "inf"/"-inf"/"nan"/"-nan"
 * tokens (case-insensitive, same as the default parser) or a finite decimal
 * number are accepted. The finite path is grammar-checked and then parsed with
 * ffc, which is locale-independent, so the result does not depend on the
 * process LC_NUMERIC setting. Returns REDIS_OK and stores the value in *out on
 * success, REDIS_ERR otherwise. */
static int strictParseDouble(const char *s, size_t len, double *out) {
    if (len == 3 && strcasecmp(s,"inf") == 0) {
        *out = INFINITY;
        return REDIS_OK;
    }
    if (len == 4 && strcasecmp(s,"-inf") == 0) {
        *out = -INFINITY;
        return REDIS_OK;
    }
    if ((len == 3 && strcasecmp(s,"nan") == 0) ||
        (len == 4 && strcasecmp(s,"-nan") == 0))
    {
        *out = NAN;
        return REDIS_OK;
    }

    if (strictDoubleGrammar(s,len) != REDIS_OK)
        return REDIS_ERR;

    ffc_parse_options o = ffc_parse_options_default();
    o.format |= FFC_FORMAT_FLAG_NO_INFNAN;
    ffc_result res = ffc_from_chars_double_options(s,s+len,out,o);
    if (res.outcome != FFC_OUTCOME_OK || res.ptr != s+len || !isfinite(*out))
        return REDIS_ERR;
    return REDIS_OK;
}

static char *readBytes(redisReader *r, unsigned int bytes) {
    char *p;
    if (r->len-r->pos >= bytes) {
        p = r->buf+r->pos;
        r->pos += bytes;
        return p;
    }
    return NULL;
}

/* Find pointer to \r\n. */
static char *seekNewline(char *s, size_t len) {
    char *ret;

    /* We cannot match with fewer than 2 bytes */
    if (len < 2)
        return NULL;

    /* Search up to len - 1 characters */
    len--;

    /* Look for the \r */
    while ((ret = memchr(s, '\r', len)) != NULL) {
        if (ret[1] == '\n') {
            /* Found. */
            break;
        }
        /* Continue searching. */
        ret++;
        len -= ret - s;
        s = ret;
    }

    return ret;
}

/* Convert a string into a long long. Returns REDIS_OK if the string could be
 * parsed into a (non-overflowing) long long, REDIS_ERR otherwise. The value
 * will be set to the parsed value when appropriate.
 *
 * Note that this function demands that the string strictly represents
 * a long long: no spaces or other characters before or after the string
 * representing the number are accepted, nor zeroes at the start if not
 * for the string "0" representing the zero number.
 *
 * Because of its strictness, it is safe to use this function to check if
 * you can convert a string into a long long, and obtain back the string
 * from the number without any loss in the string representation. */
static int string2ll(const char *s, size_t slen, long long *value) {
    const char *p = s;
    size_t plen = 0;
    int negative = 0;
    unsigned long long v;

    if (plen == slen)
        return REDIS_ERR;

    /* Special case: first and only digit is 0. */
    if (slen == 1 && p[0] == '0') {
        if (value != NULL) *value = 0;
        return REDIS_OK;
    }

    if (p[0] == '-') {
        negative = 1;
        p++; plen++;

        /* Abort on only a negative sign. */
        if (plen == slen)
            return REDIS_ERR;
    }

    /* First digit should be 1-9, otherwise the string should just be 0. */
    if (p[0] >= '1' && p[0] <= '9') {
        v = p[0]-'0';
        p++; plen++;
    } else if (p[0] == '0' && slen == 1) {
        *value = 0;
        return REDIS_OK;
    } else {
        return REDIS_ERR;
    }

    while (plen < slen && p[0] >= '0' && p[0] <= '9') {
        if (v > (ULLONG_MAX / 10)) /* Overflow. */
            return REDIS_ERR;
        v *= 10;

        if (v > (ULLONG_MAX - (p[0]-'0'))) /* Overflow. */
            return REDIS_ERR;
        v += p[0]-'0';

        p++; plen++;
    }

    /* Return if not all bytes were used. */
    if (plen < slen)
        return REDIS_ERR;

    if (negative) {
        if (v > ((unsigned long long)(-(LLONG_MIN+1))+1)) /* Overflow. */
            return REDIS_ERR;
        if (value != NULL) *value = -v;
    } else {
        if (v > LLONG_MAX) /* Overflow. */
            return REDIS_ERR;
        if (value != NULL) *value = v;
    }
    return REDIS_OK;
}

static char *readLine(redisReader *r, int *_len) {
    char *p, *s;
    int len;

    p = r->buf+r->pos;
    s = seekNewline(p,(r->len-r->pos));
    if (s != NULL) {
        len = s-(r->buf+r->pos);
        r->pos += len+2; /* skip \r\n */
        if (_len) *_len = len;
        return p;
    }
    return NULL;
}

static void moveToNextTask(redisReader *r) {
    redisReadTask *cur, *prv;
    while (r->ridx >= 0) {
        /* Return a.s.a.p. when the stack is now empty. */
        if (r->ridx == 0) {
            r->ridx--;
            return;
        }

        cur = r->task[r->ridx];
        prv = r->task[r->ridx-1];
        assert(prv->type == REDIS_REPLY_ARRAY ||
               prv->type == REDIS_REPLY_MAP ||
               prv->type == REDIS_REPLY_ATTR ||
               prv->type == REDIS_REPLY_SET ||
               prv->type == REDIS_REPLY_PUSH);
        if (cur->idx == prv->elements-1) {
            r->ridx--;
        } else {
            /* Reset the type because the next item can be anything */
            assert(cur->idx < prv->elements);
            cur->type = -1;
            cur->elements = -1;
            cur->idx++;
            return;
        }
    }
}

static int processLineItem(redisReader *r) {
    redisReadTask *cur = r->task[r->ridx];
    void *obj;
    char *p;
    int len;
    size_t off = redisReaderFieldOffset(r);

    if ((p = readLine(r,&len)) != NULL) {
        if (cur->type == REDIS_REPLY_INTEGER) {
            long long v;

            if (string2ll(p, len, &v) == REDIS_ERR) {
                if (r->strict_numeric)
                    __redisReaderSetErrorStrict(r,"Bad integer value",off);
                else
                    __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                            "Bad integer value");
                return REDIS_ERR;
            }

            if (r->fn && r->fn->createInteger) {
                obj = r->fn->createInteger(cur,v);
            } else {
                obj = (void*)REDIS_REPLY_INTEGER;
            }
        } else if (cur->type == REDIS_REPLY_DOUBLE) {
            char buf[326];
            double d;

            if ((size_t)len >= sizeof(buf)) {
                if (r->strict_numeric)
                    __redisReaderSetErrorStrict(r,"Double value is too large",off);
                else
                    __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                            "Double value is too large");
                return REDIS_ERR;
            }

            /* Copy + NUL-terminate (as the strtod path does) so the string
             * handed to createDouble keeps its terminator; some custom
             * createDouble callbacks rely on it. ffc itself parses a
             * [start,end) range and needs no terminator. */
            memcpy(buf,p,len);
            buf[len] = '\0';

            if (r->strict_numeric) {
                /* Strict mode: only the explicit inf/nan tokens or a finite
                 * decimal number are accepted, grammar-checked and parsed with
                 * the locale-independent ffc. The result does not depend on
                 * the process LC_NUMERIC setting. */
                if (strictParseDouble(buf,len,&d) != REDIS_OK) {
                    __redisReaderSetErrorStrict(r,"Bad double value",off);
                    return REDIS_ERR;
                }
            } else {
#ifndef HIREDIS_FLOAT_STRTOD
                if (len == 3 && strcasecmp(buf,"inf") == 0) {
                    d = INFINITY; /* Positive infinite. */
                } else if (len == 4 && strcasecmp(buf,"-inf") == 0) {
                    d = -INFINITY; /* Negative infinite. */
                } else if ((len == 3 && strcasecmp(buf,"nan") == 0) ||
                           (len == 4 && strcasecmp(buf, "-nan") == 0)) {
                    d = NAN; /* nan. */
                } else {
                    /* RESP3 allows only finite values besides the inf/nan tokens
                     * handled above, so run ffc with NO_INFNAN; the full-consume +
                     * isfinite checks then mirror the strtod path's eptr/isfinite. */
                    ffc_parse_options o = ffc_parse_options_default();
                    o.format |= FFC_FORMAT_FLAG_NO_INFNAN;
                    ffc_result res = ffc_from_chars_double_options(buf, buf + len, &d, o);
                    if (res.outcome != FFC_OUTCOME_OK || res.ptr != buf + len ||
                        !isfinite(d)) {
                        __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                                "Bad double value");
                        return REDIS_ERR;
                    }
                }
#else
                char *eptr;

                if (len == 3 && strcasecmp(buf,"inf") == 0) {
                    d = INFINITY; /* Positive infinite. */
                } else if (len == 4 && strcasecmp(buf,"-inf") == 0) {
                    d = -INFINITY; /* Negative infinite. */
                } else if ((len == 3 && strcasecmp(buf,"nan") == 0) ||
                           (len == 4 && strcasecmp(buf, "-nan") == 0)) {
                    d = NAN; /* nan. */
                } else {
                    d = strtod((char*)buf,&eptr);
                    /* RESP3 only allows "inf", "-inf", and finite values, while
                     * strtod() allows other variations on infinity,
                     * etc. We explicity handle our two allowed infinite cases and NaN
                     * above, so strtod() should only result in finite values. */
                    if (buf[0] == '\0' || eptr != &buf[len] || !isfinite(d)) {
                        __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                                "Bad double value");
                        return REDIS_ERR;
                    }
                }
#endif
            }

            if (r->fn && r->fn->createDouble) {
                obj = r->fn->createDouble(cur,d,buf,len);
            } else {
                obj = (void*)REDIS_REPLY_DOUBLE;
            }
        } else if (cur->type == REDIS_REPLY_NIL) {
            if (len != 0) {
                __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                        "Bad nil value");
                return REDIS_ERR;
            }

            if (r->fn && r->fn->createNil)
                obj = r->fn->createNil(cur);
            else
                obj = (void*)REDIS_REPLY_NIL;
        } else if (cur->type == REDIS_REPLY_BOOL) {
            int bval;

            if (len != 1 || !strchr("tTfF", p[0])) {
                __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                        "Bad bool value");
                return REDIS_ERR;
            }

            bval = p[0] == 't' || p[0] == 'T';
            if (r->fn && r->fn->createBool)
                obj = r->fn->createBool(cur,bval);
            else
                obj = (void*)REDIS_REPLY_BOOL;
        } else if (cur->type == REDIS_REPLY_BIGNUM) {
            /* Ensure all characters are decimal digits (with possible leading
             * minus sign). */
            for (int i = 0; i < len; i++) {
                /* XXX Consider: Allow leading '+'? Error on leading '0's? */
                if (i == 0 && p[0] == '-') continue;
                if (p[i] < '0' || p[i] > '9') {
                    __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                            "Bad bignum value");
                    return REDIS_ERR;
                }
            }
            if (r->fn && r->fn->createString)
                obj = r->fn->createString(cur,p,len);
            else
                obj = (void*)REDIS_REPLY_BIGNUM;
        } else {
            /* Type will be error or status. */
            for (int i = 0; i < len; i++) {
                if (p[i] == '\r' || p[i] == '\n') {
                    __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                            "Bad simple string value");
                    return REDIS_ERR;
                }
            }
            if (r->fn && r->fn->createString)
                obj = r->fn->createString(cur,p,len);
            else
                obj = (void*)(uintptr_t)(cur->type);
        }

        if (obj == NULL) {
            __redisReaderSetErrorOOM(r);
            return REDIS_ERR;
        }

        /* Set reply if this is the root object. */
        if (r->ridx == 0) r->reply = obj;
        moveToNextTask(r);
        return REDIS_OK;
    }

    return REDIS_ERR;
}

static int processBulkItem(redisReader *r) {
    redisReadTask *cur = r->task[r->ridx];
    void *obj = NULL;
    char *p, *s;
    long long len;
    unsigned long bytelen;
    size_t strict_total = 0;
    int success = 0;
    size_t off = redisReaderFieldOffset(r);

    p = r->buf+r->pos;
    s = seekNewline(p,r->len-r->pos);
    if (s != NULL) {
        p = r->buf+r->pos;
        bytelen = s-(r->buf+r->pos)+2; /* include \r\n */

        if (string2ll(p, bytelen - 2, &len) == REDIS_ERR) {
            if (r->strict_numeric)
                __redisReaderSetErrorStrict(r,"Bad bulk string length",off);
            else
                __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                        "Bad bulk string length");
            return REDIS_ERR;
        }

        if (r->strict_numeric) {
            /* Strict: the length must be a complete decimal that does not
             * overflow, and the entire bulk must fit in the buffer. The
             * size_t arithmetic is checked before any allocation is
             * attempted, so an overflowed length can never reach
             * createString(). */
            if (len < -1) {
                __redisReaderSetErrorStrict(r,"Bulk string length out of range",off);
                return REDIS_ERR;
            }
            if (len == -1) {
                /* The nil object can always be created. */
                if (r->fn && r->fn->createNil)
                    obj = r->fn->createNil(cur);
                else
                    obj = (void*)REDIS_REPLY_NIL;
                strict_total = bytelen;
                success = 1;
            } else {
                size_t content = (size_t)len;
                /* Total bytes the bulk occupies: the length line (bytelen,
                 * including its \r\n) + the content + the trailing \r\n. */
                size_t total = (size_t)bytelen + content + 2;
                if (total < content || r->pos > SIZE_MAX - total) {
                    __redisReaderSetErrorStrict(r,"Bulk string length out of range",off);
                    return REDIS_ERR;
                }
                if (r->pos + total > r->len)
                    return REDIS_ERR; /* incomplete: need more data */
                strict_total = total;
                if ((cur->type == REDIS_REPLY_VERB && len < 4) ||
                    (cur->type == REDIS_REPLY_VERB && s[5] != ':'))
                {
                    __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                            "Verbatim string 4 bytes of content type are "
                            "missing or incorrectly encoded.");
                    return REDIS_ERR;
                }
                if (r->fn && r->fn->createString)
                    obj = r->fn->createString(cur,s+2,len);
                else
                    obj = (void*)(uintptr_t)cur->type;
                success = 1;
            }
        } else {
            if (len < -1 || (LLONG_MAX > SIZE_MAX && len > (long long)SIZE_MAX)) {
                __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                        "Bulk string length out of range");
                return REDIS_ERR;
            }

            if (len == -1) {
                /* The nil object can always be created. */
                if (r->fn && r->fn->createNil)
                    obj = r->fn->createNil(cur);
                else
                    obj = (void*)REDIS_REPLY_NIL;
                success = 1;
            } else {
                /* Only continue when the buffer contains the entire bulk item. */
                bytelen += len+2; /* include \r\n */
                if (r->pos+bytelen <= r->len) {
                    if ((cur->type == REDIS_REPLY_VERB && len < 4) ||
                        (cur->type == REDIS_REPLY_VERB && s[5] != ':'))
                    {
                        __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                                "Verbatim string 4 bytes of content type are "
                                "missing or incorrectly encoded.");
                        return REDIS_ERR;
                    }
                    if (r->fn && r->fn->createString)
                        obj = r->fn->createString(cur,s+2,len);
                    else
                        obj = (void*)(uintptr_t)cur->type;
                    success = 1;
                }
            }
        }

        /* Proceed when obj was created. */
        if (success) {
            if (obj == NULL) {
                __redisReaderSetErrorOOM(r);
                return REDIS_ERR;
            }

            if (r->strict_numeric)
                r->pos += strict_total;
            else
                r->pos += bytelen;

            /* Set reply if this is the root object. */
            if (r->ridx == 0) r->reply = obj;
            moveToNextTask(r);
            return REDIS_OK;
        }
    }

    return REDIS_ERR;
}

static int redisReaderGrow(redisReader *r) {
    redisReadTask **aux;
    int newlen;

    /* Grow our stack size */
    newlen = r->tasks + REDIS_READER_STACK_SIZE;
    aux = hi_realloc(r->task, sizeof(*r->task) * newlen);
    if (aux == NULL)
        goto oom;

    r->task = aux;

    /* Allocate new tasks */
    for (; r->tasks < newlen; r->tasks++) {
        r->task[r->tasks] = hi_calloc(1, sizeof(**r->task));
        if (r->task[r->tasks] == NULL)
            goto oom;
    }

    return REDIS_OK;
oom:
    __redisReaderSetErrorOOM(r);
    return REDIS_ERR;
}

/* Process the array, map and set types. */
static int processAggregateItem(redisReader *r) {
    redisReadTask *cur = r->task[r->ridx];
    void *obj;
    char *p;
    long long elements;
    int root = 0, len;
    size_t off = redisReaderFieldOffset(r);

    if (r->maxdepth > 0 && r->ridx >= r->maxdepth) {
        __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                "Max nesting depth exceeded");
        return REDIS_ERR;
    }

    if (r->ridx == r->tasks - 1) {
        if (redisReaderGrow(r) == REDIS_ERR)
            return REDIS_ERR;
    }

    if ((p = readLine(r,&len)) != NULL) {
        if (string2ll(p, len, &elements) == REDIS_ERR) {
            if (r->strict_numeric)
                __redisReaderSetErrorStrict(r,"Bad multi-bulk length",off);
            else
                __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                        "Bad multi-bulk length");
            return REDIS_ERR;
        }

        root = (r->ridx == 0);

        if (elements < -1 || (LLONG_MAX > SIZE_MAX && elements > SIZE_MAX) ||
            (r->maxelements > 0 && elements > r->maxelements))
        {
            if (r->strict_numeric)
                __redisReaderSetErrorStrict(r,"Multi-bulk length out of range",off);
            else
                __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                        "Multi-bulk length out of range");
            return REDIS_ERR;
        }

        if (elements == -1) {
            if (r->fn && r->fn->createNil)
                obj = r->fn->createNil(cur);
            else
                obj = (void*)REDIS_REPLY_NIL;

            if (obj == NULL) {
                __redisReaderSetErrorOOM(r);
                return REDIS_ERR;
            }

            moveToNextTask(r);
        } else {
            if (cur->type == REDIS_REPLY_MAP || cur->type == REDIS_REPLY_ATTR) {
                long long maxelements = LLONG_MAX / 2;
                if (LLONG_MAX > SIZE_MAX &&
                    maxelements > (long long)(SIZE_MAX / 2)) {
                    maxelements = (long long)(SIZE_MAX / 2);
                }

                if (elements > maxelements) {
                    if (r->strict_numeric)
                        __redisReaderSetErrorStrict(r,"Multi-bulk length out of range",off);
                    else
                        __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                                "Multi-bulk length out of range");
                    return REDIS_ERR;
                }

                elements *= 2;
            }

            /* Strict: block the createArray() allocation before it is
             * attempted when the element count would overflow size_t. The
             * default path relies on calloc() failing, which may surface as
             * OOM; strict mode reports a protocol error instead. */
            if (r->strict_numeric &&
                elements > (long long)(SIZE_MAX / sizeof(void*)))
            {
                __redisReaderSetErrorStrict(r,"Multi-bulk length out of range",off);
                return REDIS_ERR;
            }

            if (r->fn && r->fn->createArray)
                obj = r->fn->createArray(cur,(size_t)elements);
            else
                obj = (void*)(uintptr_t)cur->type;

            if (obj == NULL) {
                __redisReaderSetErrorOOM(r);
                return REDIS_ERR;
            }

            /* Modify task stack when there are more than 0 elements. */
            if (elements > 0) {
                cur->elements = elements;
                cur->obj = obj;
                r->ridx++;
                r->task[r->ridx]->type = -1;
                r->task[r->ridx]->elements = -1;
                r->task[r->ridx]->idx = 0;
                r->task[r->ridx]->obj = NULL;
                r->task[r->ridx]->parent = cur;
                r->task[r->ridx]->privdata = r->privdata;
            } else {
                moveToNextTask(r);
            }
        }

        /* Set reply if this is the root object. */
        if (root) r->reply = obj;
        return REDIS_OK;
    }

    return REDIS_ERR;
}

static int processItem(redisReader *r) {
    redisReadTask *cur = r->task[r->ridx];
    char *p;

    /* check if we need to read type */
    if (cur->type < 0) {
        if ((p = readBytes(r,1)) != NULL) {
            switch (p[0]) {
            case '-':
                cur->type = REDIS_REPLY_ERROR;
                break;
            case '+':
                cur->type = REDIS_REPLY_STATUS;
                break;
            case ':':
                cur->type = REDIS_REPLY_INTEGER;
                break;
            case ',':
                cur->type = REDIS_REPLY_DOUBLE;
                break;
            case '_':
                cur->type = REDIS_REPLY_NIL;
                break;
            case '$':
                cur->type = REDIS_REPLY_STRING;
                break;
            case '*':
                cur->type = REDIS_REPLY_ARRAY;
                break;
            case '%':
                cur->type = REDIS_REPLY_MAP;
                break;
            case '|':
                cur->type = REDIS_REPLY_ATTR;
                break;
            case '~':
                cur->type = REDIS_REPLY_SET;
                break;
            case '#':
                cur->type = REDIS_REPLY_BOOL;
                break;
            case '=':
                cur->type = REDIS_REPLY_VERB;
                break;
            case '>':
                cur->type = REDIS_REPLY_PUSH;
                break;
            case '(':
                cur->type = REDIS_REPLY_BIGNUM;
                break;
            default:
                __redisReaderSetErrorProtocolByte(r,*p);
                return REDIS_ERR;
            }
        } else {
            /* could not consume 1 byte */
            return REDIS_ERR;
        }
    }

    /* process typed item */
    switch(cur->type) {
    case REDIS_REPLY_ERROR:
    case REDIS_REPLY_STATUS:
    case REDIS_REPLY_INTEGER:
    case REDIS_REPLY_DOUBLE:
    case REDIS_REPLY_NIL:
    case REDIS_REPLY_BOOL:
    case REDIS_REPLY_BIGNUM:
        return processLineItem(r);
    case REDIS_REPLY_STRING:
    case REDIS_REPLY_VERB:
        return processBulkItem(r);
    case REDIS_REPLY_ARRAY:
    case REDIS_REPLY_MAP:
    case REDIS_REPLY_ATTR:
    case REDIS_REPLY_SET:
    case REDIS_REPLY_PUSH:
        return processAggregateItem(r);
    default:
        assert(NULL);
        return REDIS_ERR; /* Avoid warning. */
    }
}

redisReader *redisReaderCreateWithFunctions(redisReplyObjectFunctions *fn) {
    redisReader *r;

    r = hi_calloc(1,sizeof(redisReader));
    if (r == NULL)
        return NULL;

    r->buf = sdsempty();
    if (r->buf == NULL)
        goto oom;

    r->task = hi_calloc(REDIS_READER_STACK_SIZE, sizeof(*r->task));
    if (r->task == NULL)
        goto oom;

    for (; r->tasks < REDIS_READER_STACK_SIZE; r->tasks++) {
        r->task[r->tasks] = hi_calloc(1, sizeof(**r->task));
        if (r->task[r->tasks] == NULL)
            goto oom;
    }

    r->fn = fn;
    r->maxbuf = REDIS_READER_MAX_BUF;
    r->maxelements = REDIS_READER_MAX_ARRAY_ELEMENTS;
    r->maxdepth = REDIS_READER_MAX_REPLY_DEPTH;
    r->ridx = -1;
    r->strict_numeric = 0;
    r->totalbytes = 0;

    return r;
oom:
    redisReaderFree(r);
    return NULL;
}

void redisReaderFree(redisReader *r) {
    if (r == NULL)
        return;

    if (r->reply != NULL && r->fn && r->fn->freeObject)
        r->fn->freeObject(r->reply);

    if (r->task) {
        /* We know r->task[i] is allocated if i < r->tasks */
        for (int i = 0; i < r->tasks; i++) {
            hi_free(r->task[i]);
        }

        hi_free(r->task);
    }

    sdsfree(r->buf);
    hi_free(r);
}

void redisReaderSetStrictNumeric(redisReader *r, int strict) {
    r->strict_numeric = strict ? 1 : 0;
}

int redisReaderFeed(redisReader *r, const char *buf, size_t len) {
    sds newbuf;

    /* Return early when this reader is in an erroneous state. */
    if (r->err)
        return REDIS_ERR;

    /* Copy the provided buffer. */
    if (buf != NULL && len >= 1) {
        /* Destroy internal buffer when it is empty and is quite large. */
        if (r->len == 0 && r->maxbuf != 0 && sdsavail(r->buf) > r->maxbuf) {
            sdsfree(r->buf);
            r->buf = sdsempty();
            if (r->buf == 0) goto oom;

            r->pos = 0;
        }

        newbuf = sdscatlen(r->buf,buf,len);
        if (newbuf == NULL) goto oom;

        r->buf = newbuf;
        r->len = sdslen(r->buf);
        r->totalbytes += len;
    }

    return REDIS_OK;
oom:
    __redisReaderSetErrorOOM(r);
    return REDIS_ERR;
}

int redisReaderGetReply(redisReader *r, void **reply) {
    /* Default target pointer to NULL. */
    if (reply != NULL)
        *reply = NULL;

    /* Return early when this reader is in an erroneous state. */
    if (r->err)
        return REDIS_ERR;

    /* When the buffer is empty, there will never be a reply. */
    if (r->len == 0)
        return REDIS_OK;

    /* Set first item to process when the stack is empty. */
    if (r->ridx == -1) {
        r->task[0]->type = -1;
        r->task[0]->elements = -1;
        r->task[0]->idx = -1;
        r->task[0]->obj = NULL;
        r->task[0]->parent = NULL;
        r->task[0]->privdata = r->privdata;
        r->ridx = 0;
    }

    /* Process items in reply. */
    while (r->ridx >= 0)
        if (processItem(r) != REDIS_OK)
            break;

    /* Return ASAP when an error occurred. */
    if (r->err)
        return REDIS_ERR;

    /* Discard part of the buffer when we've consumed at least 1k, to avoid
     * doing unnecessary calls to memmove() in sds.c. */
    if (r->pos >= 1024) {
        if (sdsrange(r->buf,r->pos,-1) < 0) return REDIS_ERR;
        r->pos = 0;
        r->len = sdslen(r->buf);
    }

    /* Emit a reply when there is one. */
    if (r->ridx == -1) {
        if (reply != NULL) {
            *reply = r->reply;
        } else if (r->reply != NULL && r->fn && r->fn->freeObject) {
            r->fn->freeObject(r->reply);
        }
        r->reply = NULL;
    }
    return REDIS_OK;
}
