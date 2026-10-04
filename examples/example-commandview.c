/*
 * Short demo of the zero-copy command view API.
 *
 * Instead of copying every argument into one contiguous buffer (like
 * redisFormatCommandArgv does), a redisCommandView encodes the RESP framing
 * once and hands back ordered segments that borrow the original argument
 * buffers.  The segments can be fed straight to a scatter/gather write.
 *
 * This example is standalone: it encodes a command with binary-safe
 * arguments, prints each segment, and verifies that the concatenated
 * segments are byte-identical to redisFormatCommandArgv's output.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hiredis.h>

int main(void) {
    /* Binary-safe arguments: embedded NUL, empty string, non-UTF-8 bytes. */
    const char *argv[4];
    size_t argvlen[4];
    argv[0] = "SET";           argvlen[0] = 3;
    argv[1] = "a\0b";          argvlen[1] = 3;
    argv[2] = "";              argvlen[2] = 0;
    argv[3] = "\xff\xfe\x00z"; argvlen[3] = 4;

    redisCommandView *view = redisCommandViewCreateArgv(4, argv, argvlen);
    if (view == NULL) {
        fprintf(stderr, "failed to create command view\n");
        return 1;
    }

    printf("segments: %zu, total encoded length: %zu bytes\n",
           redisCommandViewSegmentCount(view),
           redisCommandViewTotalLen(view));

    /* Reference encoding for comparison. */
    char *cmd = NULL;
    long long cmdlen = redisFormatCommandArgv(&cmd, 4, argv, argvlen);

    /* Walk the segments in wire order.  With a live connection each segment
     * would be one entry of a writev(2) scatter/gather array. */
    char *joined = malloc(redisCommandViewTotalLen(view));
    size_t off = 0, idx = 0;
    redisCommandViewSegment seg;
    while (redisCommandViewNext(view, &seg) == 0) {
        printf("  segment %zu: %zu bytes\n", idx++, seg.len);
        memcpy(joined + off, seg.ptr, seg.len);
        off += seg.len;
    }

    if (off == (size_t)cmdlen && memcmp(joined, cmd, cmdlen) == 0)
        printf("OK: segments reassemble to the redisFormatCommandArgv encoding\n");
    else
        printf("MISMATCH: view encoding differs\n");

    /* Rewinding never re-encodes; iteration can be repeated at will. */
    redisCommandViewRewind(view);

    /* Frees the view and its owned RESP framing only.  The borrowed argv[]
     * buffers belong to the caller and are left untouched. */
    redisCommandViewFree(view);

    free(joined);
    redisFreeCommand(cmd);
    return 0;
}
