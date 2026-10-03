/* example-strictnumeric.c
 *
 * Demonstrates the optional strict numeric mode for the hiredis RESP
 * reader.  In strict mode, integer, double and length fields are parsed
 * with stricter validation:
 *
 *   - Integers must be a complete decimal number that does not overflow
 *     (no partial atoi-style acceptance, no trailing characters).
 *   - Doubles must be a finite decimal number or an explicit inf/-inf/nan
 *     literal; parsing is locale-independent and rejects trailing garbage
 *     and hidden whitespace.
 *   - Length fields (bulk strings and aggregate replies) must be complete
 *     decimal numbers that do not overflow, and the overflow check happens
 *     before any reply object is allocated.
 *
 * Malformed values are rejected with a protocol error whose message includes
 * the cumulative input byte offset of the offending field.
 *
 * This example is self-contained and does not require a running Redis
 * server.  It feeds RESP replies directly to the reader and prints the
 * parse results.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

#include <hiredis.h>

/* Feed a reply to the reader in one piece and print the result. */
static void feed_and_print(redisReader *r, const char *label, const char *data, size_t len) {
    void *reply = NULL;
    int rc;

    printf("--- %s ---\n", label);
    printf("input: %.*s\n", (int)len, data);

    rc = redisReaderFeed(r, data, len);
    if (rc != REDIS_OK) {
        printf("feed error: %s\n", r->errstr);
        return;
    }

    rc = redisReaderGetReply(r, &reply);
    if (rc != REDIS_OK) {
        printf("parse error: %s\n", r->errstr);
        return;
    }

    if (reply == NULL) {
        printf("incomplete reply (need more data)\n");
        return;
    }

    redisReply *rep = reply;
    switch (rep->type) {
    case REDIS_REPLY_INTEGER:
        printf("integer: %lld\n", rep->integer);
        break;
    case REDIS_REPLY_DOUBLE:
        printf("double: %g\n", rep->dval);
        break;
    case REDIS_REPLY_STRING:
        printf("string: \"%.*s\"\n", (int)rep->len, rep->str);
        break;
    case REDIS_REPLY_ARRAY:
        printf("array: %zu elements\n", rep->elements);
        for (size_t i = 0; i < rep->elements; i++) {
            redisReply *e = rep->element[i];
            if (e->type == REDIS_REPLY_INTEGER)
                printf("  [%zu] integer: %lld\n", i, e->integer);
            else if (e->type == REDIS_REPLY_DOUBLE)
                printf("  [%zu] double: %g\n", i, e->dval);
            else if (e->type == REDIS_REPLY_STRING)
                printf("  [%zu] string: \"%.*s\"\n", i, (int)e->len, e->str);
            else
                printf("  [%zu] type %d\n", i, e->type);
        }
        break;
    case REDIS_REPLY_NIL:
        printf("nil\n");
        break;
    case REDIS_REPLY_BOOL:
        printf("bool: %s\n", rep->integer ? "true" : "false");
        break;
    default:
        printf("reply type %d\n", rep->type);
        break;
    }

    freeReplyObject(reply);
    printf("\n");
}

/* Feed a reply one byte at a time to demonstrate that chunking does not
 * bypass strict validation. */
static void feed_chunked_and_print(redisReader *r, const char *label, const char *data, size_t len) {
    void *reply = NULL;
    int rc;

    printf("--- %s (chunked, one byte at a time) ---\n", label);
    printf("input: %.*s\n", (int)len, data);

    for (size_t i = 0; i < len; i++) {
        rc = redisReaderFeed(r, &data[i], 1);
        if (rc != REDIS_OK) {
            printf("feed error at byte %zu: %s\n", i, r->errstr);
            return;
        }
    }

    rc = redisReaderGetReply(r, &reply);
    if (rc != REDIS_OK) {
        printf("parse error: %s\n", r->errstr);
        return;
    }

    if (reply == NULL) {
        printf("incomplete reply (need more data)\n");
        return;
    }

    redisReply *rep = reply;
    if (rep->type == REDIS_REPLY_INTEGER)
        printf("integer: %lld\n", rep->integer);
    else if (rep->type == REDIS_REPLY_DOUBLE)
        printf("double: %g\n", rep->dval);
    else if (rep->type == REDIS_REPLY_STRING)
        printf("string: \"%.*s\"\n", (int)rep->len, rep->str);
    else
        printf("reply type %d\n", rep->type);

    freeReplyObject(reply);
    printf("\n");
}

int main(void) {
    redisReader *r;

    printf("=== Strict numeric mode demonstration ===\n\n");

    /* Create a reader and enable strict numeric mode. */
    r = redisReaderCreate();
    if (r == NULL) {
        fprintf(stderr, "Failed to create reader\n");
        return 1;
    }
    redisReaderSetStrictNumeric(r, 1);

    /* Valid integer. */
    feed_and_print(r, "valid integer", ":42\r\n", 5);

    /* Valid negative integer. */
    feed_and_print(r, "valid negative integer", ":-12345\r\n", 9);

    /* Valid double. */
    feed_and_print(r, "valid double", ",1.5\r\n", 6);

    /* Valid double with exponent. */
    feed_and_print(r, "valid double with exponent", ",1.5e3\r\n", 8);

    /* Valid inf literal. */
    feed_and_print(r, "valid inf", ",inf\r\n", 6);

    /* Valid -inf literal. */
    feed_and_print(r, "valid -inf", ",-inf\r\n", 7);

    /* Valid nan literal. */
    feed_and_print(r, "valid nan", ",nan\r\n", 6);

    /* Valid bulk string. */
    feed_and_print(r, "valid bulk string", "$5\r\nhello\r\n", 11);

    /* Valid null bulk string. */
    feed_and_print(r, "valid null bulk string", "$-1\r\n", 5);

    /* Valid array. */
    feed_and_print(r, "valid array", "*3\r\n:1\r\n:2\r\n:3\r\n", 16);

    /* Valid null array. */
    feed_and_print(r, "valid null array", "*-1\r\n", 5);

    /* Chunked integer with trailing junk - must be rejected. */
    feed_chunked_and_print(r, "chunked integer with trailing junk", ":12x\r\n", 6);

    /* Double with trailing junk - must be rejected. */
    feed_and_print(r, "double with trailing junk", ",1.5x\r\n", 7);

    /* Double with hidden whitespace - must be rejected. */
    feed_and_print(r, "double with hidden whitespace", ", 1.5\r\n", 7);

    /* Double with leading plus - must be rejected. */
    feed_and_print(r, "double with leading plus", ",+1.5\r\n", 7);

    /* Integer with leading zeros - must be rejected. */
    feed_and_print(r, "integer with leading zeros", ":007\r\n", 6);

    /* Empty integer - must be rejected. */
    feed_and_print(r, "empty integer", ":\r\n", 3);

    /* Empty double - must be rejected. */
    feed_and_print(r, "empty double", ",\r\n", 3);

    redisReaderFree(r);

    /* Demonstrate that default mode is unchanged. */
    printf("=== Default mode (strict disabled) ===\n\n");

    r = redisReaderCreate();
    if (r == NULL) {
        fprintf(stderr, "Failed to create reader\n");
        return 1;
    }

    /* In default mode, the same replies are accepted as before. */
    feed_and_print(r, "default mode integer", ":42\r\n", 5);
    feed_and_print(r, "default mode double", ",1.5\r\n", 6);
    feed_and_print(r, "default mode bulk string", "$5\r\nhello\r\n", 11);

    /* In default mode, trailing junk on a double is still rejected by the
     * existing strtod-based parser (no partial acceptance). */
    feed_and_print(r, "default mode double with trailing junk", ",1.5x\r\n", 7);

    redisReaderFree(r);

    /* Demonstrate locale independence: parse a double under a changed
     * locale and confirm the result is still 1.5. */
    printf("=== Locale independence demonstration ===\n\n");

    const char *locales[] = {
        "de_DE.UTF-8", "de-DE", "German_Germany.1252", "fr_FR.UTF-8", NULL
    };
    const char *saved_locale = setlocale(LC_NUMERIC, NULL);

    for (int i = 0; locales[i] != NULL; i++) {
        char *result = setlocale(LC_NUMERIC, locales[i]);
        if (result == NULL) {
            printf("locale %s: not available (skipped)\n", locales[i]);
            continue;
        }

        r = redisReaderCreate();
        redisReaderSetStrictNumeric(r, 1);

        void *reply = NULL;
        redisReaderFeed(r, ",1.5\r\n", 6);
        int rc = redisReaderGetReply(r, &reply);

        if (rc == REDIS_OK && reply != NULL) {
            redisReply *rep = reply;
            printf("locale %s: parsed double = %g (expected 1.5)\n",
                   locales[i], rep->dval);
            freeReplyObject(reply);
        } else {
            printf("locale %s: parse error: %s\n", locales[i], r->errstr);
        }

        redisReaderFree(r);
    }

    /* Restore the original locale. */
    setlocale(LC_NUMERIC, saved_locale);
    printf("\n");

    printf("=== Done ===\n");
    return 0;
}
