/*
 * source_demo.c - Demonstrate hiredis's optional reply source reporting.
 *
 * When debugging chunked RESP it is useful to know which byte range of the
 * cumulative input each top-level reply consumed. This program feeds a few
 * hand-written RESP streams split across several calls to redisReaderFeed()
 * and prints the [start,end) range (and type) that each completed top-level
 * reply consumed.
 *
 * The program doubles as a deterministic test fixture: it asserts the
 * expected ranges and exits non-zero on failure. It only links against the
 * static hiredis reader; no Redis server is required.
 *
 * Build (from the repository root):
 *   cc -std=c99 -I. -o source_demo source_demo.c libhiredis.a
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "hiredis.h"

/* Feed a chunk; the demo expects feeding to succeed. */
static void feed(redisReader *r, const char *buf, size_t len) {
    int rc = redisReaderFeed(r, buf, len);
    assert(rc == REDIS_OK);
}

int main(void) {
    redisReader *r;
    void *reply;
    redisReplySource src;
    int rc;

    setbuf(stdout, NULL);

    printf("== Two replies across 3 feeds (consecutive ranges) ==\n");
    r = redisReaderCreate();
    redisReaderSetSourceReport(r, 1);
    feed(r, "+OK\r", 4);                  /* feed 1: partial first reply */
    rc = redisReaderGetReply(r, &reply);
    assert(rc == REDIS_OK && reply == NULL); /* partial: no report yet */
    feed(r, "\n:42\r\n", 6);              /* feed 2: finish +OK, then :42 */
    rc = redisReaderGetReply(r, &reply);
    assert(rc == REDIS_OK && reply != NULL);
    rc = redisReaderGetReplySource(r, &src);
    assert(rc == REDIS_OK);
    printf("  +OK  type=%d range=[%zu,%zu) len=%zu\n",
           src.type, src.start, src.end, src.end - src.start);
    assert(src.start == 0 && src.end == 5);
    rc = redisReaderGetReply(r, &reply);  /* second reply, no new feed */
    assert(rc == REDIS_OK && reply != NULL);
    rc = redisReaderGetReplySource(r, &src);
    assert(rc == REDIS_OK);
    printf("  :42  type=%d range=[%zu,%zu) len=%zu\n",
           src.type, src.start, src.end, src.end - src.start);
    assert(src.start == 5 && src.end == 10);
    feed(r, "-ERR boom\r\n", 11);         /* feed 3: third reply */
    rc = redisReaderGetReply(r, &reply);
    assert(rc == REDIS_OK && reply != NULL);
    rc = redisReaderGetReplySource(r, &src);
    assert(rc == REDIS_OK);
    printf("  -ERR type=%d range=[%zu,%zu) len=%zu\n",
           src.type, src.start, src.end, src.end - src.start);
    assert(src.start == 10 && src.end == 21);
    redisReaderFree(r);

    printf("== Nested array: still one top-level range ==\n");
    r = redisReaderCreate();
    redisReaderSetSourceReport(r, 1);
    feed(r, "*2\r\n$5\r\nhello\r\n:7\r\n", 19);
    rc = redisReaderGetReply(r, &reply);
    assert(rc == REDIS_OK && reply != NULL);
    rc = redisReaderGetReplySource(r, &src);
    assert(rc == REDIS_OK);
    printf("  array type=%d range=[%zu,%zu) len=%zu\n",
           src.type, src.start, src.end, src.end - src.start);
    assert(src.start == 0 && src.end == 19 && src.type == REDIS_REPLY_ARRAY);
    redisReaderFree(r);

    printf("== Push reply is a separate top-level report ==\n");
    r = redisReaderCreate();
    redisReaderSetSourceReport(r, 1);
    feed(r, ">2\r\n$6\r\nLOLWUT\r\n:42\r\n", 21);
    rc = redisReaderGetReply(r, &reply);
    assert(rc == REDIS_OK && reply != NULL);
    rc = redisReaderGetReplySource(r, &src);
    assert(rc == REDIS_OK);
    printf("  push type=%d range=[%zu,%zu) len=%zu\n",
           src.type, src.start, src.end, src.end - src.start);
    assert(src.start == 0 && src.end == 21 && src.type == REDIS_REPLY_PUSH);
    redisReaderFree(r);

    printf("== Binary bulk: newlines are content, not a boundary ==\n");
    r = redisReaderCreate();
    redisReaderSetSourceReport(r, 1);
    /* content is 8 bytes: a \n b \r \n c \r \n ; the embedded \r\n must not
     * be treated as a boundary. Wire: $8\r\n <8 bytes> \r\n */
    feed(r, "$8\r\na\nb\r\nc\r\n\r\n", 14);
    rc = redisReaderGetReply(r, &reply);
    assert(rc == REDIS_OK && reply != NULL);
    rc = redisReaderGetReplySource(r, &src);
    assert(rc == REDIS_OK);
    printf("  bulk type=%d range=[%zu,%zu) len=%zu\n",
           src.type, src.start, src.end, src.end - src.start);
    assert(src.start == 0 && src.end == 14 && src.type == REDIS_REPLY_STRING);
    redisReaderFree(r);

    printf("== Empty bulk ==\n");
    r = redisReaderCreate();
    redisReaderSetSourceReport(r, 1);
    feed(r, "$0\r\n\r\n", 6);
    rc = redisReaderGetReply(r, &reply);
    assert(rc == REDIS_OK && reply != NULL);
    rc = redisReaderGetReplySource(r, &src);
    assert(rc == REDIS_OK);
    printf("  empty bulk type=%d range=[%zu,%zu) len=%zu\n",
           src.type, src.start, src.end, src.end - src.start);
    assert(src.start == 0 && src.end == 6 && src.type == REDIS_REPLY_STRING);
    redisReaderFree(r);

    printf("== Protocol error gives an error range ==\n");
    r = redisReaderCreate();
    redisReaderSetSourceReport(r, 1);
    feed(r, "@bad\r\n", 6);
    rc = redisReaderGetReply(r, &reply);
    assert(rc == REDIS_ERR);
    rc = redisReaderGetReplySource(r, &src);
    assert(rc == REDIS_OK);
    printf("  error type=%d range=[%zu,%zu) len=%zu errstr=%s\n",
           src.type, src.start, src.end, src.end - src.start, r->errstr);
    assert(src.start == 0 && src.end == 1 && src.type == 0);
    redisReaderFree(r);

    printf("== Disabled by default; can be turned off ==\n");
    r = redisReaderCreate();
    /* reporting is off by default */
    feed(r, "+OK\r\n", 5);
    rc = redisReaderGetReply(r, &reply);
    assert(rc == REDIS_OK && reply != NULL);
    rc = redisReaderGetReplySource(r, &src);
    assert(rc == REDIS_ERR); /* no source when disabled */
    printf("  default: no source (as expected)\n");
    /* enable, then disable again */
    redisReaderSetSourceReport(r, 1);
    feed(r, ":1\r\n", 4);
    rc = redisReaderGetReply(r, &reply);
    assert(rc == REDIS_OK && reply != NULL);
    rc = redisReaderGetReplySource(r, &src);
    assert(rc == REDIS_OK && src.start == 5 && src.end == 9);
    redisReaderSetSourceReport(r, 0);
    feed(r, ":2\r\n", 4);
    rc = redisReaderGetReply(r, &reply);
    assert(rc == REDIS_OK && reply != NULL);
    rc = redisReaderGetReplySource(r, &src);
    assert(rc == REDIS_ERR); /* disabled again */
    printf("  re-disabled: no source (as expected)\n");
    redisReaderFree(r);

    printf("== Reset restarts the cumulative offset at 0 ==\n");
    r = redisReaderCreate();
    redisReaderSetSourceReport(r, 1);
    feed(r, "+OK\r\n", 5);
    rc = redisReaderGetReply(r, &reply);
    assert(rc == REDIS_OK && reply != NULL);
    rc = redisReaderGetReplySource(r, &src);
    assert(rc == REDIS_OK && src.start == 0 && src.end == 5);
    redisReaderReset(r);
    feed(r, ":9\r\n", 4);
    rc = redisReaderGetReply(r, &reply);
    assert(rc == REDIS_OK && reply != NULL);
    rc = redisReaderGetReplySource(r, &src);
    assert(rc == REDIS_OK);
    printf("  after reset type=%d range=[%zu,%zu) len=%zu\n",
           src.type, src.start, src.end, src.end - src.start);
    assert(src.start == 0 && src.end == 4); /* new session starts at 0 */
    redisReaderFree(r);

    printf("== Compaction keeps cumulative offsets correct ==\n");
    {
        /* A 2000-byte bulk string forces the pos>=1024 buffer compaction in
         * redisReaderGetReply; the reported range must still be cumulative. */
        enum { BIGLEN = 2000 };
        char *big = malloc(16 + BIGLEN + 2); /* header + content + \r\n */
        assert(big != NULL);
        int hdr = snprintf(big, 16 + BIGLEN + 2, "$%d\r\n", BIGLEN);
        assert(hdr > 0 && hdr < 16);
        memset(big + hdr, 'x', BIGLEN);
        big[hdr + BIGLEN] = '\r';
        big[hdr + BIGLEN + 1] = '\n';

        r = redisReaderCreate();
        redisReaderSetSourceReport(r, 1);
        feed(r, big, hdr + BIGLEN + 2);
        rc = redisReaderGetReply(r, &reply);
        assert(rc == REDIS_OK && reply != NULL);
        rc = redisReaderGetReplySource(r, &src);
        assert(rc == REDIS_OK);
        printf("  big bulk type=%d range=[%zu,%zu) len=%zu\n",
               src.type, src.start, src.end, src.end - src.start);
        assert(src.start == 0 && src.end == (size_t)(hdr + BIGLEN + 2));

        /* A following reply must start right after the big one. */
        feed(r, ":1\r\n", 4);
        rc = redisReaderGetReply(r, &reply);
        assert(rc == REDIS_OK && reply != NULL);
        rc = redisReaderGetReplySource(r, &src);
        assert(rc == REDIS_OK);
        printf("  after big: type=%d range=[%zu,%zu) len=%zu\n",
               src.type, src.start, src.end, src.end - src.start);
        assert(src.start == (size_t)(hdr + BIGLEN + 2) &&
               src.end == (size_t)(hdr + BIGLEN + 2) + 4);
        redisReaderFree(r);
        free(big);
    }

    printf("\nAll source-range checks passed.\n");
    return 0;
}
