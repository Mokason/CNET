#include "../include/counterfactual_order.h"

#include <stdlib.h>
#include <string.h>

int cnet_cf_order_env_enabled(void) {
    const char *e = getenv("CNET_CF_ORDER");
    if (!e || !e[0] || (e[0] == '0' && e[1] == '\0')) return 0;
    return 1;
}

int cnet_cf_order_rank(const CnetCfOrderCandidate *cands, size_t n,
                       int use_cf,
                       size_t *order, size_t *out_n) {
    size_t i, j, m = 0;
    if (!cands || !order || !out_n) return -1;
    if (n == 0) { *out_n = 0; return 0; }

    /* Certified-only membership. */
    for (i = 0; i < n; i++) {
        if (!cands[i].certified) continue;
        order[m++] = i;
    }
    *out_n = m;

    /* Insertion sort: reliability desc, optional cf_score desc, name asc. */
    for (i = 1; i < m; i++) {
        size_t key = order[i];
        j = i;
        while (j > 0) {
            size_t prev = order[j - 1];
            const CnetCfOrderCandidate *a = &cands[prev];
            const CnetCfOrderCandidate *b = &cands[key];
            int swap = 0;
            if (b->reliability > a->reliability) swap = 1;
            else if (b->reliability < a->reliability) swap = 0;
            else if (use_cf && b->cf_score > a->cf_score) swap = 1;
            else if (use_cf && b->cf_score < a->cf_score) swap = 0;
            else {
                const char *an = a->name ? a->name : "";
                const char *bn = b->name ? b->name : "";
                if (strcmp(bn, an) < 0) swap = 1;
                else swap = 0;
            }
            if (!swap) break;
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }
    return 0;
}
