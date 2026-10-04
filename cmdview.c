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

#include "cmdview.h"
#include "alloc.h"
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <stdint.h>

static int size_add(size_t a, size_t b, size_t *out) {
    if (a > SIZE_MAX - b) return -1;
    *out = a + b;
    return 0;
}

static int size_mul(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) return -1;
    *out = a * b;
    return 0;
}

static uint32_t countDigits64(uint64_t v) {
    uint32_t result = 1;
    for (;;) {
        if (v < 10) return result;
        if (v < 100) return result + 1;
        if (v < 1000) return result + 2;
        if (v < 10000) return result + 3;
        v /= 10000U;
        result += 4;
    }
}

static size_t append_decimal(char *buf, uint64_t v) {
    char tmp[20];
    int n = 0, i;
    if (v == 0) {
        buf[n++] = '0';
    } else {
        while (v != 0) {
            tmp[n++] = (char)('0' + (v % 10));
            v /= 10;
        }
        for (i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    }
    return (size_t)n;
}

static size_t arg_len(int j, const char **argv, const size_t *argvlen) {
    if (argvlen != NULL) return argvlen[j];
    return strlen(argv[j]);
}

int redisCmdViewCreate(redisCmdView **out, int argc, const char **argv,
                        const size_t *argvlen) {
    redisCmdView *v = NULL;
    size_t nseg = 0, hdrlen = 0, payload = 0, segsz = 0, wire = 0, off;
    int j;

    if (out == NULL) return REDIS_ERR;
    *out = NULL;
    if (argc < 0) return REDIS_ERR;
    if (argc > 0 && argv == NULL) return REDIS_ERR;

    if (size_mul((size_t)argc, 3, &nseg) != 0) return REDIS_ERR;
    if (size_add(nseg, 1, &nseg) != 0) return REDIS_ERR;

    if (size_add(hdrlen, (size_t)1 + countDigits64((uint64_t)argc) + 2, &hdrlen) != 0)
        return REDIS_ERR;

    for (j = 0; j < argc; j++) {
        size_t l;
        if (argvlen == NULL && argv[j] == NULL) return REDIS_ERR;
        l = arg_len(j, argv, argvlen);
        if (l > 0 && argv[j] == NULL) return REDIS_ERR;
        if (size_add(hdrlen, (size_t)1 + countDigits64((uint64_t)l) + 2 + 2, &hdrlen) != 0)
            return REDIS_ERR;
        if (size_add(payload, l, &payload) != 0) return REDIS_ERR;
    }

    if (size_add(hdrlen, payload, &wire) != 0) return REDIS_ERR;
    if (size_mul(nseg, sizeof(redisCmdViewSeg), &segsz) != 0) return REDIS_ERR;

    v = hi_calloc(1, sizeof(*v));
    if (v == NULL) return REDIS_ERR;
    v->seg = (redisCmdViewSeg *)hi_malloc(segsz);
    if (v->seg == NULL) { hi_free(v); return REDIS_ERR; }
    v->hdr = (char *)hi_malloc(hdrlen);
    if (v->hdr == NULL) { hi_free(v->seg); hi_free(v); return REDIS_ERR; }

    off = 0;
    v->hdr[off++] = '*';
    off += append_decimal(v->hdr + off, (uint64_t)argc);
    v->hdr[off++] = '\r';
    v->hdr[off++] = '\n';
    v->seg[0].data = v->hdr;
    v->seg[0].len = off;

    for (j = 0; j < argc; j++) {
        size_t l = arg_len(j, argv, argvlen);
        size_t bulk_start = off;
        size_t idx = (size_t)1 + (size_t)3 * (size_t)j;

        v->hdr[off++] = '$';
        off += append_decimal(v->hdr + off, (uint64_t)l);
        v->hdr[off++] = '\r';
        v->hdr[off++] = '\n';
        v->seg[idx].data = v->hdr + bulk_start;
        v->seg[idx].len = off - bulk_start;

        v->seg[idx + 1].data = argv[j];
        v->seg[idx + 1].len = l;

        v->hdr[off++] = '\r';
        v->hdr[off++] = '\n';
        v->seg[idx + 2].data = v->hdr + off - 2;
        v->seg[idx + 2].len = 2;
    }
    assert(off == hdrlen);

    v->argc = (size_t)argc;
    v->segments = nseg;
    v->wire_len = wire;
    v->hdrlen = hdrlen;
    v->cur = 0;
    *out = v;
    return REDIS_OK;
}

void redisCmdViewRewind(redisCmdView *v) { if (v) v->cur = 0; }

int redisCmdViewNext(redisCmdView *v, redisCmdViewSeg *seg) {
    if (v == NULL || seg == NULL) return 0;
    if (v->cur >= v->segments) return 0;
    *seg = v->seg[v->cur];
    v->cur++;
    return 1;
}

size_t redisCmdViewLen(const redisCmdView *v) { return v ? v->wire_len : 0; }
size_t redisCmdViewSegments(const redisCmdView *v) { return v ? v->segments : 0; }
size_t redisCmdViewArgc(const redisCmdView *v) { return v ? v->argc : 0; }

void redisCmdViewFree(redisCmdView *v) {
    if (v == NULL) return;
    hi_free(v->hdr);
    hi_free(v->seg);
    hi_free(v);
}
