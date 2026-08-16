#include "../include/cnet_dc_invent.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    CnetDcArena a;
    CnetDcGrammar g;
    CnetDcTerm hits[CNET_DC_MAX_HITS];
    CnetDcTerm two, dream;
    char spoken[160];
    unsigned rng = 13u;
    int n_int = 0, n_list = 0, n_ood = 0, i, dreams = 0;
    int t0, tint, tbool, list_int;
    int reused = 0;

    cnet_dc_arena_init(&a);
    cnet_dc_grammar_init(&g);
    t0 = cnet_dc_var(&a, 0);
    tint = cnet_dc_base(&a, "int");
    tbool = cnet_dc_base(&a, "bool");
    list_int = cnet_dc_list(&a, tint);
    if (cnet_dc_grammar_add(&g, "zero", tint, 0) != 0 ||
        cnet_dc_grammar_add(&g, "incr", cnet_dc_arrow(&a, tint, tint), 0) != 0 ||
        cnet_dc_grammar_add(&g, "empty_int", list_int, 0) != 0 ||
        cnet_dc_grammar_add(&g, "cons",
                            cnet_dc_arrow(&a, t0,
                                          cnet_dc_arrow(&a, cnet_dc_list(&a, t0),
                                                        cnet_dc_list(&a, t0))),
                            0) != 0)
        return 1;

    if (cnet_dc_wake(&a, &g, tint, 2, hits, CNET_DC_MAX_HITS, &n_int) != 0 ||
        n_int <= 0)
        return 1;
    memset(&two, 0, sizeof two);
    for (i = 0; i < n_int; ++i) {
        if (strcmp(hits[i].text, "(incr (incr zero))") == 0) two = hits[i];
    }
    if (two.text[0] == '\0' || cnet_dc_sleep_invent(&g, "two", &two) != 0)
        return 1;
    if (cnet_dc_wake(&a, &g, list_int, 2, hits, CNET_DC_MAX_HITS, &n_list) != 0 ||
        n_list <= 0)
        return 1;
    for (i = 0; i < n_list; ++i) {
        if (hits[i].used_invented) reused = 1;
    }
    if (cnet_dc_wake(&a, &g, tbool, 2, hits, CNET_DC_MAX_HITS, &n_ood) != 0 ||
        n_ood != 0)
        return 1;
    for (i = 0; i < 24; ++i) {
        if (cnet_dc_dream(&a, &g, &rng, 2, &dream) != 0 || dream.text[0] == '\0')
            return 1;
        ++dreams;
    }
    if (cnet_dc_speak_bound(&two, "2", spoken, sizeof spoken) != 0 ||
        cnet_dc_speak_bound(&two, "", spoken, sizeof spoken) == 0)
        return 1;
    if (!reused) {
        printf("CNET_DC_INVENT_BENCH_RED reason=no_reuse\n");
        return 1;
    }
    printf("CNET_DC_INVENT_BENCH_PASS wake_hits=%d other_goal_hits=%d ood=0 "
           "dreams=%d reused=1 speak_bound=1 residual=0 "
           "claim=typed_invention_only broader_claims=WITHHELD\n",
           n_int, n_list, dreams);
    return 0;
}
