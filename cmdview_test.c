/* cmdview_test.c - deterministic acceptance tests for the redisCmdView API. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include <hiredis.h>
#include <cmdview.h>
#include <alloc.h>

static int failures = 0;
static int checks = 0;

#define CHECK(cond, msg) do { \
    checks++; \
    if (!(cond)) { \
        printf("  FAIL: %s\n", msg); \
        failures++; \
    } else { \
        printf("  ok: %s\n", msg); \
    } \
} while (0)

/* ---- counting allocator ------------------------------------------------- */
static size_t g_live;
static size_t g_total;
static size_t g_frees;
static int g_fail_at;

static void *c_malloc(size_t sz) {
    if (g_fail_at > 0 && g_total >= (size_t)(g_fail_at - 1)) return NULL;
    void *p = malloc(sz);
    if (p) { g_total++; g_live++; }
    return p;
}
static void *c_calloc(size_t n, size_t sz) {
    if (g_fail_at > 0 && g_total >= (size_t)(g_fail_at - 1)) return NULL;
    void *p = calloc(n, sz);
    if (p) { g_total++; g_live++; }
    return p;
}
static void *c_realloc(void *p, size_t sz) {
    if (g_fail_at > 0 && g_total >= (size_t)(g_fail_at - 1)) return NULL;
    void *np = realloc(p, sz);
    if (np && p == NULL) { g_total++; g_live++; }
    return np;
}
static char *c_strdup(const char *s) {
    if (g_fail_at > 0 && g_total >= (size_t)(g_fail_at - 1)) return NULL;
    char *p = strdup(s);
    if (p) { g_total++; g_live++; }
    return p;
}
static void c_free(void *p) {
    if (p) { g_live--; g_frees++; free(p); }
}

static void alloc_reset(void) {
    g_live = 0; g_total = 0; g_frees = 0; g_fail_at = 0;
}

static void alloc_install(void) {
    hiredisAllocFuncs f = { c_malloc, c_calloc, c_realloc, c_strdup, c_free };
    hiredisSetAllocators(&f);
}

/* ---- helpers ------------------------------------------------------------ */

static char *view_concat(redisCmdView *v, size_t *outlen) {
    size_t cap = redisCmdViewLen(v);
    char *buf = malloc(cap ? cap : 1);
    size_t pos = 0;
    redisCmdViewSeg seg;
    if (!buf) return NULL;
    redisCmdViewRewind(v);
    while (redisCmdViewNext(v, &seg)) {
        memcpy(buf + pos, seg.data, seg.len);
        pos += seg.len;
    }
    *outlen = pos;
    return buf;
}

static redisCmdViewSeg *view_snapshot(redisCmdView *v, size_t *n) {
    size_t cnt = redisCmdViewSegments(v);
    redisCmdViewSeg *snap = malloc(cnt * sizeof(*snap));
    size_t i = 0;
    redisCmdViewSeg seg;
    if (!snap) return NULL;
    redisCmdViewRewind(v);
    while (redisCmdViewNext(v, &seg))
        snap[i++] = seg;
    *n = i;
    return snap;
}

static void print_segments(redisCmdView *v) {
    redisCmdViewSeg seg;
    size_t idx = 0, off = 0;
    redisCmdViewRewind(v);
    printf("  segments (idx, offset, len, bytes):\n");
    while (redisCmdViewNext(v, &seg)) {
        size_t k;
        printf("    [%2zu] off=%-3zu len=%-3zu  ", idx, off, seg.len);
        for (k = 0; k < seg.len && k < 24; k++) {
            unsigned char b = (unsigned char)seg.data[k];
            if (b >= 0x20 && b < 0x7f) putchar((char)b);
            else printf("<%02x>", b);
        }
        if (seg.len > 24) printf("...");
        printf("\n");
        off += seg.len;
        idx++;
    }
}

int main(void) {
    const char *argv[] = { "GET", "a\0b", "", "x", "\xff\xfe\x80" };
    size_t argvlen[] = { 3, 3, 0, 1, 3 };
    int argc = 5;

    alloc_install();

    printf("[1] binary safety + exact match with redisFormatCommandArgv\n");
    alloc_reset();
    redisCmdView *view = NULL;
    int rc = redisCmdViewCreate(&view, argc, argv, argvlen);
    CHECK(rc == REDIS_OK && view != NULL, "create succeeds");
    CHECK(redisCmdViewArgc(view) == (size_t)argc, "argc matches");
    CHECK(redisCmdViewSegments(view) == (size_t)(1 + 3 * argc), "segment count is 1 + 3*argc");
    CHECK(redisCmdViewLen(view) == 44, "wire length is 44");
    print_segments(view);

    char *vbytes = NULL; size_t vlen = 0;
    if (view) vbytes = view_concat(view, &vlen);

    char *abytes = NULL;
    long alen = redisFormatCommandArgv(&abytes, argc, argv, argvlen);
    CHECK(alen >= 0 && abytes != NULL, "redisFormatCommandArgv still works");
    CHECK(vbytes != NULL && vlen == (size_t)alen, "concatenated length equals Argv length");
    if (vbytes && abytes && vlen == (size_t)alen)
        CHECK(memcmp(vbytes, abytes, vlen) == 0, "concatenated bytes are identical to Argv encoding");
    free(abytes);
    free(vbytes);

    printf("[2] repeated iteration is identical\n");
    size_t n1 = 0, n2 = 0;
    redisCmdViewSeg *s1 = view_snapshot(view, &n1);
    redisCmdViewSeg *s2 = view_snapshot(view, &n2);
    CHECK(s1 && s2 && n1 == n2 && n1 == redisCmdViewSegments(view), "two passes produce same segment count");
    int same = 1;
    for (size_t i = 0; i < n1; i++) {
        if (s1[i].len != s2[i].len) { same = 0; break; }
        if (s1[i].data != s2[i].data) { same = 0; break; }
        if (s1[i].len && memcmp(s1[i].data, s2[i].data, s1[i].len) != 0) { same = 0; break; }
    }
    CHECK(same, "re-iterating gives identical segments (no re-encode)");
    redisCmdViewRewind(view);
    redisCmdViewSeg seg;
    CHECK(redisCmdViewNext(view, &seg) == 1 && seg.len == 4, "rewind then next works");
    free(s1);
    free(s2);

    printf("[3] empty parameter group (argc == 0)\n");
    redisCmdView *eview = NULL;
    rc = redisCmdViewCreate(&eview, 0, NULL, NULL);
    CHECK(rc == REDIS_OK && eview != NULL, "empty group create succeeds");
    CHECK(redisCmdViewArgc(eview) == 0, "empty group argc is 0");
    CHECK(redisCmdViewSegments(eview) == 1, "empty group has 1 segment");
    CHECK(redisCmdViewLen(eview) == 4, "empty group wire length is 4 (*0 CRLF)");
    if (eview) {
        char *eb = NULL; size_t el = 0;
        eb = view_concat(eview, &el);
        CHECK(eb && el == 4 && memcmp(eb, "*0\r\n", 4) == 0, "empty group encodes *0 CRLF");
        free(eb);
        redisCmdViewFree(eview);
    }

    printf("[4] oversized length rejected before any allocation\n");
    alloc_reset();
    const char *big_argv[1] = { "x" };
    size_t big_len[1] = { SIZE_MAX };
    redisCmdView *bview = (redisCmdView *)0x1;
    rc = redisCmdViewCreate(&bview, 1, big_argv, big_len);
    CHECK(rc == REDIS_ERR, "oversized length returns REDIS_ERR");
    CHECK(bview == NULL, "oversized create leaves *out untouched (NULL)");
    CHECK(g_total == 0, "no allocation attempted before rejection");
    CHECK(g_live == 0, "no live allocations after rejection");

    printf("[5] allocation failure injection (no leak, no half view)\n");
    for (int fail_at = 1; fail_at <= 3; fail_at++) {
        alloc_reset();
        g_fail_at = fail_at;
        redisCmdView *fview = (redisCmdView *)0x1;
        rc = redisCmdViewCreate(&fview, argc, argv, argvlen);
        char msg[96];
        snprintf(msg, sizeof(msg), "fail_at=%d returns REDIS_ERR", fail_at);
        CHECK(rc == REDIS_ERR, msg);
        snprintf(msg, sizeof(msg), "fail_at=%d leaves *out NULL", fail_at);
        CHECK(fview == NULL, msg);
        snprintf(msg, sizeof(msg), "fail_at=%d frees every allocation (no leak)", fail_at);
        CHECK(g_live == 0, msg);
    }

    printf("[6] free ownership: borrowed arguments survive the view\n");
    alloc_reset();
    redisCmdView *oview = NULL;
    rc = redisCmdViewCreate(&oview, argc, argv, argvlen);
    CHECK(rc == REDIS_OK, "ownership-test create succeeds");
    const char *arg_snapshot[5];
    size_t arg_lens[5];
    for (int i = 0; i < argc; i++) {
        arg_snapshot[i] = argv[i];
        arg_lens[i] = argvlen[i];
    }
    redisCmdViewFree(oview);
    CHECK(g_live == 0, "storage owned by the view is freed");
    int args_ok = 1;
    for (int i = 0; i < argc; i++) {
        if (memcmp(argv[i], arg_snapshot[i], arg_lens[i]) != 0) args_ok = 0;
    }
    CHECK(args_ok, "borrowed argument buffers are untouched after free");

    redisCmdViewFree(view);
    alloc_reset();
    hiredisResetAllocators();

    printf("\n%d/%d checks passed\n", checks - failures, checks);
    if (failures) {
        printf("%d CHECKS FAILED\n", failures);
        return 1;
    }
    printf("ALL CHECKS PASSED\n");
    return 0;
}
