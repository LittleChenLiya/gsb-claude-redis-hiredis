/*
 * strict_numeric_demo.c -- Acceptance demo/tests for the hiredis reader's
 * optional strict numeric mode (see STRICT_NUMERIC.md).
 *
 * A single self-contained C program that feeds RESP2/RESP3 payloads to a
 * redisReader (in whole or in chunks), prints the parse result of every
 * case, and exits non-zero if any expectation fails.
 *
 * Build (from the repository root):
 *   gcc -std=c99 -Wall -Wextra -I. strict_numeric_demo.c read.c sds.c alloc.c -o strict_numeric_demo
 * Run:
 *   ./strict_numeric_demo
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <math.h>

#include "read.h"

/* Instrumented reply object functions: count allocations and capture the
 * values the parser produced so the demo can print them. */
static int alloc_calls;       /* createString + createArray invocations */
static int nil_calls;         /* createNil invocations                  */
static long long last_int;    /* last integer value parsed              */
static double last_double;    /* last double value parsed               */
static int double_calls;      /* createDouble invocations               */

static void *demo_createString(const redisReadTask *t, char *s, size_t len) {
    (void)t; (void)s; (void)len;
    alloc_calls++;
    return (void*)1; /* sentinel, never dereferenced */
}

static void *demo_createArray(const redisReadTask *t, size_t elements) {
    (void)t; (void)elements;
    alloc_calls++;
    return (void*)2;
}

static void *demo_createInteger(const redisReadTask *t, long long v) {
    (void)t;
    last_int = v;
    return (void*)3;
}

static void *demo_createDouble(const redisReadTask *t, double d, char *s, size_t len) {
    (void)t; (void)s; (void)len;
    last_double = d;
    double_calls++;
    return (void*)4;
}

static void *demo_createNil(const redisReadTask *t) {
    (void)t;
    nil_calls++;
    return (void*)5;
}

static void *demo_createBool(const redisReadTask *t, int b) {
    (void)t; (void)b;
    return (void*)6;
}

static void demo_freeObject(void *obj) {
    (void)obj; /* sentinels need no cleanup */
}

static redisReplyObjectFunctions demo_fn = {
    demo_createString,
    demo_createArray,
    demo_createInteger,
    demo_createDouble,
    demo_createNil,
    demo_createBool,
    demo_freeObject
};

static int tests_run, tests_failed;

static void report(int ok, const char *name, const char *detail) {
    tests_run++;
    if (!ok) tests_failed++;
    printf("[%s] %-58s %s\n", ok ? "PASS" : "FAIL", name, detail);
}

/* Feed `data` to `r` in chunks of `chunksz` bytes (0 = single shot), then
 * ask for a reply. Returns REDIS_OK/REDIS_ERR from redisReaderGetReply. */
static int feed_chunked(redisReader *r, const char *data, size_t len,
                        size_t chunksz, void **reply) {
    size_t off = 0;
    if (chunksz == 0) chunksz = len ? len : 1;
    while (off < len) {
        size_t n = len - off < chunksz ? len - off : chunksz;
        if (redisReaderFeed(r, data + off, n) != REDIS_OK)
            return REDIS_ERR;
        off += n;
    }
    return redisReaderGetReply(r, reply);
}

static redisReader *new_reader(int strict) {
    redisReader *r = redisReaderCreateWithFunctions(&demo_fn);
    redisReaderSetStrictNumeric(r, strict);
    return r;
}

/* Run a single payload (fed in chunks) against a fresh reader and return
 * the outcome: 0 = reply produced, 1 = protocol error, 2 = no reply yet. */
static int run_case(int strict, const char *data, size_t chunksz,
                    void **reply, char *errbuf, size_t errlen) {
    redisReader *r = new_reader(strict);
    int rc;
    *reply = NULL;
    rc = feed_chunked(r, data, strlen(data), chunksz, reply);
    if (rc != REDIS_OK) {
        snprintf(errbuf, errlen, "%s", r->errstr);
        redisReaderFree(r);
        return 1;
    }
    if (*reply == NULL) {
        errbuf[0] = '\0';
        redisReaderFree(r);
        return 2;
    }
    errbuf[0] = '\0';
    redisReaderFree(r);
    return 0;
}

int main(void) {
    char err[256];
    void *reply;
    int outcome;
    int nil_before, alloc_before, dbl_before;

    printf("hiredis strict numeric mode -- acceptance demo\n");
    printf("==============================================\n\n");

    /* 1. Chunked "12x" integer must be rejected: the field arrives split
     * across feeds; the parser must not accept a partial atoi-style "12"
     * and must flag the junk at the absolute stream offset. */
    outcome = run_case(1, ":12x\r\n", 2, &reply, err, sizeof(err));
    report(outcome == 1 && strstr(err, "byte 1") != NULL,
           "strict: chunked \":12x\" integer rejected", err);

    outcome = run_case(1, ":12x\r\n", 1, &reply, err, sizeof(err));
    report(outcome == 1, "strict: \":12x\" fed byte-by-byte rejected", err);

    /* 2. Valid negative integers pass. */
    outcome = run_case(1, ":-42\r\n", 0, &reply, err, sizeof(err));
    {
        char d[64];
        snprintf(d, sizeof(d), "value=%lld", last_int);
        report(outcome == 0 && last_int == -42,
               "strict: negative integer :-42 accepted", d);
    }

    /* Boundary chunking: sign, digits and CRLF in separate feeds. */
    outcome = run_case(1, ":-42\r\n", 1, &reply, err, sizeof(err));
    {
        char d[64];
        snprintf(d, sizeof(d), "value=%lld", last_int);
        report(outcome == 0 && last_int == -42,
               "strict: :-42 fed byte-by-byte accepted", d);
    }

    /* Integer overflow boundaries. */
    outcome = run_case(1, ":9223372036854775807\r\n", 0, &reply, err, sizeof(err));
    report(outcome == 0 && last_int == 9223372036854775807LL,
           "strict: LLONG_MAX accepted", "value=9223372036854775807");

    outcome = run_case(1, ":9223372036854775808\r\n", 3, &reply, err, sizeof(err));
    report(outcome == 1, "strict: LLONG_MAX+1 rejected (chunked)", err);

    outcome = run_case(1, ":-9223372036854775808\r\n", 0, &reply, err, sizeof(err));
    report(outcome == 0 && last_int == (-9223372036854775807LL - 1),
           "strict: LLONG_MIN accepted", "value=-9223372036854775808");

    /* 3. Doubles under a changed locale. */
    {
        const char *candidates[] = {
            "de_DE.UTF-8", "de-DE", "German_Germany.1252", "German"
        };
        const char *used = NULL;
        char decimal = '.';
        double after;
        size_t i;

        outcome = run_case(1, ",1.5\r\n", 0, &reply, err, sizeof(err));
        report(outcome == 0 && last_double == 1.5,
               "strict: \",1.5\" parses to 1.5 (C locale)", "value=1.5");

        for (i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++) {
            if (setlocale(LC_NUMERIC, candidates[i]) != NULL) {
                used = candidates[i];
                break;
            }
        }
        if (used != NULL && localeconv()->decimal_point != NULL)
            decimal = localeconv()->decimal_point[0];

        outcome = run_case(1, ",1.5\r\n", 0, &reply, err, sizeof(err));
        after = last_double;
        {
            char d[128];
            snprintf(d, sizeof(d), "locale=%s decimal_point='%c' value=%.17g",
                     used ? used : "(none available)", decimal, after);
            report(used != NULL && decimal == ',' && outcome == 0 && after == 1.5,
                   "strict: \",1.5\" identical under comma locale", d);
        }

        /* A comma decimal separator must NOT be accepted, even when the
         * active locale uses one. */
        outcome = run_case(1, ",1,5\r\n", 0, &reply, err, sizeof(err));
        report(outcome == 1,
               "strict: \",1,5\" rejected under comma locale", err);

        setlocale(LC_NUMERIC, "C");
    }

    /* 4. Length overflow must not trigger allocation. */
    alloc_before = alloc_calls;
    outcome = run_case(1, "$99999999999999999999999999\r\n", 4, &reply, err, sizeof(err));
    report(outcome == 1 && alloc_calls == alloc_before,
           "strict: bulk length decimal overflow, no allocation", err);

    alloc_before = alloc_calls;
    outcome = run_case(1, "*99999999999999999999999999\r\n", 0, &reply, err, sizeof(err));
    report(outcome == 1 && alloc_calls == alloc_before,
           "strict: multi-bulk length overflow, no allocation", err);

    /* An extreme but representable bulk length: the parser must wait for
     * data instead of allocating. */
    alloc_before = alloc_calls;
    outcome = run_case(1, "$9223372036854775807\r\n", 0, &reply, err, sizeof(err));
    report(outcome == 2 && alloc_calls == alloc_before,
           "strict: LLONG_MAX bulk length waits, no allocation",
           "no reply, no createString/createArray call");

    /* 5. Null lengths. */
    nil_before = nil_calls;
    outcome = run_case(1, "$-1\r\n", 0, &reply, err, sizeof(err));
    report(outcome == 0 && nil_calls == nil_before + 1,
           "strict: $-1 null bulk string accepted", "reply=NIL");

    nil_before = nil_calls;
    outcome = run_case(1, "*-1\r\n", 2, &reply, err, sizeof(err));
    report(outcome == 0 && nil_calls == nil_before + 1,
           "strict: *-1 null array accepted (chunked)", "reply=NIL");

    /* 6. inf / nan literals and finite-double strictness. */
    dbl_before = double_calls;
    outcome = run_case(1, ",inf\r\n", 0, &reply, err, sizeof(err));
    report(outcome == 0 && double_calls == dbl_before + 1 &&
           isinf(last_double) && last_double > 0,
           "strict: \",inf\" accepted", "value=+inf");

    outcome = run_case(1, ",-inf\r\n", 1, &reply, err, sizeof(err));
    report(outcome == 0 && isinf(last_double) && last_double < 0,
           "strict: \",-inf\" accepted (chunked)", "value=-inf");

    outcome = run_case(1, ",nan\r\n", 0, &reply, err, sizeof(err));
    report(outcome == 0 && isnan(last_double),
           "strict: \",nan\" accepted", "value=nan");

    outcome = run_case(1, ",infinity\r\n", 0, &reply, err, sizeof(err));
    report(outcome == 1, "strict: \",infinity\" rejected", err);

    outcome = run_case(1, ",1.5 \r\n", 0, &reply, err, sizeof(err));
    report(outcome == 1, "strict: trailing space in double rejected", err);

    outcome = run_case(1, ", 1.5\r\n", 0, &reply, err, sizeof(err));
    report(outcome == 1, "strict: leading space in double rejected", err);

    outcome = run_case(1, ",0x1p3\r\n", 0, &reply, err, sizeof(err));
    report(outcome == 1, "strict: hex float rejected", err);

    outcome = run_case(1, ",+1.5\r\n", 0, &reply, err, sizeof(err));
    report(outcome == 1, "strict: leading '+' double rejected", err);

    /* Boundary chunking of a double: split inside the fraction. */
    outcome = run_case(1, ",1.5\r\n", 3, &reply, err, sizeof(err));
    report(outcome == 0 && last_double == 1.5,
           "strict: \",1.5\" split across 3-byte chunks", "value=1.5");

    /* 7. Error offsets count cumulative input bytes, even after the
     * reader trims its internal buffer. */
    {
        redisReader *r = new_reader(1);
        int i, rc = REDIS_OK, drained = 0;
        void *rp = NULL;

        /* Push 300 small replies (1200 bytes) through the reader so the
         * internal buffer gets trimmed at least once, then a bad integer.
         * The error offset must count all bytes fed so far. */
        for (i = 0; i < 300; i++)
            redisReaderFeed(r, ":1\r\n", 4);
        do {
            rp = NULL;
            rc = redisReaderGetReply(r, &rp);
            if (rp != NULL) drained++;
        } while (rc == REDIS_OK && rp != NULL);
        redisReaderFeed(r, ":12x\r\n", 6);
        rp = NULL;
        rc = redisReaderGetReply(r, &rp);
        {
            char d[160];
            snprintf(d, sizeof(d), "drained=%d err=\"%s\"", drained, r->errstr);
            report(rc == REDIS_ERR && drained == 300 &&
                   strstr(r->errstr, "byte 1201") != NULL,
                   "strict: error offset counts cumulative stream bytes", d);
        }
        redisReaderFree(r);
    }

    /* 8. Default mode is untouched. */
    {
        redisReader *r = redisReaderCreateWithFunctions(&demo_fn);
        report(r->strict_numeric == 0,
               "default: strict numeric mode is off", "strict_numeric=0");
        redisReaderFree(r);
    }

    outcome = run_case(0, ":42\r\n", 0, &reply, err, sizeof(err));
    report(outcome == 0 && last_int == 42,
           "default: plain integer still parses", "value=42");

    outcome = run_case(0, ",1.5\r\n", 0, &reply, err, sizeof(err));
    report(outcome == 0 && last_double == 1.5,
           "default: plain double still parses", "value=1.5");

    outcome = run_case(0, ":12x\r\n", 0, &reply, err, sizeof(err));
    report(outcome == 1 && strstr(err, "byte") == NULL,
           "default: bad integer error keeps classic format", err);

    printf("\n%d checks run, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
