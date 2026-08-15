#include "../include/cnet_dc_invent.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define REQUIRE(cond, reason)                                                 \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("CNET_DC_EGRAPH_RED reason=%s checks=%d\n", reason,        \
                   checks);                                                   \
            ++failures;                                                       \
            return;                                                           \
        }                                                                     \
        ++checks;                                                             \
    } while (0)

static void fill_cert(CnetDcMissRow *r, const char *id, const char *term,
                      int type) {
    memset(r, 0, sizeof *r);
    snprintf(r->goal_type, sizeof r->goal_type, "%s", "int");
    snprintf(r->trace_id, sizeof r->trace_id, "%s", id);
    snprintf(r->term, sizeof r->term, "%s", term);
    r->certified = 1;
    r->has_out = 1;
    cnet_dc_value_int(&r->out, 2);
    r->type = type;
}

static void test_map_hole_eclass_not_strstr(void) {
    CnetDcArena a;
    CnetDcGrammar g;
    CnetDcMissRow rows[2];
    CnetDcTerm brick, solved[2];
    char name[CNET_DC_NAME_MAX];
    int tint, n_before;
    const char *surf_a = "(incr (incr zero))";
    const char *surf_b = "((lam (x) (incr (incr x))) zero)";
    const char *brick_need = "(lam (x) (incr (incr x)))";

    cnet_dc_arena_init(&a);
    cnet_dc_grammar_init(&g);
    tint = cnet_dc_base(&a, "int");
    REQUIRE(cnet_dc_grammar_add(&g, "zero", tint, 0) == 0, "zero");
    REQUIRE(cnet_dc_grammar_add(&g, "incr", cnet_dc_arrow(&a, tint, tint), 0) ==
                0,
            "incr");
    fill_cert(&rows[0], "t1", surf_a, tint);
    fill_cert(&rows[1], "t2", surf_b, tint);
    REQUIRE(cnet_dc_misslog_eclass_shared(rows, 2, surf_a, surf_b) == 1,
            "eclass_after_invbeta");
    REQUIRE(strstr(surf_b, surf_a) == NULL, "no_surface_substring");
    memset(solved, 0, sizeof solved);
    snprintf(solved[0].text, sizeof solved[0].text, "%s", surf_a);
    solved[0].type = tint;
    snprintf(solved[1].text, sizeof solved[1].text, "%s", surf_b);
    solved[1].type = tint;
    REQUIRE(cnet_dc_sleep_compress(&g, solved, 2, name, sizeof name, &brick) ==
                1,
            "cse_strstr_misses");
    n_before = g.n_prims;
    memset(&brick, 0, sizeof brick);
    name[0] = '\0';
    REQUIRE(cnet_dc_misslog_extract_rows(&g, rows, 2, name, sizeof name,
                                         &brick) == 0,
            "extract");
    REQUIRE(strcmp(brick.text, brick_need) == 0, "shared_lam_brick");
    REQUIRE(brick.text[0] == '(', "compound");
    REQUIRE(g.n_prims == n_before + 1, "proposed_one");
    REQUIRE(g.prims[g.n_prims - 1].invented == 1, "sleep_invent");
    REQUIRE(name[0] != '\0', "named");
    REQUIRE(cnet_dc_extract_specialist_admit_calls() == 0, "no_admit");
}

static void test_laws_two_proper_compound(void) {
    CnetDcArena a;
    CnetDcGrammar g;
    CnetDcMissRow one[1], two_atom[2], two_top[2];
    CnetDcTerm brick;
    char name[CNET_DC_NAME_MAX];
    int tint;

    cnet_dc_arena_init(&a);
    cnet_dc_grammar_init(&g);
    tint = cnet_dc_base(&a, "int");
    REQUIRE(cnet_dc_grammar_add(&g, "zero", tint, 0) == 0, "zero");
    REQUIRE(cnet_dc_grammar_add(&g, "incr", cnet_dc_arrow(&a, tint, tint), 0) ==
                0,
            "incr");
    fill_cert(&one[0], "only", "(incr (incr zero))", tint);
    REQUIRE(cnet_dc_misslog_extract_rows(&g, one, 1, name, sizeof name,
                                         &brick) == 1,
            "one_trace_not_sleep");
    fill_cert(&two_atom[0], "a1", "zero", tint);
    fill_cert(&two_atom[1], "a2", "zero", tint);
    REQUIRE(cnet_dc_misslog_extract_rows(&g, two_atom, 2, name, sizeof name,
                                         &brick) == 1,
            "atom_not_compound");
    fill_cert(&two_top[0], "p1", "incr", tint);
    fill_cert(&two_top[1], "p2", "incr", tint);
    REQUIRE(cnet_dc_misslog_extract_rows(&g, two_top, 2, name, sizeof name,
                                         &brick) == 1,
            "enumerated_prim_not_sleep");
}

static void test_banned_missing(void) {
    CnetDcGrammar g;
    CnetDcMissRow rows[4];
    CnetDcTerm brick;
    char name[CNET_DC_NAME_MAX];
    int n = 99;

    cnet_dc_grammar_init(&g);
    REQUIRE(cnet_dc_misslog_load("plans/cnet_asi5_v5_handoff.md", rows, 4,
                                 &n) != 0,
            "refuse_asi5");
    REQUIRE(cnet_dc_misslog_load("/tmp/chat-1/heldout.jsonl", rows, 4, &n) != 0,
            "refuse_chat1");
    REQUIRE(cnet_dc_misslog_load("/tmp/compete_suite/x.jsonl", rows, 4, &n) !=
                0,
            "refuse_compete");
    REQUIRE(cnet_dc_misslog_extract(&g, "plans/suite_data_v5/x.jsonl", name,
                                    sizeof name, &brick) != 0,
            "refuse_extract_suite");
    n = 99;
    REQUIRE(cnet_dc_misslog_load("/tmp/cnet_dc_missing_misslog.jsonl", rows, 4,
                                 &n) == 0,
            "missing_ok");
    REQUIRE(n == 0, "missing_zero_rows");
    REQUIRE(cnet_dc_misslog_extract(&g, "/tmp/cnet_dc_missing_misslog.jsonl",
                                    name, sizeof name, &brick) == 1,
            "missing_extract_nothing");
}

static void test_io_fail_not_gold(void) {
    CnetDcArena a;
    CnetDcGrammar g;
    CnetDcMissRow rows[3];
    CnetDcTerm brick;
    char name[CNET_DC_NAME_MAX];
    int tint, n_before;

    cnet_dc_arena_init(&a);
    cnet_dc_grammar_init(&g);
    tint = cnet_dc_base(&a, "int");
    REQUIRE(cnet_dc_grammar_add(&g, "zero", tint, 0) == 0, "zero");
    REQUIRE(cnet_dc_grammar_add(&g, "incr", cnet_dc_arrow(&a, tint, tint), 0) ==
                0,
            "incr");
    fill_cert(&rows[0], "c1", "(incr (incr zero))", tint);
    memset(&rows[1], 0, sizeof rows[1]);
    snprintf(rows[1].goal_type, sizeof rows[1].goal_type, "%s", "int");
    snprintf(rows[1].trace_id, sizeof rows[1].trace_id, "%s", "fail");
    snprintf(rows[1].abstain_reason, sizeof rows[1].abstain_reason, "%s",
             CNET_DC_MISS_NO_EVAL_MATCH);
    snprintf(rows[1].unused_invent_term, sizeof rows[1].unused_invent_term, "%s",
             "(incr zero)");
    rows[1].certified = 0;
    rows[1].has_out = 1;
    cnet_dc_value_int(&rows[1].out, 2);
    n_before = g.n_prims;
    REQUIRE(cnet_dc_misslog_extract_rows(&g, rows, 2, name, sizeof name,
                                         &brick) == 1,
            "unused_not_second_certified");
    REQUIRE(g.n_prims == n_before, "no_propose_from_fail");
    REQUIRE(cnet_dc_misslog_eclass_shared(rows, 2, "(incr zero)",
                                          "(incr (incr zero))") == 0,
            "unused_not_equated_to_gold");
    fill_cert(&rows[2], "c2", "((lam (x) (incr (incr x))) zero)", tint);
    REQUIRE(cnet_dc_misslog_eclass_shared(rows, 3, "(incr zero)",
                                          "(incr (incr zero))") == 0,
            "fail_row_still_not_gold");
    REQUIRE(cnet_dc_extract_specialist_admit_calls() == 0, "no_admit_failpath");
}

static void test_persist_and_cse_kept(void) {
    CnetDcArena a;
    CnetDcGrammar g;
    CnetDcMissRow rows[4], loaded[4];
    CnetDcTerm brick, solved[2];
    char name[CNET_DC_NAME_MAX];
    const char *path = "/tmp/cnet_dc_egraph_misslog.jsonl";
    int tint, n = 0;
    FILE *wipe;

    cnet_dc_arena_init(&a);
    cnet_dc_grammar_init(&g);
    tint = cnet_dc_base(&a, "int");
    REQUIRE(cnet_dc_grammar_add(&g, "zero", tint, 0) == 0, "zero");
    REQUIRE(cnet_dc_grammar_add(&g, "incr", cnet_dc_arrow(&a, tint, tint), 0) ==
                0,
            "incr");
    wipe = fopen(path, "w");
    REQUIRE(wipe != NULL, "wipe");
    fclose(wipe);
    fill_cert(&rows[0], "p1", "(incr (incr zero))", tint);
    fill_cert(&rows[1], "p2", "((lam (x) (incr (incr x))) zero)", tint);
    REQUIRE(cnet_dc_misslog_append(path, &rows[0]) == 0, "append1");
    REQUIRE(cnet_dc_misslog_append(path, &rows[1]) == 0, "append2");
    REQUIRE(cnet_dc_misslog_load(path, loaded, 4, &n) == 0, "reload");
    REQUIRE(n == 2 && loaded[0].certified == 1 && loaded[1].certified == 1,
            "persisted_certified");
    REQUIRE(cnet_dc_misslog_extract(&g, path, name, sizeof name, &brick) == 0,
            "extract_file");
    REQUIRE(strcmp(brick.text, "(lam (x) (incr (incr x)))") == 0,
            "file_brick");
    REQUIRE(cnet_dc_misslog_append("plans/cnet_asi5_v5_handoff.md",
                                   &rows[0]) != 0,
            "append_banned");
    memset(solved, 0, sizeof solved);
    snprintf(solved[0].text, sizeof solved[0].text, "%s", "(incr (incr zero))");
    solved[0].type = tint;
    snprintf(solved[1].text, sizeof solved[1].text, "%s",
             "((cons (incr (incr zero))) empty_int)");
    solved[1].type = tint;
    REQUIRE(cnet_dc_sleep_compress(&g, solved, 2, name, sizeof name, &brick) ==
                0,
            "cse_door_still_works");
    REQUIRE(strcmp(brick.text, "(incr (incr zero))") == 0, "cse_shared");
    REQUIRE(cnet_dc_extract_specialist_admit_calls() == 0, "no_admit_persist");
}

int main(void) {
    test_map_hole_eclass_not_strstr();
    test_laws_two_proper_compound();
    test_banned_missing();
    test_io_fail_not_gold();
    test_persist_and_cse_kept();
    if (failures != 0) return 1;
    if (checks != 44) {
        printf("CNET_DC_EGRAPH_RED reason=check_count got=%d want=44\n",
               checks);
        return 1;
    }
    printf("CNET_DC_EGRAPH_PASS checks=44 eclass=1 cse_miss=1 cse_kept=1 "
           "propose=1 admit=0 inv_beta_bound=1 python=0 dream_q=SKIP "
           "f11=0 suite_mine=0 residual=0 broader_claims=WITHHELD\n");
    return 0;
}
