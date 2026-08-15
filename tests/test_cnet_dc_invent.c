#include "../include/cnet_dc_invent.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define REQUIRE(cond, reason)                                                 \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("CNET_DC_INVENT_RED reason=%s\n", reason);                 \
            ++failures;                                                       \
            return;                                                           \
        }                                                                     \
    } while (0)

static int has_text(const CnetDcTerm *hits, int n, const char *need) {
    int i;
    for (i = 0; i < n; ++i) {
        if (strcmp(hits[i].text, need) == 0) return 1;
    }
    return 0;
}

static void test_wake_undeclared(void) {
    CnetDcArena a;
    CnetDcGrammar g;
    CnetDcTerm hits[CNET_DC_MAX_HITS];
    int n = 0, t0, tint, tbool, list_int;
    cnet_dc_arena_init(&a);
    cnet_dc_grammar_init(&g);
    t0 = cnet_dc_var(&a, 0);
    tint = cnet_dc_base(&a, "int");
    tbool = cnet_dc_base(&a, "bool");
    list_int = cnet_dc_list(&a, tint);
    REQUIRE(cnet_dc_grammar_add(&g, "zero", tint, 0) == 0, "add_zero");
    REQUIRE(cnet_dc_grammar_add(&g, "empty_int", list_int, 0) == 0, "add_empty");
    REQUIRE(cnet_dc_grammar_add(&g, "cons",
                                cnet_dc_arrow(&a, t0,
                                              cnet_dc_arrow(&a, cnet_dc_list(&a, t0),
                                                            cnet_dc_list(&a, t0))),
                                0) == 0,
            "add_cons");
    REQUIRE(cnet_dc_wake(&a, &g, list_int, 2, hits, CNET_DC_MAX_HITS, &n) == 0,
            "wake");
    REQUIRE(n > 0, "undeclared_hit");
    REQUIRE(has_text(hits, n, "((cons zero) empty_int)") ||
                has_text(hits, n, "empty_int"),
            "list_int_term");
    n = 0;
    REQUIRE(cnet_dc_wake(&a, &g, tbool, 2, hits, CNET_DC_MAX_HITS, &n) == 0,
            "wake_bool");
    REQUIRE(n == 0, "ood_abstain");
}

static void test_sleep_reuse_other_goal(void) {
    CnetDcArena a;
    CnetDcGrammar g;
    CnetDcTerm hits[CNET_DC_MAX_HITS];
    CnetDcTerm two;
    int n_int = 0, n_list = 0, i, t0, tint, list_int, reused = 0;
    cnet_dc_arena_init(&a);
    cnet_dc_grammar_init(&g);
    t0 = cnet_dc_var(&a, 0);
    tint = cnet_dc_base(&a, "int");
    list_int = cnet_dc_list(&a, tint);
    REQUIRE(cnet_dc_grammar_add(&g, "zero", tint, 0) == 0, "zero");
    REQUIRE(cnet_dc_grammar_add(&g, "incr", cnet_dc_arrow(&a, tint, tint), 0) ==
                0,
            "incr");
    REQUIRE(cnet_dc_grammar_add(&g, "empty_int", list_int, 0) == 0, "empty");
    REQUIRE(cnet_dc_grammar_add(&g, "cons",
                                cnet_dc_arrow(&a, t0,
                                              cnet_dc_arrow(&a, cnet_dc_list(&a, t0),
                                                            cnet_dc_list(&a, t0))),
                                0) == 0,
            "cons");
    REQUIRE(cnet_dc_wake(&a, &g, tint, 2, hits, CNET_DC_MAX_HITS, &n_int) == 0,
            "wake_int");
    REQUIRE(has_text(hits, n_int, "(incr (incr zero))"), "made_two");
    memset(&two, 0, sizeof two);
    for (i = 0; i < n_int; ++i) {
        if (strcmp(hits[i].text, "(incr (incr zero))") == 0) two = hits[i];
    }
    REQUIRE(cnet_dc_sleep_invent(&g, "two", &two) == 0, "sleep");
    REQUIRE(cnet_dc_wake(&a, &g, list_int, 2, hits, CNET_DC_MAX_HITS, &n_list) ==
                0,
            "wake_other_goal");
    REQUIRE(n_list > 0, "other_goal_hit");
    for (i = 0; i < n_list; ++i) {
        if (hits[i].used_invented) reused = 1;
    }
    REQUIRE(reused, "reused_two");
    REQUIRE(has_text(hits, n_list, "((cons two) empty_int)"), "cons_two");
}

static void test_dream_typed(void) {
    CnetDcArena a;
    CnetDcGrammar g;
    CnetDcTerm dream;
    unsigned rng = 7u;
    int i, tint;
    cnet_dc_arena_init(&a);
    cnet_dc_grammar_init(&g);
    tint = cnet_dc_base(&a, "int");
    REQUIRE(cnet_dc_grammar_add(&g, "zero", tint, 0) == 0, "zero");
    REQUIRE(cnet_dc_grammar_add(&g, "incr", cnet_dc_arrow(&a, tint, tint), 0) ==
                0,
            "incr");
    for (i = 0; i < 16; ++i) {
        memset(&dream, 0, sizeof dream);
        REQUIRE(cnet_dc_dream(&a, &g, &rng, 2, &dream) == 0, "dream");
        REQUIRE(dream.text[0] != '\0' && dream.type >= 0, "typed");
        REQUIRE(strstr(dream.text, "llm") == NULL, "no_residual");
    }
}

static void test_speak_bound(void) {
    CnetDcTerm term;
    char spoken[160];
    memset(&term, 0, sizeof term);
    snprintf(term.text, sizeof term.text, "%s", "((cons zero) empty)");
    term.type = 1;
    REQUIRE(cnet_dc_speak_bound(&term, "2002", spoken, sizeof spoken) == 0,
            "speak");
    REQUIRE(strstr(spoken, "2002") != NULL, "bound_value");
    REQUIRE(strstr(spoken, CNET_DC_INVENT_CONTRACT) != NULL, "contract");
    REQUIRE(cnet_dc_speak_bound(&term, "", spoken, sizeof spoken) != 0,
            "refuse_empty");
    REQUIRE(cnet_dc_speak_bound(&term, NULL, spoken, sizeof spoken) != 0,
            "refuse_null");
    REQUIRE(cnet_dc_speak_bound(NULL, "13", spoken, sizeof spoken) != 0,
            "refuse_no_term");
}

static void test_wake_io_solves(void) {
    CnetDcArena a;
    CnetDcGrammar g;
    CnetDcTerm hits[CNET_DC_MAX_HITS];
    CnetDcExample ex;
    int n = 0, tint, t0, list_int;
    cnet_dc_arena_init(&a);
    cnet_dc_grammar_init(&g);
    t0 = cnet_dc_var(&a, 0);
    tint = cnet_dc_base(&a, "int");
    list_int = cnet_dc_list(&a, tint);
    REQUIRE(cnet_dc_grammar_add(&g, "zero", tint, 0) == 0, "zero");
    REQUIRE(cnet_dc_grammar_add(&g, "incr", cnet_dc_arrow(&a, tint, tint), 0) ==
                0,
            "incr");
    REQUIRE(cnet_dc_grammar_add(&g, "empty_int", list_int, 0) == 0, "empty");
    REQUIRE(cnet_dc_grammar_add(&g, "cons",
                                cnet_dc_arrow(&a, t0,
                                              cnet_dc_arrow(&a, cnet_dc_list(&a, t0),
                                                            cnet_dc_list(&a, t0))),
                                0) == 0,
            "cons");
    memset(&ex, 0, sizeof ex);
    cnet_dc_value_int(&ex.out, 2);
    REQUIRE(cnet_dc_wake_io(&a, &g, tint, 2, &ex, 1, hits, CNET_DC_MAX_HITS,
                            &n) == 0,
            "wake_io");
    REQUIRE(n > 0, "io_hit");
    REQUIRE(has_text(hits, n, "(incr (incr zero))"), "two");
    REQUIRE(!has_text(hits, n, "zero"), "type_only_zero_refused");
    cnet_dc_value_int(&ex.out, 99);
    n = 0;
    REQUIRE(cnet_dc_wake_io(&a, &g, tint, 2, &ex, 1, hits, CNET_DC_MAX_HITS,
                            &n) == 0,
            "wake_io_miss");
    REQUIRE(n == 0, "io_abstain");
    n = 0;
    REQUIRE(cnet_dc_wake_io(&a, &g, tint, 2, NULL, 0, hits, CNET_DC_MAX_HITS,
                            &n) == 0,
            "wake_io_no_ex");
    REQUIRE(n == 0, "no_examples_abstain");
}

static void test_wake_io_function(void) {
    CnetDcArena a;
    CnetDcGrammar g;
    CnetDcTerm hits[CNET_DC_MAX_HITS];
    CnetDcExample ex[2];
    int n = 0, tint, tfun;
    cnet_dc_arena_init(&a);
    cnet_dc_grammar_init(&g);
    tint = cnet_dc_base(&a, "int");
    tfun = cnet_dc_arrow(&a, tint, tint);
    REQUIRE(cnet_dc_grammar_add(&g, "zero", tint, 0) == 0, "zero");
    REQUIRE(cnet_dc_grammar_add(&g, "incr", tfun, 0) == 0, "incr");
    memset(ex, 0, sizeof ex);
    cnet_dc_value_int(&ex[0].in, 0);
    cnet_dc_value_int(&ex[0].out, 1);
    cnet_dc_value_int(&ex[1].in, 4);
    cnet_dc_value_int(&ex[1].out, 5);
    REQUIRE(cnet_dc_wake_io(&a, &g, tfun, 1, ex, 2, hits, CNET_DC_MAX_HITS,
                            &n) == 0,
            "wake_fn");
    REQUIRE(n > 0 && has_text(hits, n, "incr"), "incr_solves");
}

static void test_sleep_compresses(void) {
    CnetDcArena a;
    CnetDcGrammar g;
    CnetDcTerm solved[2], brick;
    char name[CNET_DC_NAME_MAX];
    int tint, list_int, n_before;
    cnet_dc_arena_init(&a);
    cnet_dc_grammar_init(&g);
    tint = cnet_dc_base(&a, "int");
    list_int = cnet_dc_list(&a, tint);
    REQUIRE(cnet_dc_grammar_add(&g, "zero", tint, 0) == 0, "zero");
    REQUIRE(cnet_dc_grammar_add(&g, "incr", cnet_dc_arrow(&a, tint, tint), 0) ==
                0,
            "incr");
    REQUIRE(cnet_dc_grammar_add(&g, "empty_int", list_int, 0) == 0, "empty");
    memset(solved, 0, sizeof solved);
    snprintf(solved[0].text, sizeof solved[0].text, "%s", "(incr (incr zero))");
    solved[0].type = tint;
    snprintf(solved[1].text, sizeof solved[1].text, "%s",
             "((cons (incr (incr zero))) empty_int)");
    solved[1].type = list_int;
    n_before = g.n_prims;
    REQUIRE(cnet_dc_sleep_compress(&g, solved, 1, name, sizeof name, &brick) ==
                1,
            "one_trace_not_sleep");
    REQUIRE(cnet_dc_sleep_compress(&g, solved, 2, name, sizeof name, &brick) ==
                0,
            "compress");
    REQUIRE(strcmp(brick.text, "(incr (incr zero))") == 0, "shared_subbrick");
    REQUIRE(g.n_prims == n_before + 1, "one_brick");
    REQUIRE(name[0] != '\0', "named");
}

static void test_mine_io_refuses_suites(void) {
    CnetDcExample ex[4];
    int n = 0;
    FILE *f;
    REQUIRE(cnet_dc_mine_io_jsonl("plans/cnet_asi5_v5_handoff.md", ex, 4, &n) !=
                0,
            "refuse_asi5");
    REQUIRE(cnet_dc_mine_io_jsonl("/tmp/chat-1/heldout.jsonl", ex, 4, &n) != 0,
            "refuse_chat1");
    f = fopen("/tmp/cnet_dc_io_traces.jsonl", "w");
    REQUIRE(f != NULL, "write_traces");
    fprintf(f, "{\"in\":0,\"out\":1}\n{\"in\":4,\"out\":5}\n");
    fclose(f);
    n = 0;
    REQUIRE(cnet_dc_mine_io_jsonl("/tmp/cnet_dc_io_traces.jsonl", ex, 4, &n) ==
                0,
            "mine_ok");
    REQUIRE(n == 2 && ex[0].out.num == 1 && ex[1].in.num == 4, "mined_rows");
    n = 0;
    REQUIRE(cnet_dc_mine_io_jsonl("/tmp/cnet_dc_missing_traces.jsonl", ex, 4,
                                  &n) == 0,
            "missing_ok");
    REQUIRE(n == 0, "missing_empty");
}

int main(void) {
    test_wake_undeclared();
    test_sleep_reuse_other_goal();
    test_dream_typed();
    test_speak_bound();
    test_wake_io_solves();
    test_wake_io_function();
    test_sleep_compresses();
    test_mine_io_refuses_suites();
    if (failures != 0) return 1;
    printf("CNET_DC_INVENT_PASS wake=1 wake_io=1 sleep_reuse=1 "
           "sleep_compress=1 dream=1 speak_bound=1 residual=0 "
           "dream_q=SKIP broader_claims=WITHHELD\n");
    return 0;
}
