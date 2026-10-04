/* example-cmdview.c - short demo of the zero-copy command view API.
 *
 * This demo does NOT need a running Redis server: it only builds a view over
 * a command and prints the ordered RESP segments, showing that the argument
 * bytes are borrowed (not copied) while the small protocol headers are owned
 * by the view.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <hiredis.h>
#include <cmdview.h>

static void print_segment(const redisCmdViewSeg *seg) {
    const unsigned char *p = (const unsigned char *)seg->data;
    size_t i;
    for (i = 0; i < seg->len; i++) {
        unsigned char b = p[i];
        if (b >= 0x20 && b < 0x7f)
            putchar((char)b);
        else
            printf("[%02x]", b);
    }
}

int main(void) {
    /* A command with a NUL-containing argument and an empty byte string. */
    const char *argv[] = { "SET", "key", "a\0b", "", "\xff\xfe" };
    size_t argvlen[] = { 3, 3, 3, 0, 2 };
    int argc = 5;

    redisCmdView *view = NULL;
    redisCmdViewSeg seg;
    size_t total = 0, idx = 0;

    if (redisCmdViewCreate(&view, argc, argv, argvlen) != REDIS_OK) {
        fprintf(stderr, "failed to create view\n");
        return 1;
    }

    printf("argc=%zu  segments=%zu  wire_len=%zu\n",
           redisCmdViewArgc(view), redisCmdViewSegments(view),
           redisCmdViewLen(view));

    /* Iterate in order; the same segments are produced on every pass. */
    redisCmdViewRewind(view);
    while (redisCmdViewNext(view, &seg)) {
        printf("  seg[%zu] off=%-3zu len=%-3zu  ", idx, total, seg.len);
        print_segment(&seg);
        printf("\n");
        total += seg.len;
        idx++;
    }
    printf("concatenated %zu bytes across %zu segments\n", total, idx);

    /* Freeing the view releases only its owned header storage; the borrowed
     * argument buffers remain valid and are not freed. */
    redisCmdViewFree(view);

    return 0;
}
