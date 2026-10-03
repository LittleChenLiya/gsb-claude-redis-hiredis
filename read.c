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
 * non-'.' locale). Define HIREDIS_FLOAT_STRTOD to fall back to strtod(). */
#define FFC_IMPL   /* emit ffc's implementation in this (sole) translation unit */
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

/* Set a protocol error that carries the absolute byte offset (counted over
 * the cumulative input stream, across all redisReaderFeed() calls) of the
 * offending field. Used by the strict numeric mode. */
static void __redisReaderSetErrorAtOffset(redisReader *r, const char *msg,
                                          unsigned long long offset) {
    char sbuf[128];
    snprintf(sbuf,sizeof(sbuf),"%s at byte %llu",msg,offset);
    __redisReaderSetError(r,REDIS_ERR_PROTOCOL,sbuf);
}

/* Strict decimal integer parse for the optional strict numeric mode.
 *
 * Accepts only an optional leading '-' followed by ASCII decimal digits in
 * canonical form: no leading zeros ("0" itself is fine, "-0" is not), no
 * whitespace, no '+' sign and no trailing garbage. The whole field must be
 * consumed; a partial atoi()-style parse is never accepted. Overflow beyond
 * the long long range is reported as an error before any value is produced.
 *
 * Returns REDIS_OK on success, REDIS_ERR otherwise. */
static int strictParseDecimal(const char *s, size_t slen, long long *value) {
    size_t i = 0;
    int negative = 0;
    unsigned long long v = 0, limit;

    if (slen == 0)
        return REDIS_ERR;

    if (s[0] == '-') {
        negative = 1;
        i = 1;
        if (i == slen) /* lone '-' */
            return REDIS_ERR;
    }

    /* Canonical form: a leading zero is only allowed for the value 0. */
    if (s[i] == '0') {
        if (negative || slen - i != 1)
            return REDIS_ERR;
        if (value != NULL) *value = 0;
        return REDIS_OK;
    }

    limit = negative ? (unsigned long long)LLONG_MAX + 1ULL
                     : (unsigned long long)LLONG_MAX;

    for (; i < slen; i++) {
        unsigned char c = (unsigned char)s[i];
        unsigned int digit;
        if (c < '0' || c > '9') /* hidden whitespace, junk, etc. */
            return REDIS_ERR;
        digit = c - '0';
        if (v > (limit - digit) / 10ULL) /* overflow */
            return REDIS_ERR;
        v = v * 10ULL + digit;
    }

    if (value != NULL) {
        if (negative) {
            if (v == (unsigned long long)LLONG_MAX + 1ULL)
                *value = LLONG_MIN;
            else
                *value = -(long long)v;
        } else {
            *value = (long long)v;
        }
    }
    return REDIS_OK;
}

/* Strict RESP3 double parse for the optional strict numeric mode.
 *
 * Only two kinds of input are accepted:
 *   1. The explicit literals "inf", "-inf", "nan" and "-nan"
 *      (case-insensitive).
 *   2. A finite decimal number in canonical ASCII form:
 *        '-'? D+ ('.' D+)? (('e'|'E') ('+'|'-')? D+)?
 *      i.e. at least one integer digit, an optional fractional part with at
 *      least one digit, and an optional decimal exponent.
 *
 * Everything else is rejected: hex floats, "infinity" and other long forms,
 * leading '+' signs, and any leading/trailing/hidden whitespace or garbage.
 * The conversion is locale-independent: it never consults LC_NUMERIC, so
 * "1.5" parses to the same value under any locale. */
static int strictParseDouble(const char *s, size_t slen, double *value) {
    size_t i = 0, intdigits, fracdigits;

    if (slen == 0)
        return REDIS_ERR;

    /* Explicit inf/nan literals. */
    if (slen == 3 && strncasecmp(s,"inf",3) == 0) {
        *value = INFINITY;
        return REDIS_OK;
    }
    if (slen == 4 && strncasecmp(s,"-inf",4) == 0) {
        *value = -INFINITY;
        return REDIS_OK;
    }
    if ((slen == 3 && strncasecmp(s,"nan",3) == 0) ||
        (slen == 4 && strncasecmp(s,"-nan",4) == 0)) {
        *value = NAN;
        return REDIS_OK;
    }

    /* Finite decimal number. Validate the full grammar first so that no
     * partially-parsed value can leak through. */
    if (s[i] == '-') {
        i++;
        if (i == slen)
            return REDIS_ERR;
    }

    intdigits = 0;
    while (i < slen && s[i] >= '0' && s[i] <= '9') {
        i++; intdigits++;
    }
    if (intdigits == 0)
        return REDIS_ERR;

    fracdigits = 0;
    if (i < slen && s[i] == '.') {
        i++;
        while (i < slen && s[i] >= '0' && s[i] <= '9') {
            i++; fracdigits++;
        }
        if (fracdigits == 0) /* "1." is not canonical */
            return REDIS_ERR;
    }

    if (i < slen && (s[i] == 'e' || s[i] == 'E')) {
        size_t expdigits = 0;
        i++;
        if (i < slen && (s[i] == '+' || s[i] == '-'))
            i++;
        while (i < slen && s[i] >= '0' && s[i] <= '9') {
            i++; expdigits++;
        }
        if (expdigits == 0)
            return REDIS_ERR;
    }

    if (i != slen) /* trailing garbage */
        return REDIS_ERR;

#ifndef HIREDIS_FLOAT_STRTOD
    /* ffc is locale-independent and, given the grammar validated above,
     * consumes the whole range by construction. */
    {
        ffc_parse_options o = ffc_parse_options_default();
        ffc_result res;
        o.format |= FFC_FORMAT_FLAG_NO_INFNAN;
        res = ffc_from_chars_double_options(s, s + slen, value, o);
        if (res.outcome != FFC_OUTCOME_OK || res.ptr != s + slen ||
            !isfinite(*value))
            return REDIS_ERR;
    }
#else
    /* Locale-independent fallback conversion for builds that compile ffc
     * out: gather up to 19 significant decimal digits into an integer
     * mantissa and scale by a power of ten. Never touches strtod(), so the
     * result does not depend on LC_NUMERIC. Note: values with more than 19
     * significant digits may be rounded slightly differently than strtod. */
    {
        unsigned long long mant = 0;
        long exp10 = 0;
        double d;
        size_t j = (s[0] == '-') ? 1 : 0;
        int seen_dot = 0;

        for (; j < slen && s[j] != 'e' && s[j] != 'E'; j++) {
            if (s[j] == '.') {
                seen_dot = 1;
                continue;
            }
            if (mant <= (ULLONG_MAX - 9ULL) / 10ULL) {
                mant = mant * 10ULL + (unsigned)(s[j] - '0');
                if (seen_dot) exp10--;
            } else if (!seen_dot) {
                exp10++; /* dropped low-significance integer digits */
            }
        }
        if (j < slen) { /* exponent */
            long e = 0;
            int eneg = 0;
            j++;
            if (s[j] == '+' || s[j] == '-') {
                eneg = (s[j] == '-');
                j++;
            }
            for (; j < slen; j++) {
                if (e < 100000) /* clamp; pow() saturates anyway */
                    e = e * 10 + (s[j] - '0');
            }
            exp10 += eneg ? -e : e;
        }
        if (exp10 >= 0)
            d = (double)mant * pow(10.0, (double)exp10);
        else
            d = (double)mant / pow(10.0, (double)-exp10);
        if (!isfinite(d))
            return REDIS_ERR;
        *value = (s[0] == '-') ? -d : d;
    }
#endif
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

    if ((p = readLine(r,&len)) != NULL) {
        if (cur->type == REDIS_REPLY_INTEGER) {
            long long v;

            if (r->strict_numeric) {
                if (strictParseDecimal(p, len, &v) == REDIS_ERR) {
                    __redisReaderSetErrorAtOffset(r,
                            "Strict numeric: bad integer value",
                            r->consumed + (size_t)(p - r->buf));
                    return REDIS_ERR;
                }
            } else if (string2ll(p, len, &v) == REDIS_ERR) {
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
                /* Strict, locale-independent parse; rejects trailing
                 * garbage, hidden whitespace and non-canonical forms. */
                if (strictParseDouble(p, len, &d) == REDIS_ERR) {
                    __redisReaderSetErrorAtOffset(r,
                            "Strict numeric: bad double value",
                            r->consumed + (size_t)(p - r->buf));
                    return REDIS_ERR;
                }

                if (r->fn && r->fn->createDouble) {
                    obj = r->fn->createDouble(cur,d,buf,len);
                } else {
                    obj = (void*)REDIS_REPLY_DOUBLE;
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

            if (r->fn && r->fn->createDouble) {
                obj = r->fn->createDouble(cur,d,buf,len);
            } else {
                obj = (void*)REDIS_REPLY_DOUBLE;
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

            if (r->fn && r->fn->createDouble) {
                obj = r->fn->createDouble(cur,d,buf,len);
            } else {
                obj = (void*)REDIS_REPLY_DOUBLE;
            }
#endif
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
    int success = 0;

    p = r->buf+r->pos;
    s = seekNewline(p,r->len-r->pos);
    if (s != NULL) {
        p = r->buf+r->pos;
        bytelen = s-(r->buf+r->pos)+2; /* include \r\n */

        if (r->strict_numeric) {
            /* Strict mode: the length must be a complete, canonical,
             * non-overflowing decimal number. Overflow or junk is rejected
             * here, before any allocation or range decision is made. */
            if (strictParseDecimal(p, bytelen - 2, &len) == REDIS_ERR) {
                __redisReaderSetErrorAtOffset(r,
                        "Strict numeric: bad bulk string length",
                        r->consumed + (size_t)(p - r->buf));
                return REDIS_ERR;
            }
        } else if (string2ll(p, bytelen - 2, &len) == REDIS_ERR) {
            __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                    "Bad bulk string length");
            return REDIS_ERR;
        }

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
            int complete;
            if (r->strict_numeric) {
                /* Strict mode: compute the total item size with 64-bit,
                 * overflow-safe arithmetic, so an extreme (but in-range)
                 * length can never wrap the cursor math and trigger an
                 * allocation. The allocation only happens once the whole
                 * item is already buffered. */
                unsigned long long need = (unsigned long long)bytelen +
                                          (unsigned long long)len + 2ULL;
                complete = (need <= (unsigned long long)(r->len - r->pos));
                if (complete)
                    bytelen = (unsigned long)need;
            } else {
                bytelen += len+2; /* include \r\n */
                complete = (r->pos+bytelen <= r->len);
            }
            if (complete) {
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

        /* Proceed when obj was created. */
        if (success) {
            if (obj == NULL) {
                __redisReaderSetErrorOOM(r);
                return REDIS_ERR;
            }

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
        if (r->strict_numeric) {
            /* Strict mode: reject overflow/junk before the element count is
             * used for any allocation (createArray) or range check. */
            if (strictParseDecimal(p, len, &elements) == REDIS_ERR) {
                __redisReaderSetErrorAtOffset(r,
                        "Strict numeric: bad multi-bulk length",
                        r->consumed + (size_t)(p - r->buf));
                return REDIS_ERR;
            }
        } else if (string2ll(p, len, &elements) == REDIS_ERR) {
            __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                    "Bad multi-bulk length");
            return REDIS_ERR;
        }

        root = (r->ridx == 0);

        if (elements < -1 || (LLONG_MAX > SIZE_MAX && elements > SIZE_MAX) ||
            (r->maxelements > 0 && elements > r->maxelements))
        {
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
                    __redisReaderSetError(r,REDIS_ERR_PROTOCOL,
                            "Multi-bulk length out of range");
                    return REDIS_ERR;
                }

                elements *= 2;
            }

            if (r->fn && r->fn->createArray)
                obj = r->fn->createArray(cur,elements);
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

void redisReaderSetStrictNumeric(redisReader *r, int enabled) {
    if (r == NULL)
        return;
    r->strict_numeric = (enabled != 0);
}

int redisReaderFeed(redisReader *r, const char *buf, size_t len) {    sds newbuf;

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
     * doing unnecessary calls to memmove() in sds.c. Account the discarded
     * bytes so error offsets stay relative to the cumulative input stream. */
    if (r->pos >= 1024) {
        if (sdsrange(r->buf,r->pos,-1) < 0) return REDIS_ERR;
        r->consumed += r->pos;
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
