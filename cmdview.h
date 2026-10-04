/*
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2014, Pieter Noordhuis <pcnoordhuis at gmail dot com>
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

#ifndef __HIREDIS_CMDVIEW_H
#define __HIREDIS_CMDVIEW_H

#include <stddef.h> /* size_t */
#include <stdint.h> /* uint64_t */

#include "read.h" /* REDIS_OK / REDIS_ERR */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * redisCmdView - a zero-copy, segmented view of a RESP command.
 *
 * The view encodes a command (argc + argv[]) in the Redis serialization
 * protocol (RESP) WITHOUT copying the argument payloads. The encoded command
 * is exposed as an ordered sequence of byte segments:
 *
 *   segment 0    : the RESP array header  "*<argc>\r\n"   (owned by the view)
 *   for arg i   : "$<len_i>\r\n"  bulk header              (owned by the view)
 *               : argument bytes                           (BORROWED)
 *               : "\r\n"                                    (owned by the view)
 *
 * Concatenating every segment in order reproduces the exact RESP byte stream
 * that redisFormatCommandArgv() produces.
 *
 * Why a view?
 *   redisFormatCommandArgv() allocates one buffer and copies every argument
 *   into it. When arguments are large that doubles memory and pays for a copy
 *   that is never needed -- the bytes only have to be written to a socket.
 *   redisCmdView keeps the argument bytes where they are and only materializes
 *   the small protocol headers, so the cost is independent of the argument
 *   payload sizes.
 *
 * Ownership / lifetime
 * ---------------------
 *  - All header bytes (the "*<argc>\r\n", "$<len>\r\n" and "\r\n" pieces) are
 *    held in a single allocation owned by the view and released by
 *    redisCmdViewFree().
 *  - The argument segments point DIRECTLY at the caller's argv buffers. The
 *    view only borrows them: it never copies, reallocates or frees them. The
 *    caller must keep every argv buffer (and the argv / argvlen arrays) alive
 *    and unmodified for as long as the view is in use.
 *  - redisCmdViewFree() releases the view and its owned header/segment storage
 *    but does NOT free any borrowed argument buffer.
 *
 * Binary safety
 * --------------
 *  - Lengths are byte counts supplied by the caller. They may be 0 (empty byte
 *    strings are allowed) and the bytes may contain NUL as well as arbitrary,
 *    non-UTF-8 values.
 *  - If argvlen is NULL, strlen() is used instead; that path is NOT binary
 *    safe, so pass explicit lengths for arguments that may contain NUL bytes.
 *
 * Iteration
 * ---------
 *  - redisCmdViewRewind() resets the read cursor to the first segment WITHOUT
 *    re-encoding anything; the same segments are produced on every pass.
 *  - redisCmdViewNext() fetches the next segment in order.
 */

/* A single ordered segment: a pointer into either the view's owned header
 * storage or a borrowed argument buffer, plus its byte length. */
typedef struct redisCmdViewSeg {
    const char *data; /* pointer to the segment bytes */
    size_t      len;  /* segment length in bytes */
} redisCmdViewSeg;

typedef struct redisCmdView {
    size_t  argc;     /* number of command arguments */
    size_t  segments; /* total number of segments (1 + 3 * argc) */
    size_t  wire_len; /* total RESP wire length in bytes */

    redisCmdViewSeg *seg; /* ordered segment table, owned by the view */
    char            *hdr; /* owned header bytes */
    size_t           hdrlen;
    size_t           cur; /* iteration cursor */
} redisCmdView;

/*
 * Create a view over the given arguments.
 *
 * argc is the number of arguments; argv is an array of argc byte buffers;
 * argvlen is an array of argc byte lengths (NULL to use strlen()).
 *
 * Returns REDIS_OK on success and sets *out to the new view.
 * Returns REDIS_ERR on failure (invalid arguments, size overflow, or
 * allocation failure); *out is left untouched and no resources are leaked.
 * All size arithmetic is checked for overflow before any allocation is made,
 * so an oversized command is rejected without allocating anything.
 */
int redisCmdViewCreate(redisCmdView **out, int argc, const char **argv,
                       const size_t *argvlen);

/* Reset the iteration cursor to the first segment (no re-encoding). */
void redisCmdViewRewind(redisCmdView *v);

/*
 * Fetch the next segment in order. Returns 1 and fills *seg when a segment
 * was produced, or 0 when all segments have been consumed.
 */
int redisCmdViewNext(redisCmdView *v, redisCmdViewSeg *seg);

/* Total RESP wire length in bytes (the sum of all segment lengths). */
size_t redisCmdViewLen(const redisCmdView *v);

/* Number of segments in the view (1 + 3 * argc). */
size_t redisCmdViewSegments(const redisCmdView *v);

/* Number of command arguments. */
size_t redisCmdViewArgc(const redisCmdView *v);

/*
 * Release the view and its owned header/segment storage. Does NOT free any
 * borrowed argument buffer.
 */
void redisCmdViewFree(redisCmdView *v);

#ifdef __cplusplus
}
#endif

#endif /* __HIREDIS_CMDVIEW_H */
