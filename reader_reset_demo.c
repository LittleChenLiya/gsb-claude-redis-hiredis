/*
 * Local compilation and usage demo for redisReaderReset().
 *
 * Build (from the repository root):
 *
 *   cc -o reader_reset_demo reader_reset_demo.c read.c sds.c alloc.c hiredis.c  *          net.c sockcompat.c -lm -lws2_32
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "read.h"
#include "hiredis.h"

/* ---- counting allocator ------------------------------------------------- */

static struct {
    int live;
    int allocs;
    int frees;
    int fail_at;
} cnt;

#define CNT_HDR sizeof(size_t)
static void *cnt_malloc(size_t size) {
    if (cnt.fail_at != 0 && cnt.allocs + 1 == cnt.fail_at) {
        cnt.fail_at = 0;
        return NULL;
    }
    size_t *p = (size_t*)malloc(CNT_HDR + size);
    if (p == NULL) return NULL;
    *p = size;
    cnt.live++;
    cnt.allocs++;
    return (void*)(p + 1);
}

static void cnt_free(void *ptr) {
    if (ptr == NULL) return;
    size_t *p = (size_t*)ptr - 1;
    cnt.live--;
    cnt.frees++;
    free(p);
}

static void *cnt_calloc(size_t nmemb, size_t size) {
    if (cnt.fail_at != 0 && cnt.allocs + 1 == cnt.fail_at) {
        cnt.fail_at = 0;
        return NULL;
    }
    if (size != 0 && nmemb > SIZE_MAX / size) return NULL;
    size_t total = nmemb * size;
    size_t *p = (size_t*)calloc(1, CNT_HDR + total);
    if (p == NULL) return NULL;
    *p = total;
    cnt.live++;
    cnt.allocs++;
    return (void*)(p + 1);
}

static void *cnt_realloc(void *ptr, size_t size) {
    if (ptr == NULL) return cnt_malloc(size);
    if (size == 0) { cnt_free(ptr); return NULL; }
    size_t *old = (size_t*)ptr - 1;
    size_t oldsize = *old;
    void *p = cnt_malloc(size);
    if (p == NULL) return NULL;
    memcpy(p, ptr, oldsize < size ? oldsize : size);
    cnt_free(ptr);
    return p;
}

static char *cnt_strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *p = (char*)cnt_malloc(len);
    if (p != NULL) memcpy(p, s, len);
    return p;
}

static void cnt_reset(void) {
    cnt.live = cnt.allocs = cnt.frees = 0;
    cnt.fail_at = 0;
}

static hiredisAllocFuncs cnt_funcs(void) {
    hiredisAllocFuncs ha = {
        .mallocFn = cnt_malloc,
        .callocFn = cnt_calloc,
        .reallocFn = cnt_realloc,
        .strdupFn = cnt_strdup,
        .freeFn = cnt_free,
    };
    return ha;
}

/* ---- demo --------------------------------------------------------------- */

static int failures;

#define CHECK(cond, msg) do {                                                      if (cond) { printf("  ok: %s\n", msg); }                                       else      { printf("  FAIL: %s\n", msg); failures++; }                     } while (0)

int main(void) {
    redisReader *r;
    void *reply;
    int ret;

    printf("== Demo 1: reuse after a protocol error ==\n");
    r = redisReaderCreate();

    /* Feed half of a two-element array, then a bad protocol byte. */
    ret = redisReaderFeed(r, "*2\r\n$5\r\nhello\r\n", 15);
    CHECK(ret == REDIS_OK, "fed half of an array");
    ret = redisReaderFeed(r, "@foo\r\n", 6);
    CHECK(ret == REDIS_OK, "fed bad protocol byte");
    ret = redisReaderGetReply(r, &reply);
    CHECK(ret == REDIS_ERR && reply == NULL,
          "GetReply reports a protocol error");
    CHECK(r->err == REDIS_ERR_PROTOCOL, "reader is in the error state");

    /* While in the error state, feeding new data is rejected. */
    ret = redisReaderFeed(r, "+PONG\r\n", 7);
    CHECK(ret == REDIS_ERR, "feed is blocked until reset");

    /* Explicitly reset: the reader can be used again. */
    redisReaderReset(r);
    redisReaderReset(r); /* repeated reset is safe */
    ret = redisReaderFeed(r, "+PONG\r\n", 7);
    CHECK(ret == REDIS_OK, "feed works after reset");
    ret = redisReaderGetReply(r, &reply);
    CHECK(ret == REDIS_OK && reply != NULL, "a reply can be read again");
    CHECK(((redisReply*)reply)->type == REDIS_REPLY_STATUS &&
          ((redisReply*)reply)->len == 4 &&
          memcmp(((redisReply*)reply)->str, "PONG", 4) == 0,
          "the reply is the expected +PONG status");
    freeReplyObject(reply);
    redisReaderFree(r);

    printf("== Demo 2: reset frees half-built reply, delivered reply not freed ==\n");
    cnt_reset();
    hiredisAllocFuncs ha = cnt_funcs();
    hiredisSetAllocators(&ha);

    r = redisReaderCreate();
    int baseline = cnt.live;

    /* A protocol error frees the half-built reply. */
    redisReaderFeed(r, "*2\r\n$5\r\nhello\r\n", 15);
    redisReaderFeed(r, "@foo\r\n", 6);
    ret = redisReaderGetReply(r, &reply);
    CHECK(ret == REDIS_ERR, "protocol error while building an array");
    CHECK(cnt.live < baseline, "half-built reply released by the error path");

    /* Reset must not free it again (no double free). */
    redisReaderReset(r);
    CHECK(cnt.live == baseline, "reset does not double-free the half-built reply");

    /* Deliver a reply; the reader must not free it on reset. */
    ret = redisReaderFeed(r, "+PONG\r\n", 7);
    CHECK(ret == REDIS_OK, "feed after reset");
    ret = redisReaderGetReply(r, &reply);
    CHECK(ret == REDIS_OK && reply != NULL, "reply delivered to the caller");
    CHECK(cnt.live == baseline + 2, "delivered reply is still alive (caller owns it)");
    redisReaderReset(r);
    CHECK(cnt.live == baseline + 2, "reset does not reclaim a delivered reply");
    freeReplyObject(reply);
    CHECK(cnt.live == baseline, "caller frees the delivered reply");
    redisReaderFree(r);
    CHECK(cnt.live == 0, "reader freed; no leaks");

    /* Reset also aborts a partial parse with no error. */
    cnt_reset();
    hiredisSetAllocators(&ha);
    r = redisReaderCreate();
    baseline = cnt.live;
    redisReaderFeed(r, "*2\r\n$5\r\nhello\r\n", 15);
    ret = redisReaderGetReply(r, &reply);
    assert(ret == REDIS_OK && reply == NULL);
    CHECK(cnt.live > baseline, "half-built reply exists before reset");
    redisReaderReset(r);
    CHECK(cnt.live == baseline, "reset releases the half-built reply");
    ret = redisReaderFeed(r, "+PONG\r\n", 7);
    CHECK(ret == REDIS_OK, "reader usable after reset");
    ret = redisReaderGetReply(r, &reply);
    CHECK(ret == REDIS_OK && reply != NULL, "reply delivered after reset");
    freeReplyObject(reply);
    redisReaderFree(r);
    CHECK(cnt.live == 0, "no leaks after reader free");

    hiredisResetAllocators();

    printf("== Demo 3: reset preserves reply functions and limits ==\n");
    static redisReplyObjectFunctions custom_fn;
    r = redisReaderCreateWithFunctions(&custom_fn);
    r->maxdepth = 42;
    r->maxelements = 123;
    r->maxbuf = 456;
    r->privdata = (void*)(uintptr_t)0xdeadbeef;
    redisReaderReset(r);
    CHECK(r->fn == &custom_fn, "reply functions preserved");
    CHECK(r->maxdepth == 42, "maxdepth preserved");
    CHECK(r->maxelements == 123, "maxelements preserved");
    CHECK(r->maxbuf == 456, "maxbuf preserved");
    CHECK(r->privdata == (void*)(uintptr_t)0xdeadbeef, "privdata preserved");
    redisReaderFree(r);

    if (failures == 0)
        printf("\nAll checks passed.\n");
    else
        printf("\n%d check(s) failed.\n", failures);
    return failures != 0;
}
