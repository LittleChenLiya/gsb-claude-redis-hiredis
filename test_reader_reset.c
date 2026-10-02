/*
 * Standalone acceptance test for redisReaderReset().
 *
 * Scenarios covered:
 *   1. Half-fed array followed by a bad protocol byte -> protocol error;
 *      after redisReaderReset() a legal PING reply can be read again.
 *   2. A counting allocator proves the half-built reply tree is released,
 *      while a reply already delivered to the caller is never freed by the
 *      reader (no double free).
 *   3. Caller configuration survives reset: custom reply object functions,
 *      privdata and maxdepth are preserved (error state and buffer are not).
 *   4. While the reader is erroneous, Feed/GetReply keep failing until a
 *      reset succeeds (no half-stale data can be fed).
 *   5. OOM injection during reset: reset fails, reader stays erroneous,
 *      a later reset recovers it.
 *   6. Repeated reset and reset on a fresh (empty) reader are safe.
 *
 * Local compile demo (run from the repository root, no hiredis.c needed):
 *
 *   gcc -Wall -Wextra -std=c99 -o test_reader_reset \
 *       test_reader_reset.c read.c sds.c alloc.c
 *   ./test_reader_reset
 *
 * (MSVC: cl /W4 test_reader_reset.c read.c sds.c alloc.c)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alloc.h"
#include "read.h"

static int failures = 0;

#define CHECK(cond) do {                                                \
    if (!(cond)) {                                                      \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++;                                                     \
    }                                                                   \
} while (0)

/* ------------------------------------------------------------------ */
/* Counting / fail-injecting allocator, installed via hiredis.         */
/* ------------------------------------------------------------------ */

static long live_allocs = 0;    /* malloc/calloc/strdup/realloc(NULL) - free */
static int fail_allocs = 0;     /* when non-zero, every allocation fails */

static void *t_malloc(size_t sz) {
    if (fail_allocs) return NULL;
    live_allocs++;
    return malloc(sz);
}

static void *t_calloc(size_t n, size_t sz) {
    if (fail_allocs) return NULL;
    live_allocs++;
    return calloc(n, sz);
}

static void *t_realloc(void *p, size_t sz) {
    if (p == NULL) {
        if (fail_allocs) return NULL;
        live_allocs++;
    }
    return realloc(p, sz);
}

static char *t_strdup(const char *s) {
    size_t len;
    char *out;
    if (fail_allocs) return NULL;
    live_allocs++;
    len = strlen(s) + 1;
    out = malloc(len);
    if (out) memcpy(out, s, len);
    return out;
}

static void t_free(void *p) {
    if (p != NULL) live_allocs--;
    free(p);
}

/* ------------------------------------------------------------------ */
/* Custom reply object functions with create/free counters. Objects    */
/* are allocated through hi_* so the counting allocator sees them.     */
/* ------------------------------------------------------------------ */

typedef struct tReply {
    int type;
    char *str;
    size_t elements;
    struct tReply **element;
} tReply;

static long objs_created = 0;
static long objs_freed = 0;

static void tFreeObject(void *ptr) {
    tReply *o = ptr;
    size_t i;
    if (o == NULL) return;
    if (o->element != NULL) {
        for (i = 0; i < o->elements; i++)
            tFreeObject(o->element[i]);
        hi_free(o->element);
    }
    hi_free(o->str);
    hi_free(o);
    objs_freed++;
}

static tReply *tNewObj(const redisReadTask *task) {
    tReply *o = hi_calloc(1, sizeof(*o));
    if (o == NULL) return NULL;
    o->type = task->type;
    objs_created++;
    return o;
}

static void tAttach(const redisReadTask *task, tReply *o) {
    if (task->parent != NULL) {
        tReply *parent = task->parent->obj;
        parent->element[task->idx] = o;
    }
}

static void *tCreateString(const redisReadTask *task, char *str, size_t len) {
    tReply *o = tNewObj(task);
    if (o == NULL) return NULL;
    o->str = hi_malloc(len + 1);
    if (o->str == NULL) { tFreeObject(o); return NULL; }
    memcpy(o->str, str, len);
    o->str[len] = '\0';
    tAttach(task, o);
    return o;
}

static void *tCreateArray(const redisReadTask *task, size_t elements) {
    tReply *o = tNewObj(task);
    if (o == NULL) return NULL;
    o->elements = elements;
    if (elements > 0) {
        o->element = hi_calloc(elements, sizeof(tReply*));
        if (o->element == NULL) { tFreeObject(o); return NULL; }
    }
    tAttach(task, o);
    return o;
}

static void *tCreateInteger(const redisReadTask *task, long long value) {
    tReply *o;
    (void)value;
    o = tNewObj(task);
    if (o) tAttach(task, o);
    return o;
}

static void *tCreateDouble(const redisReadTask *task, double value, char *str, size_t len) {
    (void)value;
    return tCreateString(task, str, len);
}

static void *tCreateNil(const redisReadTask *task) {
    tReply *o = tNewObj(task);
    if (o) tAttach(task, o);
    return o;
}

static void *tCreateBool(const redisReadTask *task, int bval) {
    tReply *o;
    (void)bval;
    o = tNewObj(task);
    if (o) tAttach(task, o);
    return o;
}

static redisReplyObjectFunctions tFns = {
    tCreateString,
    tCreateArray,
    tCreateInteger,
    tCreateDouble,
    tCreateNil,
    tCreateBool,
    tFreeObject
};

/* ------------------------------------------------------------------ */

int main(void) {
    hiredisAllocFuncs ha = { t_malloc, t_calloc, t_realloc, t_strdup, t_free };
    redisReader *r, *r2;
    tReply *ping = NULL;
    void *reply = NULL;
    int privdata = 0x1234;

    hiredisSetAllocators(&ha);

    r = redisReaderCreateWithFunctions(&tFns);
    CHECK(r != NULL);
    CHECK(r->fn == &tFns);

    /* Caller configuration that must survive reset. (Assign privdata
     * directly: the redisReaderSetPrivdata macro casts the pointer to int,
     * which trips -Werror on LLP64 platforms.) */
    r->privdata = &privdata;
    r->maxdepth = 1;

    /* --- 1. Half array, then a bad protocol byte -> error ------------- */
    CHECK(redisReaderFeed(r, "*2\r\n$5\r\nhello\r\n", 15) == REDIS_OK);
    CHECK(redisReaderGetReply(r, &reply) == REDIS_OK);
    CHECK(reply == NULL);                        /* incomplete: no reply yet */
    CHECK(objs_created == 2 && objs_freed == 0); /* root array + "hello"   */

    CHECK(redisReaderFeed(r, "!", 1) == REDIS_OK);
    CHECK(redisReaderGetReply(r, &reply) == REDIS_ERR);
    CHECK(r->err == REDIS_ERR_PROTOCOL);
    CHECK(strstr(r->errstr, "Protocol error") != NULL);
    /* The half-built tree was released when the error was raised. */
    CHECK(objs_freed == objs_created);

    /* --- 4. While erroneous, Feed/GetReply keep failing --------------- */
    CHECK(redisReaderFeed(r, "+PONG\r\n", 7) == REDIS_ERR);
    CHECK(redisReaderGetReply(r, &reply) == REDIS_ERR);

    /* --- Reset, then a legal PING reply must be readable -------------- */
    CHECK(redisReaderReset(r) == REDIS_OK);
    CHECK(r->err == 0 && r->errstr[0] == '\0');
    CHECK(r->len == 0 && r->pos == 0 && r->ridx == -1 && r->reply == NULL);

    /* --- 3. Caller configuration survives reset ----------------------- */
    CHECK(r->fn == &tFns);
    CHECK(r->privdata == &privdata);
    CHECK(r->maxdepth == 1);
    CHECK(r->maxelements == REDIS_READER_MAX_ARRAY_ELEMENTS);
    CHECK(r->maxbuf == REDIS_READER_MAX_BUF);

    /* maxdepth is still enforced after reset. */
    CHECK(redisReaderFeed(r, "*1\r\n*1\r\n:1\r\n", 12) == REDIS_OK);
    CHECK(redisReaderGetReply(r, &reply) == REDIS_ERR);
    CHECK(strstr(r->errstr, "Max nesting depth") != NULL);

    CHECK(redisReaderReset(r) == REDIS_OK);
    CHECK(redisReaderFeed(r, "+PING\r\n", 7) == REDIS_OK);
    CHECK(redisReaderGetReply(r, &reply) == REDIS_OK);
    CHECK(reply != NULL);
    ping = reply;
    CHECK(ping->type == REDIS_REPLY_STATUS);
    CHECK(strcmp(ping->str, "PING") == 0);

    /* --- 2. Delivered reply is never reclaimed by the reader ---------- */
    {
        long freed_before = objs_freed;
        CHECK(redisReaderReset(r) == REDIS_OK);   /* must not free `ping` */
        CHECK(redisReaderReset(r) == REDIS_OK);   /* repeated reset: safe */
        CHECK(objs_freed == freed_before);
    }

    /* --- 6. Reset on a fresh (empty) reader is safe -------------------- */
    r2 = redisReaderCreateWithFunctions(&tFns);
    CHECK(r2 != NULL);
    CHECK(redisReaderReset(r2) == REDIS_OK);
    CHECK(redisReaderReset(r2) == REDIS_OK);
    CHECK(redisReaderFeed(r2, ":1\r\n", 4) == REDIS_OK);
    reply = NULL;
    CHECK(redisReaderGetReply(r2, &reply) == REDIS_OK);
    CHECK(reply != NULL);
    tFreeObject(reply);
    redisReaderFree(r2);

    /* --- 5. OOM injection during reset --------------------------------- */
    CHECK(redisReaderFeed(r, "?", 1) == REDIS_OK);
    CHECK(redisReaderGetReply(r, &reply) == REDIS_ERR); /* protocol error */
    fail_allocs = 1;
    CHECK(redisReaderReset(r) == REDIS_ERR);            /* sdsempty() OOM */
    CHECK(r->err == REDIS_ERR_OOM);
    /* Reader stays erroneous: still refuses to be fed. */
    CHECK(redisReaderFeed(r, "+PING\r\n", 7) == REDIS_ERR);
    CHECK(redisReaderGetReply(r, &reply) == REDIS_ERR);
    fail_allocs = 0;
    CHECK(redisReaderReset(r) == REDIS_OK);             /* recovers */
    CHECK(redisReaderFeed(r, "+PING\r\n", 7) == REDIS_OK);
    reply = NULL;
    CHECK(redisReaderGetReply(r, &reply) == REDIS_OK);
    CHECK(reply != NULL);
    tFreeObject(reply);

    /* Freeing the reader must not reclaim the delivered PING reply. */
    {
        long freed_before = objs_freed;
        redisReaderFree(r);
        CHECK(objs_freed == freed_before);
    }
    tFreeObject(ping); /* caller frees its own delivered reply, exactly once */

    /* --- 2. Counting allocator: everything balanced -------------------- */
    CHECK(objs_created == objs_freed);
    CHECK(live_allocs == 0);

    if (failures == 0) {
        printf("test_reader_reset: all checks passed "
               "(objects created/freed: %ld/%ld, live allocs: %ld)\n",
               objs_created, objs_freed, live_allocs);
        return 0;
    }
    fprintf(stderr, "test_reader_reset: %d check(s) failed\n", failures);
    return 1;
}
