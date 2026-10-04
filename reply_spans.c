/*
 * reply_spans.c -- demo and acceptance checks for reader source spans.
 *
 * Feeds chunked RESP into a redisReader with span tracking enabled and
 * prints the [start, end) source range of every top-level reply, then
 * verifies the ranges against the expected values. Exits non-zero when
 * any check fails.
 *
 * Build (from the repository root):
 *     make reply-spans && ./reply-spans
 * or directly:
 *     gcc -I. -o reply-spans reply_spans.c read.c sds.c alloc.c
 */

#include <stdio.h>
#include <string.h>

#include "read.h"

static int failures = 0;

#define CHECK(cond) do {                                                \
    if (!(cond)) {                                                      \
        printf("  FAIL: %s (line %d)\n", #cond, __LINE__);              \
        failures++;                                                     \
    }                                                                   \
} while (0)

static const char *type_name(int type) {
    switch (type) {
    case REDIS_REPLY_STRING:  return "STRING";
    case REDIS_REPLY_ARRAY:   return "ARRAY";
    case REDIS_REPLY_INTEGER: return "INTEGER";
    case REDIS_REPLY_NIL:     return "NIL";
    case REDIS_REPLY_STATUS:  return "STATUS";
    case REDIS_REPLY_ERROR:   return "ERROR";
    case REDIS_REPLY_DOUBLE:  return "DOUBLE";
    case REDIS_REPLY_BOOL:    return "BOOL";
    case REDIS_REPLY_MAP:     return "MAP";
    case REDIS_REPLY_SET:     return "SET";
    case REDIS_REPLY_ATTR:    return "ATTR";
    case REDIS_REPLY_PUSH:    return "PUSH";
    case REDIS_REPLY_BIGNUM:  return "BIGNUM";
    case REDIS_REPLY_VERB:    return "VERB";
    case REDIS_REPLY_SPAN_ERROR: return "SPAN_ERROR";
    default:                  return "?";
    }
}

/* Print all spans recorded by the reader. */
static void print_spans(const char *label, redisReader *r) {
    size_t n, i;
    const redisReplySpan *spans = redisReaderSpans(r, &n);

    printf("%s: %zu span(s)\n", label, n);
    for (i = 0; i < n; i++) {
        printf("  span[%zu] = [%llu, %llu) len=%llu type=%s\n", i,
               (unsigned long long)spans[i].start,
               (unsigned long long)spans[i].end,
               (unsigned long long)(spans[i].end - spans[i].start),
               type_name(spans[i].type));
    }
}

static size_t span_count(redisReader *r) {
    size_t n = 0;
    redisReaderSpans(r, &n);
    return n;
}

/* Drain all immediately available replies; returns the last reply. */
static void *drain(redisReader *r) {
    void *reply = NULL, *last = NULL;
    while (redisReaderGetReply(r, &reply) == REDIS_OK && reply != NULL)
        last = reply;
    return last;
}

/* Two replies fed across three feeds must yield two consecutive ranges,
 * and no range may be reported before its reply is complete. */
static void test_two_replies_three_feeds(void) {
    redisReader *r = redisReaderCreateWithFunctions(NULL);
    const redisReplySpan *spans;
    void *reply = (void*)1;

    printf("\n== two replies across three feeds ==\n");
    redisReaderEnableSpans(r, 1);

    /* Stream is "+OK\r\n:42\r\n" (10 bytes), split into 3 feeds. */
    CHECK(redisReaderFeed(r, "+OK\r", 4) == REDIS_OK);
    CHECK(redisReaderGetReply(r, &reply) == REDIS_OK && reply == NULL);
    CHECK(span_count(r) == 0); /* partial input: nothing reported yet */

    CHECK(redisReaderFeed(r, "\n:4", 3) == REDIS_OK);
    CHECK(redisReaderGetReply(r, &reply) == REDIS_OK &&
          reply == (void*)REDIS_REPLY_STATUS);
    CHECK(span_count(r) == 1); /* first reply complete exactly here */

    CHECK(redisReaderFeed(r, "2\r\n", 3) == REDIS_OK);
    CHECK(redisReaderGetReply(r, &reply) == REDIS_OK &&
          reply == (void*)REDIS_REPLY_INTEGER);
    CHECK(span_count(r) == 2);

    print_spans("two replies, three feeds", r);
    spans = redisReaderSpans(r, NULL);
    CHECK(spans != NULL && spans[0].start == 0 && spans[0].end == 5 &&
          spans[0].type == REDIS_REPLY_STATUS);
    CHECK(spans != NULL && spans[1].start == 5 && spans[1].end == 10 &&
          spans[1].type == REDIS_REPLY_INTEGER);
    CHECK(spans != NULL && spans[0].end == spans[1].start); /* consecutive ranges */

    redisReaderFree(r);
}

/* A nested array yields a single top-level range; a push reply and the
 * normal reply after it are reported as separate ranges. */
static void test_nested_push_then_reply(void) {
    redisReader *r = redisReaderCreateWithFunctions(NULL);
    const redisReplySpan *spans;
    const char *nested = "*2\r\n*2\r\n$3\r\nfoo\r\n:1\r\n$3\r\nbar\r\n"; /* 30 */
    const char *push   = ">2\r\n$7\r\nmessage\r\n$5\r\nhello\r\n"; /* 28 */
    const char *status = "+DONE\r\n";                              /* 7  */

    printf("\n== nested array, then push, then normal reply ==\n");
    redisReaderEnableSpans(r, 1);

    CHECK(redisReaderFeed(r, nested, strlen(nested)) == REDIS_OK);
    drain(r);
    CHECK(span_count(r) == 1); /* nested array: one top-level range only */

    CHECK(redisReaderFeed(r, push, strlen(push)) == REDIS_OK);
    drain(r);
    CHECK(redisReaderFeed(r, status, strlen(status)) == REDIS_OK);
    drain(r);
    CHECK(span_count(r) == 3);

    print_spans("nested + push + status", r);
    spans = redisReaderSpans(r, NULL);
    CHECK(spans != NULL && spans[0].start == 0 && spans[0].end == 30 &&
          spans[0].type == REDIS_REPLY_ARRAY);
    CHECK(spans != NULL && spans[1].start == 30 && spans[1].end == 58 &&
          spans[1].type == REDIS_REPLY_PUSH);
    CHECK(spans != NULL && spans[2].start == 58 && spans[2].end == 65 &&
          spans[2].type == REDIS_REPLY_STATUS);

    redisReaderFree(r);
}

/* A CRLF inside binary bulk content is payload, not a reply boundary. */
static void test_binary_bulk_crlf(void) {
    redisReader *r = redisReaderCreateWithFunctions(NULL);
    const redisReplySpan *spans;
    void *reply = (void*)1;

    printf("\n== binary bulk with embedded CRLF ==\n");
    redisReaderEnableSpans(r, 1);

    /* "$6\r\nab\r\ncd\r\n" (12 bytes); the content "ab\r\ncd" holds a CRLF.
     * Feed it split right through the embedded CRLF. */
    CHECK(redisReaderFeed(r, "$6\r\nab\r", 7) == REDIS_OK);
    CHECK(redisReaderGetReply(r, &reply) == REDIS_OK && reply == NULL);
    CHECK(span_count(r) == 0); /* not complete, not cut at the inner CR */

    CHECK(redisReaderFeed(r, "\ncd\r\n", 5) == REDIS_OK);
    CHECK(redisReaderGetReply(r, &reply) == REDIS_OK &&
          reply == (void*)REDIS_REPLY_STRING);
    CHECK(span_count(r) == 1);

    print_spans("binary bulk", r);
    spans = redisReaderSpans(r, NULL);
    /* The range must cover the whole 12-byte item; a range ending at 8
     * would mean the embedded CRLF was mistaken for a boundary. */
    CHECK(spans != NULL && spans[0].start == 0 && spans[0].end == 12 &&
          spans[0].type == REDIS_REPLY_STRING);

    redisReaderFree(r);
}

/* A protocol error produces an error range; after a reset the new
 * session's offsets restart at 0. */
static void test_error_span_and_reset(void) {
    redisReader *r = redisReaderCreateWithFunctions(NULL);
    const redisReplySpan *spans;
    void *reply = NULL;

    printf("\n== protocol error span, then reset ==\n");
    redisReaderEnableSpans(r, 1);

    CHECK(redisReaderFeed(r, "!xyz\r\n", 6) == REDIS_OK);
    CHECK(redisReaderGetReply(r, &reply) == REDIS_ERR);
    CHECK(r->err == REDIS_ERR_PROTOCOL);
    CHECK(span_count(r) == 1);

    print_spans("protocol error", r);
    spans = redisReaderSpans(r, NULL);
    CHECK(spans != NULL && spans[0].start == 0 && spans[0].end == 1 &&
          spans[0].type == REDIS_REPLY_SPAN_ERROR);

    /* Reset starts a new session: offsets restart at 0, old spans go. */
    CHECK(redisReaderReset(r) == REDIS_OK);
    CHECK(span_count(r) == 0);

    CHECK(redisReaderFeed(r, "+OK\r\n", 5) == REDIS_OK);
    CHECK(drain(r) == (void*)REDIS_REPLY_STATUS);
    CHECK(span_count(r) == 1);

    print_spans("after reset (new session)", r);
    spans = redisReaderSpans(r, NULL);
    CHECK(spans != NULL && spans[0].start == 0 && spans[0].end == 5 &&
          spans[0].type == REDIS_REPLY_STATUS);

    redisReaderFree(r);
}

/* An empty bulk string has a well-defined 5-byte range. */
static void test_empty_bulk(void) {
    redisReader *r = redisReaderCreateWithFunctions(NULL);
    const redisReplySpan *spans;

    printf("\n== empty bulk string ==\n");
    redisReaderEnableSpans(r, 1);

    CHECK(redisReaderFeed(r, "$0\r\n\r\n", 6) == REDIS_OK);
    CHECK(drain(r) == (void*)REDIS_REPLY_STRING);
    CHECK(span_count(r) == 1);

    print_spans("empty bulk", r);
    spans = redisReaderSpans(r, NULL);
    /* Header "$0\r\n" plus the empty content's terminating "\r\n". */
    CHECK(spans != NULL && spans[0].start == 0 && spans[0].end == 6 &&
          spans[0].type == REDIS_REPLY_STRING);

    redisReaderFree(r);
}

/* Offsets must stay absolute across the reader's internal buffer
 * compaction (which kicks in once 1k has been consumed). */
static void test_offsets_across_compaction(void) {
    redisReader *r = redisReaderCreateWithFunctions(NULL);
    const redisReplySpan *spans;
    size_t n, i;
    int ok = 1;

    printf("\n== absolute offsets across buffer compaction ==\n");
    redisReaderEnableSpans(r, 1);

    /* 300 replies of ":12345\r\n" (8 bytes each) = 2400 bytes, fed at
     * once and drained one reply per redisReaderGetReply() call. */
    for (i = 0; i < 300; i++)
        CHECK(redisReaderFeed(r, ":12345\r\n", 8) == REDIS_OK);
    drain(r);

    n = span_count(r);
    CHECK(n == 300);
    spans = redisReaderSpans(r, NULL);
    if (spans != NULL && n == 300) {
        for (i = 0; i < 300; i++) {
            if (spans[i].start != 8ull*i || spans[i].end != 8ull*i+8 ||
                spans[i].type != REDIS_REPLY_INTEGER) {
                ok = 0;
                break;
            }
        }
        printf("  span[0] = [%llu, %llu), span[%zu] = [%llu, %llu)\n",
               (unsigned long long)spans[0].start,
               (unsigned long long)spans[0].end, n - 1,
               (unsigned long long)spans[n-1].start,
               (unsigned long long)spans[n-1].end);
    } else {
        ok = 0;
    }
    CHECK(ok); /* every range is [8i, 8i+8), consecutive and absolute */

    redisReaderFree(r);
}

/* With tracking disabled (the default) nothing is recorded. */
static void test_spans_disabled(void) {
    redisReader *r = redisReaderCreateWithFunctions(NULL);
    size_t n = 12345;

    printf("\n== tracking disabled (default) ==\n");

    CHECK(redisReaderFeed(r, "+OK\r\n:1\r\n", 9) == REDIS_OK);
    CHECK(drain(r) == (void*)REDIS_REPLY_INTEGER);
    CHECK(redisReaderSpans(r, &n) == NULL && n == 0);

    print_spans("disabled", r);

    redisReaderFree(r);
}

int main(void) {
    test_two_replies_three_feeds();
    test_nested_push_then_reply();
    test_binary_bulk_crlf();
    test_error_span_and_reset();
    test_empty_bulk();
    test_spans_disabled();
    test_offsets_across_compaction();

    if (failures) {
        printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
