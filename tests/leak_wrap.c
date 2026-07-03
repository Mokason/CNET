/* Link-time allocation-balance gate (the no-ASan toolchain's behavioral
 * substitute): build a gate with
 *   -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
 * and this file; every allocation from OUR objects is counted, every free
 * decounted, and the balance prints at exit. Balance 0 = no leaked
 * allocations on the gate's paths. CRT-internal allocations bypass the wrap
 * (link-time interposition only), so the number is ours alone. */

#include <stdio.h>
#include <stdlib.h>

void *__real_malloc(size_t n);
void *__real_calloc(size_t a, size_t b);
void *__real_realloc(void *q, size_t n);
void __real_free(void *p);

static long g_balance = 0;
static long g_allocs = 0;
static int g_armed = 0;

/* live-pointer table so survivors can be fingerprinted by size */
#define LEAK_TABLE 65536
static void *g_live[LEAK_TABLE];
static size_t g_live_sz[LEAK_TABLE];

static void live_add(void *p, size_t n) {
    size_t i;
    for (i = 0; i < LEAK_TABLE; ++i)
        if (!g_live[i]) { g_live[i] = p; g_live_sz[i] = n; return; }
}

static void live_del(void *p) {
    size_t i;
    for (i = 0; i < LEAK_TABLE; ++i)
        if (g_live[i] == p) { g_live[i] = NULL; return; }
}

/* CRT startup (argv/env setup) allocates through the wrap too and frees only
   after atexit handlers — snapshot that baseline before main and judge the
   program against the DELTA. */
static long g_baseline = 0;

__attribute__((constructor)) static void leak_baseline(void) {
    g_baseline = g_balance;
}

static void leak_report(void) {
    size_t i;
    long delta = g_balance - g_baseline;
    fprintf(stderr, "[leakcheck] allocations: %ld, balance at exit: %ld "
                    "(crt baseline %ld) -> program delta %ld%s\n",
            g_allocs, g_balance, g_baseline, delta,
            delta == 0 ? " (clean)" : " (LEAK)");
    if (delta == 0) return;
    for (i = 0; i < LEAK_TABLE; ++i) {
        if (g_live[i]) {
            const unsigned char *b = (const unsigned char *)g_live[i];
            size_t j, n = g_live_sz[i] < 24 ? g_live_sz[i] : 24;
            fprintf(stderr, "[leakcheck]   survivor: %lu bytes  \"",
                    (unsigned long)g_live_sz[i]);
            for (j = 0; j < n; ++j)
                fputc(b[j] >= 32 && b[j] < 127 ? b[j] : '.', stderr);
            fprintf(stderr, "\"  [");
            for (j = 0; j < n && j < 8; ++j) fprintf(stderr, "%02x ", b[j]);
            fprintf(stderr, "]\n");
        }
    }
}

static void arm(void) {
    if (!g_armed) {
        g_armed = 1;
        atexit(leak_report);
    }
}

void *__wrap_malloc(size_t n) {
    void *p = __real_malloc(n);
    arm();
    if (p) { g_balance++; g_allocs++; live_add(p, n); }
    return p;
}

void *__wrap_calloc(size_t a, size_t b) {
    void *p = __real_calloc(a, b);
    arm();
    if (p) { g_balance++; g_allocs++; live_add(p, a * b); }
    return p;
}

void *__wrap_realloc(void *q, size_t n) {
    void *p = __real_realloc(q, n);
    arm();
    if (p && !q) { g_balance++; g_allocs++; }
    if (p) { if (q) live_del(q); live_add(p, n); }
    return p;
}

void __wrap_free(void *p) {
    if (p) { g_balance--; live_del(p); }
    __real_free(p);
}
