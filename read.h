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


#ifndef __HIREDIS_READ_H
#define __HIREDIS_READ_H
#include <stdio.h> /* for size_t */

#define REDIS_ERR -1
#define REDIS_OK 0

/* When an error occurs, the err flag in a context is set to hold the type of
 * error that occurred. REDIS_ERR_IO means there was an I/O error and you
 * should use the "errno" variable to find out what is wrong.
 * For other values, the "errstr" field will hold a description. */
#define REDIS_ERR_IO 1 /* Error in read or write */
#define REDIS_ERR_EOF 3 /* End of file */
#define REDIS_ERR_PROTOCOL 4 /* Protocol error */
#define REDIS_ERR_OOM 5 /* Out of memory */
#define REDIS_ERR_TIMEOUT 6 /* Timed out */
#define REDIS_ERR_OTHER 2 /* Everything else... */

#define REDIS_REPLY_STRING 1
#define REDIS_REPLY_ARRAY 2
#define REDIS_REPLY_INTEGER 3
#define REDIS_REPLY_NIL 4
#define REDIS_REPLY_STATUS 5
#define REDIS_REPLY_ERROR 6
#define REDIS_REPLY_DOUBLE 7
#define REDIS_REPLY_BOOL 8
#define REDIS_REPLY_MAP 9
#define REDIS_REPLY_SET 10
#define REDIS_REPLY_ATTR 11
#define REDIS_REPLY_PUSH 12
#define REDIS_REPLY_BIGNUM 13
#define REDIS_REPLY_VERB 14

/* Default max unused reader buffer. */
#define REDIS_READER_MAX_BUF (1024*16)

/* Default multi-bulk element limit */
#define REDIS_READER_MAX_ARRAY_ELEMENTS ((1LL<<32) - 1)

/* Default maximum depth of nested aggregate replies. */
#define REDIS_READER_MAX_REPLY_DEPTH 1024

/* Span type used for source ranges that were terminated by an error
 * (e.g. a protocol error) instead of a complete reply. */
#define REDIS_REPLY_SPAN_ERROR -1

#ifdef __cplusplus
extern "C" {
#endif

/* Source span of a single top-level reply, expressed as absolute byte
 * offsets into the stream of bytes fed to the reader since its creation
 * (or since the last redisReaderReset(), which starts a new session whose
 * offsets restart at 0).
 *
 * [start, end) is a half-open range: start is the offset of the first
 * byte of the reply and end is the offset one past its last byte, so
 * consecutive replies produce consecutive ranges. 'type' is one of the
 * REDIS_REPLY_* protocol types of the top-level reply (a push reply is
 * reported as its own top-level REDIS_REPLY_PUSH range), or
 * REDIS_REPLY_SPAN_ERROR when the range was cut short by an error.
 *
 * Nested replies are never reported separately: their bytes are part of
 * the single range of the enclosing top-level reply. Only offsets and the
 * type are recorded; payload bytes are never copied into a span. */
typedef struct redisReplySpan {
    unsigned long long start; /* absolute offset of the first reply byte */
    unsigned long long end;   /* absolute offset one past the last byte */
    int type;                 /* REDIS_REPLY_* or REDIS_REPLY_SPAN_ERROR */
} redisReplySpan;

typedef struct redisReadTask {
    int type;
    long long elements; /* number of elements in multibulk container */
    int idx; /* index in parent (array) object */
    void *obj; /* holds user-generated value for a read task */
    struct redisReadTask *parent; /* parent task */
    void *privdata; /* user-settable arbitrary field */
} redisReadTask;

typedef struct redisReplyObjectFunctions {
    void *(*createString)(const redisReadTask*, char*, size_t);
    void *(*createArray)(const redisReadTask*, size_t);
    void *(*createInteger)(const redisReadTask*, long long);
    void *(*createDouble)(const redisReadTask*, double, char*, size_t);
    void *(*createNil)(const redisReadTask*);
    void *(*createBool)(const redisReadTask*, int);
    void (*freeObject)(void*);
} redisReplyObjectFunctions;

typedef struct redisReader {
    int err; /* Error flags, 0 when there is no error */
    char errstr[128]; /* String representation of error when applicable */

    char *buf; /* Read buffer */
    size_t pos; /* Buffer cursor */
    size_t len; /* Buffer length */
    size_t maxbuf; /* Max length of unused buffer */
    long long maxelements; /* Max multi-bulk elements */

    redisReadTask **task;
    int tasks;

    int ridx; /* Index of current read task */
    void *reply; /* Temporary reply pointer */

    redisReplyObjectFunctions *fn;
    void *privdata;

    int maxdepth; /* Max nested aggregate reply depth */

    /* Optional reply source span tracking (disabled by default, see
     * redisReaderEnableSpans()). */
    int spans_enabled;             /* nonzero when span tracking is on */
    unsigned long long offset;     /* absolute feed offset of buf[0] */
    unsigned long long span_start; /* absolute offset where the current
                                    * top-level reply started */
    redisReplySpan *spans;         /* recorded spans, grows dynamically */
    size_t spans_len;              /* number of recorded spans */
    size_t spans_cap;              /* allocated capacity of spans */
} redisReader;

/* Public API for the protocol parser. */
redisReader *redisReaderCreateWithFunctions(redisReplyObjectFunctions *fn);
void redisReaderFree(redisReader *r);
int redisReaderFeed(redisReader *r, const char *buf, size_t len);
int redisReaderGetReply(redisReader *r, void **reply);

/* Reset a reader to a fresh state: any error is cleared, the input buffer
 * and the reply task stack are emptied, and all recorded spans are dropped.
 * A reset starts a new session: absolute feed offsets (and therefore the
 * offsets reported in spans) restart at 0. The span tracking enabled flag
 * itself is kept. Returns REDIS_OK on success, REDIS_ERR on failure. */
int redisReaderReset(redisReader *r);

/* Enable or disable source span tracking for top-level replies. Tracking
 * is disabled by default and has no effect on the reply objects produced
 * by redisReaderGetReply(). When enabled, every completed top-level reply
 * appends one redisReplySpan, and an error appends one REDIS_REPLY_SPAN_ERROR
 * span for the incomplete reply. A reply is only reported once it is
 * complete: partially fed input never produces a span. Spans recorded
 * before disabling are kept; use redisReaderClearSpans() to drop them. */
void redisReaderEnableSpans(redisReader *r, int enabled);

/* Return the recorded spans and, when 'count' is not NULL, store the
 * number of spans in it. The returned pointer is owned by the reader and
 * is invalidated by recording new spans, clearing, resetting or freeing
 * the reader. Returns NULL when no spans were recorded. */
const redisReplySpan *redisReaderSpans(const redisReader *r, size_t *count);

/* Drop all recorded spans without disabling tracking. */
void redisReaderClearSpans(redisReader *r);

#define redisReaderSetPrivdata(_r, _p) (int)(((redisReader*)(_r))->privdata = (_p))
#define redisReaderGetObject(_r) (((redisReader*)(_r))->reply)
#define redisReaderGetError(_r) (((redisReader*)(_r))->errstr)

#ifdef __cplusplus
}
#endif

#endif
