#include "../include/cce/cce_sparse_kv.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond, desc) do { \
    if (cond) { printf("  ok   %s\n", (desc)); } \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

static int has_index(const int* xs, int n, int needle) {
    for (int i = 0; i < n; ++i) if (xs[i] == needle) return 1;
    return 0;
}

static void test_full_budget_returns_all(void) {
    float scores[5] = {0.1f, 0.4f, 0.2f, 0.9f, 0.3f};
    int out[5] = {0};
    int n = 0;
    cce_specialist_kv_budget b = {0};
    b.max_tokens = 5;

    CHECK(cce_specialist_select_kv_tokens(scores, 5, &b, out, 5, &n) == CCE_OK,
          "full budget selection succeeds");
    CHECK(n == 5, "full budget keeps every token");
    CHECK(out[0] == 0 && out[1] == 1 && out[2] == 2 && out[3] == 3 && out[4] == 4,
          "full budget preserves causal order");
}

static void test_sparse_policy_keeps_anchors_recent_and_heavy(void) {
    float scores[10] = {0.0f, 0.1f, 0.2f, 0.3f, 0.01f, 0.8f, 0.4f, 0.95f, 0.2f, 0.1f};
    int out[10] = {0};
    int n = 0;
    cce_specialist_kv_budget b = {0};
    b.max_tokens = 6;
    b.initial_tokens = 2;
    b.recent_tokens = 2;
    b.long_range_stride = 4;
    b.heavy_hitter_fraction = 1.0f;

    CHECK(cce_specialist_select_kv_tokens(scores, 10, &b, out, 10, &n) == CCE_OK,
          "sparse selection succeeds");
    CHECK(n == 6, "sparse selection respects token budget");
    CHECK(has_index(out, n, 0) && has_index(out, n, 1),
          "initial anchor tokens are retained");
    CHECK(has_index(out, n, 8) && has_index(out, n, 9),
          "recent tokens are retained");
    CHECK(has_index(out, n, 4),
          "long-range dependency anchor is retained");
    CHECK(has_index(out, n, 7),
          "highest middle heavy hitter is retained");
    for (int i = 1; i < n; ++i) {
        CHECK(out[i - 1] < out[i], "selected tokens are sorted and unique");
    }
}

static void test_default_budget_is_sparse(void) {
    cce_specialist_kv_budget b;
    cce_specialist_kv_budget_default(&b, 100);
    CHECK(b.max_tokens > 0 && b.max_tokens <= 25,
          "default budget targets about 20 percent of context");
    CHECK(b.initial_tokens > 0 && b.recent_tokens > 0,
          "default budget keeps sink and recent tokens");
    CHECK(b.long_range_stride > 0,
          "default budget includes long-range stride");
}

int main(void) {
    printf("sparse KV selector tests:\n");
    test_full_budget_returns_all();
    test_sparse_policy_keeps_anchors_recent_and_heavy();
    test_default_budget_is_sparse();

    if (failures == 0) {
        printf("\nAll sparse KV selector tests passed.\n");
        return 0;
    }
    printf("\n%d sparse KV selector test(s) FAILED.\n", failures);
    return 1;
}
