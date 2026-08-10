/* ROE skill tree: path to surpass Unlimited-OCR (as system and/or bench).
 * make roe_asi_ocr_tree → ROE_ASI_OCR_TREE_PASS
 *
 * Nodes = capabilities. rank 0 locked / 1 partial / 2 solid / 3 mastered.
 * "Surpass" is multi-axis — see snapshot verdict.
 */
#include <stdio.h>
#include <string.h>

#include "../include/cnet_roe_tree.h"

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-64s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* Force-set rank for planning snapshot (not runtime unlock). */
static void set_rank(RoeTree *T, const char *id, int rank) {
    size_t i;
    for (i = 0; i < T->n_nodes; i++) {
        if (T->nodes[i].active && !strcmp(T->nodes[i].id, id)) {
            if (rank > T->nodes[i].max_rank) rank = T->nodes[i].max_rank;
            if (rank < 0) rank = 0;
            T->nodes[i].rank = rank;
            return;
        }
    }
}

static int seed_ocr_surpass_tree(RoeTree *T) {
    if (!T) return -1;
    roe_tree_init(T);

    /* === ROOTS === */
    roe_tree_add_node(T, "root_doc", "Document OCR Domain", "ocr",
                      "Compete in document parsing / long PDF OCR", 3, 5, 1.1);
    roe_tree_add_node(T, "root_shell", "Shell / ASI Law", "shell",
                      "Fail-closed, CERT, tidy capsules", 3, 4, 1.15);
    roe_tree_add_node(T, "root_sys", "System Product", "meta",
                      "Hybrid teacher + local promote + cost", 3, 5, 1.2);

    /* === L1 Perception frontend (Unlimited has VLM encoder) === */
    roe_tree_add_node(T, "perc_render", "Page render / PDF raster", "ocr",
                      "pdf2image, DPI, multi-page batch", 3, 3, 1.05);
    roe_tree_add_node(T, "perc_layout", "Layout analysis", "ocr",
                      "blocks, columns, reading order, tables regions", 3, 5, 1.1);
    roe_tree_add_node(T, "perc_glyph", "Glyph / line detector", "ocr",
                      "text lines, words, chars; poly boxes", 3, 4, 1.08);
    roe_tree_add_node(T, "perc_table", "Table structure", "ocr",
                      "cells, spans, TEDS-grade structure", 3, 6, 1.12);
    roe_tree_add_node(T, "perc_formula", "Formula / code blocks", "ocr",
                      "math, code fences, special scripts", 3, 4, 1.08);
    roe_tree_add_node(T, "perc_multi", "Multi-page / long doc", "ocr",
                      "40+ pages one-pass or streaming state", 3, 7, 1.18);

    /* === L2 Recognition backends === */
    roe_tree_add_node(T, "rec_hermetic", "Hermetic glyph OCR", "ocr",
                      "5x7 CERT alphabet — done for offline floor", 3, 2, 1.02);
    roe_tree_add_node(T, "rec_classic", "Classic OCR engine", "ocr",
                      "Tesseract/PaddleOCR as untrusted teacher", 3, 4, 1.06);
    roe_tree_add_node(T, "rec_vlm", "VLM OCR teacher", "ocr",
                      "Unlimited-OCR / DeepSeek-OCR class teacher", 3, 8, 1.2);
    roe_tree_add_node(T, "rec_moe", "Long-context / R-SWA style", "ocr",
                      "constant-memory long output decode", 3, 7, 1.15);
    roe_tree_add_node(T, "rec_lang", "Multi-language + scripts", "ocr",
                      "CJK, RTL, mixed, handwriting subset", 3, 5, 1.1);

    /* === L3 Verify / CERT / capsules === */
    roe_tree_add_node(T, "ver_conf", "Conformal OCR abstain", "shell",
                      "conf floors; refuse bad pages", 3, 4, 1.12);
    roe_tree_add_node(T, "ver_gold", "Gold / consensus verify", "shell",
                      "multi-engine vote or human gold before CERT", 3, 5, 1.15);
    roe_tree_add_node(T, "ver_capsule", "Vision capsule schema-2", "shell",
                      "frontend asset bound to unit (exists transport)", 3, 4, 1.1);
    roe_tree_add_node(T, "ver_tidy", "Tidy vision/ocr taxonomy", "shell",
                      "cat/sub capsules; goal microsplit", 3, 3, 1.08);
    roe_tree_add_node(T, "ver_proj", "Project doc memory L3", "shell",
                      "this corpus: recurring headers/footers/templates", 3, 5, 1.15);

    /* === L4 Bench & beat axes === */
    roe_tree_add_node(T, "bench_omni", "OmniDocBench eval harness", "meta",
                      "v1.5/v1.6 overall + edit distance + TEDS", 3, 6, 1.12);
    roe_tree_add_node(T, "beat_quality", "Beat quality SOTA", "meta",
                      "overall > Unlimited ~93.2% OmniDoc v1.5", 3, 12, 1.25);
    roe_tree_add_node(T, "beat_long", "Beat long-doc speed/mem", "meta",
                      "flat throughput at long outputs; 40+ pages", 3, 8, 1.18);
    roe_tree_add_node(T, "beat_cost", "Beat $ / token / offline", "meta",
                      "local hit rate; teacher only on miss", 3, 7, 1.2);
    roe_tree_add_node(T, "beat_audit", "Beat audit / revoke / law", "meta",
                      "CERT, taint, fail-closed vs opaque VLM", 3, 6, 1.2);
    roe_tree_add_node(T, "beat_system", "Beat as product system", "meta",
                      "goal map + hybrid teacher + packs > raw model", 3, 10, 1.3);

    /* === prereqs === */
    roe_tree_add_prereq(T, "perc_render", "root_doc", 1);
    roe_tree_add_prereq(T, "perc_layout", "perc_render", 1);
    roe_tree_add_prereq(T, "perc_glyph", "perc_layout", 1);
    roe_tree_add_prereq(T, "perc_table", "perc_layout", 2);
    roe_tree_add_prereq(T, "perc_formula", "perc_glyph", 1);
    roe_tree_add_prereq(T, "perc_multi", "perc_layout", 2);
    roe_tree_add_prereq(T, "perc_multi", "rec_moe", 1);

    roe_tree_add_prereq(T, "rec_hermetic", "root_doc", 1);
    roe_tree_add_prereq(T, "rec_classic", "perc_glyph", 1);
    roe_tree_add_prereq(T, "rec_vlm", "perc_render", 2);
    roe_tree_add_prereq(T, "rec_moe", "rec_vlm", 1);
    roe_tree_add_prereq(T, "rec_lang", "rec_vlm", 1);

    roe_tree_add_prereq(T, "ver_conf", "root_shell", 1);
    roe_tree_add_prereq(T, "ver_gold", "ver_conf", 1);
    roe_tree_add_prereq(T, "ver_capsule", "root_shell", 1);
    roe_tree_add_prereq(T, "ver_tidy", "ver_capsule", 1);
    roe_tree_add_prereq(T, "ver_proj", "ver_tidy", 1);
    roe_tree_add_prereq(T, "ver_proj", "ver_gold", 1);

    roe_tree_add_prereq(T, "bench_omni", "root_sys", 1);
    roe_tree_add_prereq(T, "bench_omni", "perc_table", 1);
    roe_tree_add_prereq(T, "beat_quality", "bench_omni", 2);
    roe_tree_add_prereq(T, "beat_quality", "rec_moe", 2);
    roe_tree_add_prereq(T, "beat_quality", "perc_table", 2);
    roe_tree_add_prereq(T, "beat_long", "perc_multi", 2);
    roe_tree_add_prereq(T, "beat_long", "rec_moe", 2);
    roe_tree_add_prereq(T, "beat_cost", "rec_hermetic", 2);
    roe_tree_add_prereq(T, "beat_cost", "ver_proj", 1);
    roe_tree_add_prereq(T, "beat_cost", "rec_vlm", 1);
    roe_tree_add_prereq(T, "beat_audit", "ver_gold", 2);
    roe_tree_add_prereq(T, "beat_audit", "ver_capsule", 2);
    roe_tree_add_prereq(T, "beat_system", "beat_cost", 1);
    roe_tree_add_prereq(T, "beat_system", "beat_audit", 1);
    roe_tree_add_prereq(T, "beat_system", "ver_proj", 2);

    return 0;
}

/* Current ROE/CNET ground truth ranks (0–3). Honest. */
static void apply_current_progress(RoeTree *T) {
    set_rank(T, "root_doc", 1);
    set_rank(T, "root_shell", 2);
    set_rank(T, "root_sys", 1);

    set_rank(T, "perc_render", 0); /* no pdf2image batch in ROE OCR yet */
    set_rank(T, "perc_layout", 0);
    set_rank(T, "perc_glyph", 1); /* hermetic glyphs only */
    set_rank(T, "perc_table", 0);
    set_rank(T, "perc_formula", 0);
    set_rank(T, "perc_multi", 0);

    set_rank(T, "rec_hermetic", 3); /* ROE_ASI_OCR_PASS */
    set_rank(T, "rec_classic", 0);  /* no tesseract wired */
    set_rank(T, "rec_vlm", 1);      /* weights on disk; bridge stub */
    set_rank(T, "rec_moe", 0);
    set_rank(T, "rec_lang", 0);

    set_rank(T, "ver_conf", 1);    /* conf on hermetic */
    set_rank(T, "ver_gold", 1);    /* verify/promote path exists */
    set_rank(T, "ver_capsule", 2); /* schema-2 transport exists; OCR not bound */
    set_rank(T, "ver_tidy", 2);    /* vision/ocr taxonomy + goal */
    set_rank(T, "ver_proj", 1);    /* L3 debug memory exists; not doc templates */

    set_rank(T, "bench_omni", 0);
    set_rank(T, "beat_quality", 0);
    set_rank(T, "beat_long", 0);
    set_rank(T, "beat_cost", 1); /* local hermetic free; no real VLM loop yet */
    set_rank(T, "beat_audit", 2);
    set_rank(T, "beat_system", 1);
}

static void print_branch(const RoeTree *T, const char *branch) {
    size_t i;
    printf("\n## branch: %s\n", branch);
    printf("  %-22s %4s %4s  %s\n", "node", "rank", "max", "title");
    for (i = 0; i < T->n_nodes; i++) {
        const RoeTreeNode *n = &T->nodes[i];
        const char *mark;
        if (!n->active || strcmp(n->branch, branch) != 0) continue;
        mark = n->rank <= 0 ? "🔒" : (n->rank >= n->max_rank ? "✅" : "🟡");
        printf("  %s %-20s %2d/%-2d  %s\n", mark, n->id, n->rank, n->max_rank,
               n->title);
        if (n->desc[0]) printf("       %s\n", n->desc);
    }
}

static void print_next_unlocks(const RoeTree *T) {
    size_t i;
    int shown = 0;
    printf("\n## next unlockable (prereqs met, rank < max)\n");
    for (i = 0; i < T->n_nodes; i++) {
        const RoeTreeNode *n = &T->nodes[i];
        if (!n->active) continue;
        if (roe_tree_can_unlock(T, n->id)) {
            printf("  → %s (%s) rank %d→%d — %s\n", n->id, n->branch, n->rank,
                   n->rank + 1, n->title);
            if (++shown >= 12) break;
        }
    }
    if (!shown) printf("  (none — blocked on deeper prereqs)\n");
}

static void print_verdict(const RoeTreeSnapshot *S, const RoeTree *T) {
    const RoeTreeNode *q = roe_tree_get(T, "beat_quality");
    const RoeTreeNode *c = roe_tree_get(T, "beat_cost");
    const RoeTreeNode *a = roe_tree_get(T, "beat_audit");
    const RoeTreeNode *s = roe_tree_get(T, "beat_system");
    const RoeTreeNode *l = roe_tree_get(T, "beat_long");
    printf("\n## surpass Unlimited-OCR — multi-axis verdict\n");
    printf("  Opponent: Baidu Unlimited-OCR (~3B-A0.5B), OmniDocBench v1.5 ~93.2%% SOTA,\n");
    printf("            long-doc one-pass, flat long-output speed (R-SWA).\n");
    printf("  Quality beat (OmniDoc):     %s  rank %d/%d\n",
           q && q->rank >= 2 ? "PLAUSIBLE PATH" : "NOT YET", q ? q->rank : 0,
           q ? q->max_rank : 0);
    printf("  Long-doc speed/mem beat:    %s  rank %d/%d\n",
           l && l->rank >= 2 ? "PLAUSIBLE PATH" : "NOT YET", l ? l->rank : 0,
           l ? l->max_rank : 0);
    printf("  Cost/offline beat:          %s  rank %d/%d  ← ROE natural axis\n",
           c && c->rank >= 1 ? "STARTED" : "NOT YET", c ? c->rank : 0,
           c ? c->max_rank : 0);
    printf("  Audit/law beat:             %s  rank %d/%d  ← ROE natural axis\n",
           a && a->rank >= 2 ? "AHEAD ON PAPER" : "PARTIAL", a ? a->rank : 0,
           a ? a->max_rank : 0);
    printf("  Product-system beat:        %s  rank %d/%d  ← hybrid ROE+teacher\n",
           s && s->rank >= 1 ? "STARTED" : "NOT YET", s ? s->rank : 0,
           s ? s->max_rank : 0);
    printf("\n  Tree power now: %s\n", S->summary);
    printf("\n  HONEST CALL:\n");
    printf("  - Beating Unlimited on raw OmniDocBench quality soon: UNLIKELY without\n");
    printf("    full VLM teacher + layout/table stack + eval harness (huge).\n");
    printf("  - Surpassing it as a *regulated system* (cost, audit, local packs,\n");
    printf("    project memory, fail-closed): REALISTIC ROE path — use Unlimited as\n");
    printf("    teacher on miss, CERT only verified text, crush repeat token cost.\n");
    printf("  - Winning long-doc latency alone: need R-SWA-class decode or streaming\n");
    printf("    state machine; not hermetic glyphs.\n");
}

int main(void) {
    RoeTree T;
    RoeTreeSnapshot S;
    char dump[400];
    const char *cat = "artifacts/roe_ocr_skill_tree";
    size_t unlocked = 0, i;

    failures = checks = 0;
    printf("=== ROE skill tree: surpass Unlimited-OCR ===\n");
    check(seed_ocr_surpass_tree(&T) == 0, "seed OCR surpass tree");
    check(T.n_nodes >= 20, "enough nodes");
    apply_current_progress(&T);
    roe_tree_set_catalog(&T, cat);
    roe_tree_snapshot(&T, &S);
    for (i = 0; i < T.n_nodes; i++)
        if (T.nodes[i].rank > 0) unlocked++;

    print_branch(&T, "ocr");
    print_branch(&T, "shell");
    print_branch(&T, "meta");
    print_next_unlocks(&T);
    print_verdict(&S, &T);

    check(roe_tree_get(&T, "rec_hermetic")->rank >= 2, "hermetic OCR solid");
    check(roe_tree_get(&T, "beat_quality")->rank == 0, "quality beat still locked");
    check(roe_tree_get(&T, "beat_audit")->rank >= 1, "audit axis started");
    check(S.nodes_unlocked >= 8, "some progress nodes unlocked");
    check(roe_tree_save(&T) > 0, "persist skill tree");

    /* critical path print */
    printf("\n## recommended critical path (order)\n");
    printf("  1. perc_render     — PDF→image batch (pdf2image)\n");
    printf("  2. rec_vlm         — live Unlimited-OCR teacher (weights present)\n");
    printf("  3. ver_gold        — multi-check before CERT text\n");
    printf("  4. perc_layout     — reading order / blocks\n");
    printf("  5. perc_table      — tables (TEDS)\n");
    printf("  6. bench_omni      — OmniDocBench harness\n");
    printf("  7. ver_proj        — corpus templates → local free hits\n");
    printf("  8. beat_cost       — miss-only VLM; 90%%+ local on repeats\n");
    printf("  9. beat_system     — ROE goal tree + capsules ship as product\n");
    printf(" 10. beat_quality    — only after 1–6; optional moonshot\n");

    roe_tree_dump(&T, dump, sizeof dump);
    printf("\n  %s\n", dump);
    printf("  unlocked_nodes=%zu/%zu  catalog=%s\n", unlocked, T.n_nodes, cat);

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ROE_ASI_OCR_TREE_FAIL\n");
        return 1;
    }
    printf("ROE_ASI_OCR_TREE_PASS\n");
    return 0;
}
