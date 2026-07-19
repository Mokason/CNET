/* sparse_kv_exec: hermetic EXECUTION gate for sparse per-specialist KV
 * routing on CNET's OWN model runtime — the cce_gguf_qwen2 forward's KV
 * cache attention path (the same seam the mining oracle uses). The selector
 * is the ONE in src/cce/cce_sparse_kv.c; this gate proves it executes on a
 * real KV path, not just in isolation.
 *
 * Fixture: the shared tiny qwen2-arch GGUF (tests/tiny_model_fixture.h)
 * with TL_CTX=320 and rigged layer-0 attention so three PLANTED needle
 * tokens are guaranteed DOMINANT heavy hitters:
 *   - every token's embedding has dim0 = 0 and dim1 = 1 exactly;
 *   - the needle token's embedding has dim0 = 6 (large);
 *   - layer-0 K reads ONLY input dim0 (gain 4) onto the rope pair (1,3), so
 *     every background K row is exactly the zero vector and every needle K
 *     row is large;
 *   - layer-0 Q reads ONLY input dim1 onto the same rope pair, so every
 *     query has a positive component along the needle direction.
 *   Rope pair (1,3) turns at base^(-1/2) ~= 0.00447 rad/pos (TL_ROPE=50000),
 *   so over 320 positions the relative rotation stays under pi/2: needle
 *   q.k scores land at ~15..28 while ALL background layer-0 scores are
 *   exactly 0 — layer-0 attention output is needle-carried (softmax mass
 *   >= 0.9999) whenever the needles are attended, and collapses to the
 *   background average if they are dropped.
 *   Layer-1 V (and its bias) is zeroed so layer-1's KV restriction is
 *   output-inert: its scores, selection, budget, and in-situ checks still
 *   execute for real, but the end-to-end decode argmax agreement isolates
 *   ONE question — did the sparse path keep the planted needles' signal.
 *   (With random layer-1 V the fixture's pre-/post-needle value clusters
 *   make subset attention shift the output mix for reasons unrelated to
 *   needle retention; measured before this rig: 0/32 agreement.)
 *   Everything else stays fully random.
 *
 * Pins:
 *   (a) OFF == ON@1.0 BIT-IDENTICAL (prefill + every decode step), with the
 *       selector actually running (tap fires, full selection each step);
 *   (b) budget 0.25 on a 288-token synthetic context: budget respected at
 *       every (layer, head, step); the planted needles stay attended (all
 *       three selected at layer 0, both heads, all 32 decode steps); the
 *       in-situ top-scoring row is selected at every decode fire (both
 *       layers); decode argmax agreement vs full KV >= the stated threshold;
 *       and the sparse logits actually DIFFER from full KV (the restriction
 *       executes, the identity checks are not vacuous);
 *   (b2) degenerate regimes (SHORT context ~12 rows @0.25; moderate ~100
 *       rows @0.10): the wiring's re-derived positional shares keep the
 *       query's own row selected at every step (recent >= 1) and leave the
 *       heavy-hitter score pass budget whenever target > initial+recent;
 *   (c) malformed budgets refused: setter (negative, >1, NaN) and env knob
 *       CNET_SPARSE_KV (garbage, >1) both refuse; "0"/valid fractions load;
 *   (d) OFF restore returns bit-identical to the baseline, and the tap can
 *       never fire while OFF.
 *
 * Terminal marker: SPARSE_KV_EXEC_PASS
 */

#define TL_CTX 320
#include "tiny_model_fixture.h"
#include "../include/cnet_platform.h"

#include <math.h>
#include <stdint.h>

#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_sparse_kv.h"

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, msg) do { \
    g_checks++; \
    if (cond) { printf("  ok  %s\n", (msg)); } \
    else { g_fails++; printf("  FAIL %s\n", (msg)); } } while (0)

/* ---- context plan ---- */
#define PREFILL 256
#define DSTEPS  32
#define NRUNS   (1 + DSTEPS)            /* prefill logits + each decode step */
#define NEEDLE_ID 7
static const int g_needle_pos[3] = { 101, 149, 203 };   /* mid-context: not
    initial (<4), not stride (64/128/192), outside every decode step's
    recent-32 window (decode queries sit at pos 256..287) */

/* stated decode argmax-agreement threshold for budget 0.25 (measured on this
   fixture: 32/32 = 1.00; stated with margin for FP/platform variation) */
#define AGREE_THRESH 0.90

/* ---- tap bookkeeping ---- */
static int g_tap_mode = 0;      /* 0=off-run guard, 1=expect full, 2=budget,
                                   3=degenerate-regime share invariants */
static long g_fires = 0;
static long g_full_sel_viol = 0;
static long g_budget_viol = 0;
static long g_needle_miss = 0;
static long g_argmax_miss = 0;
/* mode 3: the wiring re-derives initial/recent against ceil(frac*rows);
   check its two invariants at EVERY fire (prefill and decode) */
static float g_deg_frac = 0.0f;
static long g_deg_qrow_miss = 0;   /* query's own row (rows-1) not selected */
static long g_deg_nofar = 0;       /* no row outside both positional zones
                                      although target > initial+recent */

static int in_sel(const int *sel, int n, int rel) {
    int i;
    for (i = 0; i < n; i++) if (sel[i] == rel) return 1;
    return 0;
}

static void skv_tap(int layer, int head, int q_pos, int jmin,
                    const float *scores, int n_rows, const int *sel,
                    int n_sel, void *u) {
    (void)head; (void)u;
    g_fires++;
    if (g_tap_mode == 1) {
        if (n_sel != n_rows) g_full_sel_viol++;
        return;
    }
    if (g_tap_mode == 3) {
        /* mirror the wiring's re-derived budget arithmetic exactly:
           recent = min(32, max(1, target/2)), initial = min(4, target/4),
           then shrink initial first if over budget */
        int target = (int)ceilf(g_deg_frac * (float)n_rows);
        int recent, initial;
        if (target < 1) target = 1;
        if (target > n_rows) target = n_rows;
        recent = target / 2;
        if (recent < 1) recent = 1;
        if (recent > 32) recent = 32;
        initial = target / 4;
        if (initial > 4) initial = 4;
        if (initial + recent > target) initial = target - recent;
        /* invariant 1: recent >= 1 keeps the query's own row selected */
        if (!in_sel(sel, n_sel, n_rows - 1)) g_deg_qrow_miss++;
        /* invariant 2: initial+recent <= target leaves the score pass
           budget — some selected row must lie outside both zones */
        if (target > initial + recent) {
            int i2, far = 0;
            for (i2 = 0; i2 < n_sel; i2++)
                if (sel[i2] >= initial && sel[i2] < n_rows - recent)
                    { far = 1; break; }
            if (!far) g_deg_nofar++;
        }
        return;
    }
    if (g_tap_mode != 2) return;
    {
        int target = (n_rows + 3) / 4;      /* ceil(0.25 * n_rows) */
        if (target < 1) target = 1;
        if (n_sel > target) g_budget_viol++;
    }
    if (q_pos >= PREFILL) {
        int i;
        if (layer == 0) {
            for (i = 0; i < 3; i++) {
                int rel = g_needle_pos[i] - jmin;
                if (rel < 0 || rel >= n_rows || !in_sel(sel, n_sel, rel))
                    g_needle_miss++;
            }
        }
        {   /* in-situ heavy hitter: the top-scoring visible row is kept */
            int am = 0;
            for (i = 1; i < n_rows; i++) if (scores[i] > scores[am]) am = i;
            if (!in_sel(sel, n_sel, am)) g_argmax_miss++;
        }
    }
}

/* ---- fixture rigging (see file header) ---- */
static void rig_weights(tl_weights *w) {
    int v;
    for (v = 0; v < TL_V; v++) {
        w->emb[v][0] = 0.0f;
        w->emb[v][1] = 1.0f;
    }
    w->emb[NEEDLE_ID][0] = 6.0f;
    /* layer-0 Q: only input dim1, onto rope pair (1,3) of both heads */
    memset(w->q_w[0], 0, sizeof w->q_w[0]);
    memset(w->q_b[0], 0, sizeof w->q_b[0]);
    w->q_w[0][1][1] = 1.0f; w->q_w[0][3][1] = 1.0f;   /* head 0 */
    w->q_w[0][5][1] = 1.0f; w->q_w[0][7][1] = 1.0f;   /* head 1 */
    /* layer-0 K: only input dim0 (gain 4), onto rope pair (1,3) of the KV
       head — needle scores ~15..28, background scores exactly 0 */
    memset(w->k_w[0], 0, sizeof w->k_w[0]);
    memset(w->k_b[0], 0, sizeof w->k_b[0]);
    w->k_w[0][1][0] = 4.0f; w->k_w[0][3][0] = 4.0f;
    /* layer-1 V zeroed: its attention output is exactly 0 in both runs, so
       decode argmax agreement isolates layer-0 needle retention (layer-1
       scores/selection still execute for real) */
    memset(w->v_w[1], 0, sizeof w->v_w[1]);
    memset(w->v_b[1], 0, sizeof w->v_b[1]);
}

/* ---- deterministic token plan (background ids 2..6 exclude the needle) ---- */
static int g_ctx_tokens[PREFILL];
static int g_dec_tokens[DSTEPS];

static void build_tokens(void) {
    int i;
    for (i = 0; i < PREFILL; i++) g_ctx_tokens[i] = 2 + (i * 3 + 1) % 5;
    for (i = 0; i < 3; i++) g_ctx_tokens[g_needle_pos[i]] = NEEDLE_ID;
    for (i = 0; i < DSTEPS; i++) g_dec_tokens[i] = 2 + (i * 7 + 2) % 5;
}

/* one protocol run: batch prefill + DSTEPS teacher-forced decode steps;
   logits of the prefill call land in out[0], step i in out[1 + i] */
static int run_protocol(cce_gguf_qwen2 *m, float out[NRUNS][TL_V]) {
    int i;
    m->cur_pos = 0;
    if (cce_gguf_qwen2_forward(m, g_ctx_tokens, PREFILL, out[0], TL_V) != CCE_OK)
        return 0;
    for (i = 0; i < DSTEPS; i++)
        if (cce_gguf_qwen2_forward(m, &g_dec_tokens[i], 1, out[1 + i],
                                   TL_V) != CCE_OK)
            return 0;
    return 1;
}

/* shortened protocol for the degenerate regimes: prefill npre tokens from
   the standard context plan, then ndec teacher-forced decode steps (logits
   discarded — mode-3 tap invariants are the assertion) */
static int run_steps(cce_gguf_qwen2 *m, int npre, int ndec) {
    float lg[TL_V];
    int i;
    m->cur_pos = 0;
    if (cce_gguf_qwen2_forward(m, g_ctx_tokens, npre, lg, TL_V) != CCE_OK)
        return 0;
    for (i = 0; i < ndec; i++)
        if (cce_gguf_qwen2_forward(m, &g_dec_tokens[i], 1, lg, TL_V) != CCE_OK)
            return 0;
    return 1;
}

static int argmax_v(const float *lg) {
    int v, am = 0;
    for (v = 1; v < TL_V; v++) if (lg[v] > lg[am]) am = v;
    return am;
}

int main(void) {
    static tl_weights w;
    static float base[NRUNS][TL_V], on1[NRUNS][TL_V];
    static float sp[NRUNS][TL_V], off2[NRUNS][TL_V];
    const char *gguf_path = "sparse_kv_exec_fixture.gguf";
    cce_gguf_qwen2 *m = NULL;

    printf("=== sparse_kv_exec: selector on the real cce_gguf_qwen2 KV path "
           "===\n");

    tl_gen(&w, 0);
    rig_weights(&w);
    build_tokens();
    {
        tl_entry ents[64];
        int n = tl_entries(&w, ents, 0);
        tl_write_gguf(gguf_path, ents, n);
    }

    CHECK(cce_gguf_load_qwen2(&m, gguf_path) == CCE_OK, "fixture loads");
    if (!m) { printf("FATAL: no model\n"); return 1; }
    CHECK(m->max_ctx >= PREFILL + DSTEPS, "KV window covers the context");
    CHECK(m->sparse_kv_fraction == 0.0f, "sparse KV defaults OFF");

    /* (c) malformed budgets refused, state unchanged */
    CHECK(cce_gguf_qwen2_set_sparse_kv(NULL, 0.5f) == CCE_ERR_INVALID_ARG,
          "NULL model refused");
    CHECK(cce_gguf_qwen2_set_sparse_kv(m, -0.25f) == CCE_ERR_INVALID_ARG,
          "negative budget refused");
    CHECK(cce_gguf_qwen2_set_sparse_kv(m, 1.5f) == CCE_ERR_INVALID_ARG,
          "budget > 1 refused");
    CHECK(cce_gguf_qwen2_set_sparse_kv(m, nanf("")) == CCE_ERR_INVALID_ARG,
          "NaN budget refused");
    CHECK(m->sparse_kv_fraction == 0.0f, "refusals left the knob unset");

    /* (d-pre) baseline OFF run; the tap must never fire while OFF */
    g_tap_mode = 0; g_fires = 0;
    cce_gguf_set_sparse_kv_tap(skv_tap, NULL);
    CHECK(run_protocol(m, base), "baseline (OFF) protocol runs");
    CHECK(g_fires == 0, "tap never fires while sparse KV is OFF");

    /* (a) ON @ 1.0 == OFF, bit-identical, with the selector really running */
    g_tap_mode = 1; g_fires = 0; g_full_sel_viol = 0;
    CHECK(cce_gguf_qwen2_set_sparse_kv(m, 1.0f) == CCE_OK, "enable @ 1.0");
    CHECK(run_protocol(m, on1), "ON@1.0 protocol runs");
    CHECK(g_fires > 0, "selector executed at 1.0 (tap fired)");
    CHECK(g_full_sel_viol == 0, "budget 1.0 keeps every visible row");
    CHECK(memcmp(base, on1, sizeof base) == 0,
          "OFF == ON@1.0 BIT-IDENTICAL (prefill + all 32 decode steps)");

    /* (b) budget 0.25 on the 288-token context with planted needles */
    g_tap_mode = 2; g_fires = 0; g_budget_viol = 0;
    g_needle_miss = 0; g_argmax_miss = 0;
    CHECK(cce_gguf_qwen2_set_sparse_kv(m, 0.25f) == CCE_OK, "enable @ 0.25");
    CHECK(run_protocol(m, sp), "ON@0.25 protocol runs");
    CHECK(g_fires > 0, "selector executed at 0.25 (tap fired)");
    CHECK(g_budget_viol == 0,
          "budget respected at every (layer, head, step): n_sel <= "
          "ceil(0.25 * visible rows)");
    CHECK(g_needle_miss == 0,
          "planted heavy-hitter needles stay attended (layer 0, both heads, "
          "all decode steps)");
    CHECK(g_argmax_miss == 0,
          "in-situ top-scoring KV row selected at every decode fire (both "
          "layers)");
    CHECK(memcmp(base, sp, sizeof base) != 0,
          "0.25 output differs from full KV (restriction really executes)");
    {
        int i, agree = 0;
        double frac;
        for (i = 0; i < DSTEPS; i++)
            if (argmax_v(base[1 + i]) == argmax_v(sp[1 + i])) agree++;
        frac = (double)agree / (double)DSTEPS;
        printf("  (decode argmax agreement @0.25 vs full KV: %d/%d = %.3f, "
               "stated threshold %.2f)\n", agree, DSTEPS, frac, AGREE_THRESH);
        CHECK(frac >= AGREE_THRESH,
              "decode argmax agreement above the stated threshold");
    }

    /* (b2) degenerate regimes: the selector fills initial rows then the
       recent window BEFORE its heavy-hitter score pass, so positional
       shares sized for the default ~20% budget would spend a small target
       entirely on the OLDEST rows (fraction 0.25 at tc=12 selected {0,1,2};
       0.10 at tc=100 selected {0..3,90..95}) — evicting the query's own
       row. The wiring re-derives the shares per target; the mode-3 tap
       asserts at every fire that the query row is selected and that the
       score pass got budget whenever target > initial+recent. */
    {
        /* SHORT context: prefill 8 + 4 decode steps -> visible rows 9..12
           at 0.25 (tc=12 -> target=3: recent=1, initial=0, 2 score-chosen) */
        g_tap_mode = 3; g_deg_frac = 0.25f;
        g_fires = 0; g_deg_qrow_miss = 0; g_deg_nofar = 0;
        CHECK(cce_gguf_qwen2_set_sparse_kv(m, 0.25f) == CCE_OK,
              "enable @ 0.25 (short context)");
        CHECK(run_steps(m, 8, 4), "short-context protocol runs");
        CHECK(g_fires > 0, "selector executed on the short context");
        CHECK(g_deg_qrow_miss == 0,
              "short ctx @0.25: query's own row selected at every step");
        CHECK(g_deg_nofar == 0,
              "short ctx @0.25: score pass got budget whenever target > "
              "initial+recent");
    }
    {
        /* moderate context: prefill 68 + 32 decode steps -> visible rows up
           to 100 at 0.10 (tc=100 -> target=10: recent=5, initial=2) */
        g_tap_mode = 3; g_deg_frac = 0.10f;
        g_fires = 0; g_deg_qrow_miss = 0; g_deg_nofar = 0;
        CHECK(cce_gguf_qwen2_set_sparse_kv(m, 0.10f) == CCE_OK,
              "enable @ 0.10 (moderate context)");
        CHECK(run_steps(m, 68, DSTEPS), "moderate-context protocol runs");
        CHECK(g_fires > 0, "selector executed on the moderate context");
        CHECK(g_deg_qrow_miss == 0,
              "moderate ctx @0.10: query's own row selected at every step");
        CHECK(g_deg_nofar == 0,
              "moderate ctx @0.10: score pass got budget whenever target > "
              "initial+recent");
    }

    /* (d) OFF restore: bit-identical to the baseline again */
    cce_gguf_set_sparse_kv_tap(NULL, NULL);
    CHECK(cce_gguf_qwen2_set_sparse_kv(m, 0.0f) == CCE_OK, "disable (0.0)");
    CHECK(run_protocol(m, off2), "restored-OFF protocol runs");
    CHECK(memcmp(base, off2, sizeof base) == 0,
          "restored OFF bit-identical to baseline (no state leak)");
    cce_gguf_qwen2_free(m); m = NULL;

    /* (c) env knob at load: malformed refuses, valid loads, "0" stays OFF */
    cnet_setenv("CNET_SPARSE_KV", "banana", 1);
    CHECK(cce_gguf_load_qwen2(&m, gguf_path) != CCE_OK,
          "CNET_SPARSE_KV=banana refuses the load");
    cnet_setenv("CNET_SPARSE_KV", "1.5", 1);
    CHECK(cce_gguf_load_qwen2(&m, gguf_path) != CCE_OK,
          "CNET_SPARSE_KV=1.5 refuses the load");
    cnet_setenv("CNET_SPARSE_KV", "-0.1", 1);
    CHECK(cce_gguf_load_qwen2(&m, gguf_path) != CCE_OK,
          "CNET_SPARSE_KV=-0.1 refuses the load");
    cnet_setenv("CNET_SPARSE_KV", "0", 1);
    m = NULL;
    CHECK(cce_gguf_load_qwen2(&m, gguf_path) == CCE_OK &&
          m && m->sparse_kv_fraction == 0.0f,
          "CNET_SPARSE_KV=0 loads with sparse KV OFF");
    if (m) { cce_gguf_qwen2_free(m); m = NULL; }
    cnet_setenv("CNET_SPARSE_KV", "0.25", 1);
    CHECK(cce_gguf_load_qwen2(&m, gguf_path) == CCE_OK &&
          m && m->sparse_kv_fraction == 0.25f,
          "CNET_SPARSE_KV=0.25 loads with the knob set");
    cnet_unsetenv("CNET_SPARSE_KV");
    if (m) {
        /* the env-armed model must match the setter-armed run exactly */
        static float envsp[NRUNS][TL_V];
        CHECK(run_protocol(m, envsp), "env-armed protocol runs");
        CHECK(memcmp(sp, envsp, sizeof sp) == 0,
              "env knob == setter @0.25 bit-identical");
        cce_gguf_qwen2_free(m); m = NULL;
    }

    remove(gguf_path);
    printf("=== %d checks, %d failures ===\n", g_checks, g_fails);
    if (g_fails == 0) {
        printf("SPARSE_KV_EXEC_PASS\n");
        return 0;
    }
    return 1;
}
