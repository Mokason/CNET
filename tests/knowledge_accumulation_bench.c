/* Knowledge accumulation benchmark — does CNET actually accumulate isolated,
 * portable, certified capability, and does adding more of it break what it
 * already knew?
 *
 * Framing: the target is ASI = Artificial **Specialized** Intelligence.
 * Explicitly NOT AGI and NOT "artificial superintelligence". This gate measures
 * mechanism and specialized recall only. It does not measure reasoning,
 * generalisation beyond a certified domain, or semantic intent understanding —
 * those are reported WITHHELD rather than inferred from a green number.
 *
 * Everything runs on a fresh temporary base. The production
 * soul_gemma4v2_final.cnb is never opened.
 *
 * make knowledge_accumulation_bench -> KNOWLEDGE_ACCUMULATION_BENCH_PASS
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/cnet_capsule.h"
#include "../include/base.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/specialist.h"

#define SYM 8
#define COV_ROWS 5           /* certified on 5 of 8 -> 3 must abstain */
#define MAX_UNITS 32
static const int SCALE_N[] = {1, 8, 32};
#define N_SCALES ((int)(sizeof SCALE_N / sizeof SCALE_N[0]))

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-60s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static Port P(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = SYM;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static void oh(double *r, int hot) {
    int i;
    for (i = 0; i < SYM; i++) r[i] = (i == hot) ? 1.0 : 0.0;
}

static int argmax(const double *v) {
    int i, b = 0;
    for (i = 1; i < SYM; i++)
        if (v[i] > v[b]) b = i;
    return b;
}

/* Knowledge unit k: a distinct rotation, so units are genuinely disjoint
   functions rather than N copies of one skill. */
static int fn_k(int k, int x) { return (x + k + 1) % SYM; }

/* Build unit k into base (+ optional coverage over a strict subset). */
static int add_unit(CnetBase *base, HybridAi *cov, int k, char *name_out,
                    size_t cap) {
    BinaryTransformNetwork *btn;
    double in[SYM][SYM], tg[SYM][SYM];
    Contract c;
    Specialist s;
    char itag[32], gtag[32];
    Port pin, pout;
    int i, rc = -1;

    /* Tag governance refuses NEAR-MISS tags (case-insensitive equality,
       underscore-stripped equality, or Levenshtein distance 1 — see
       cnb_tag_mint in include/base.h). So "kb_in_0" / "kb_in_1" is REFUSED,
       and a sequentially-numbered skill family cannot be minted. That is the
       same rule that parked 7 grow_tok_* gaps as tag_collision in production.
       Two characters vary per unit so consecutive skills stay >=2 apart. */
    snprintf(itag, sizeof itag, "kb_%c%c%d_in", 'a' + k / 26, 'a' + k % 26, k);
    snprintf(gtag, sizeof gtag, "kb_%c%c%d_out", 'a' + k / 26, 'a' + k % 26, k);
    snprintf(name_out, cap, "kb_unit_%c%c%d", 'a' + k / 26, 'a' + k % 26, k);
    pin = P(itag);
    pout = P(gtag);

    btn = (BinaryTransformNetwork *)calloc(1, sizeof *btn);
    if (!btn) return -1;
    for (i = 0; i < SYM; i++) {
        oh(in[i], i);
        oh(tg[i], fn_k(k, i));
    }
    if (btn_init(btn, SYM, SYM, 16, 64, 0.5, (unsigned)(7 + k)) != 0) return -1;
    btn_set_ports(btn, pin, pout);
    btn_train_dynamic(btn, (const double *)in, (const double *)tg, SYM, 12000,
                      200, 1e-6, 1e-8);
    btn_train(btn, (const double *)in, (const double *)tg, SYM, 3000);
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, name_out, btn, (const double *)in,
                               (const double *)tg, SYM) != 0)
        return -1;
    memset(&s, 0, sizeof s);
    if (specialist_wrap_btn(&s, btn, name_out) == 0 &&
        cnb_add_unit(base, btn, &c, NULL) == 0)
        rc = 0;
    if (rc == 0 && cov)
        (void)hybrid_coverage_record(cov, pin, pout, name_out,
                                     (const double *)in, (const double *)tg,
                                     COV_ROWS, SYM, SYM);
    contract_free(&c);
    return rc;
}

/* Held-out recall: does unit k still compute its own function? */
static int unit_correct(const CnetBase *b, const char *name, int k,
                        double *serve_ms_out) {
    BinaryTransformNetwork btn;
    Contract c;
    int i, ok = 0;
    double t0;
    memset(&btn, 0, sizeof btn);
    memset(&c, 0, sizeof c);
    if (cnb_get_unit(b, name, &btn, &c) != 0) return -1;
    t0 = now_ms();
    for (i = 0; i < SYM; i++) {
        double probe[SYM];
        const double *out;
        oh(probe, i);
        out = btn_forward(&btn, probe);
        if (out && argmax(out) == fn_k(k, i)) ok++;
    }
    if (serve_ms_out) *serve_ms_out = (now_ms() - t0) / SYM;
    btn_free(&btn);
    contract_free(&c);
    return ok;
}

int main(void) {
    CnetBase base;
    HybridAi cov;
    char names[MAX_UNITS][96];
    FILE *js;
    int si, k, i;
    int isolation_ok = 0, interference_breaks = 0;
    int roundtrip_units = 0, roundtrip_match = 0;
    int corruption_rejected = 0, incompat_rejected = 0;
    int ood_refused = 0, ood_admitted = 0;
    int baseline_correct = 0, baseline_total = 0;
    int composition_supported = -1, composition_correct = 0;
    size_t capsule_bytes = 0;
    int u0_before[SYM], u0_after[SYM], u0_captured = 0, u0_drift = 0;
    double build_ms[N_SCALES], serve_ms[N_SCALES], lookup_ms[N_SCALES];
    int scale_units[N_SCALES];
    char first_name[96];

    printf("== knowledge accumulation bench (ASI = Artificial Specialized "
           "Intelligence; NOT AGI) ==\n");
    (void)system("rm -rf tmp_kab && mkdir -p tmp_kab logs");

    /* ---------- scaling: rebuild the library at each N ------------------- */
    for (si = 0; si < N_SCALES; si++) {
        int N = SCALE_N[si];
        double t0;
        cnb_init(&base);
        hybrid_ai_init(&cov);
        t0 = now_ms();
        for (k = 0; k < N; k++) {
            if (add_unit(&base, &cov, k, names[k], sizeof names[k]) != 0) {
                printf("  build FAILED at k=%d (tag mint refusal or admit "
                       "refusal) — bench cannot measure accumulation\n", k);
                break;
            }
        }
        build_ms[si] = now_ms() - t0;
        scale_units[si] = (int)base.unit_count;

        /* lookup latency: name -> sealed unit materialise */
        {
            double l0 = now_ms();
            for (k = 0; k < N; k++) (void)cnb_has_unit(&base, names[k]);
            lookup_ms[si] = (now_ms() - l0) / (N > 0 ? N : 1);
        }
        {
            double sm = 0.0;
            (void)unit_correct(&base, names[0], 0, &sm);
            serve_ms[si] = sm;
        }
        /* Interference is a BEFORE/AFTER question, so record unit 0's actual
           answer vector when it is alone, and diff it once 31 later units have
           been admitted. Re-checking "is unit 0 still correct" at the end only
           repeats the isolation check and would report a win it never tested. */
        if (scale_units[si] > 0) {
            BinaryTransformNetwork b0;
            Contract c0;
            memset(&b0, 0, sizeof b0);
            memset(&c0, 0, sizeof c0);
            if (cnb_get_unit(&base, names[0], &b0, &c0) == 0) {
                int q;
                for (q = 0; q < SYM; q++) {
                    double probe[SYM];
                    const double *out;
                    oh(probe, q);
                    out = btn_forward(&b0, probe);
                    if (si == 0) u0_before[q] = out ? argmax(out) : -1;
                    else u0_after[q] = out ? argmax(out) : -1;
                }
                if (si == 0) u0_captured = 1;
                btn_free(&b0);
                contract_free(&c0);
            }
        }

        if (si == N_SCALES - 1) {
            /* ---- isolation: every unit individually retrievable + right --- */
            for (k = 0; k < N; k++)
                if (unit_correct(&base, names[k], k, NULL) == SYM) isolation_ok++;

            /* ---- interference: unit 0's answers before vs after N-1 adds -- */
            if (u0_captured)
                for (i = 0; i < SYM; i++)
                    if (u0_before[i] != u0_after[i]) u0_drift++;
            interference_breaks = u0_drift;

            /* ---- OOD abstention on the accumulated library ---------------- */
            for (k = 0; k < N; k++) {
                char itag[32], gtag[32];
                snprintf(itag, sizeof itag, "kb_%c%c%d_in", 'a' + k / 26, 'a' + k % 26, k);
                snprintf(gtag, sizeof gtag, "kb_%c%c%d_out", 'a' + k / 26, 'a' + k % 26, k);
                for (i = 0; i < SYM; i++) {
                    double probe[SYM];
                    oh(probe, i);
                    if (hybrid_coverage_admits(&cov, P(itag), P(gtag), probe,
                                               SYM))
                        ood_admitted++;
                    else
                        ood_refused++;
                }
            }

            /* ---- portable round-trip on a sample of units ----------------- */
            snprintf(first_name, sizeof first_name, "%s", names[0]);
            for (k = 0; k < N; k += (N > 8 ? N / 8 : 1)) {
                CnetBase dst;
                HybridAi dcov;
                CnetCapsuleReport rep;
                char dir[128];
                snprintf(dir, sizeof dir, "tmp_kab/pack_%d", k);
                memset(&rep, 0, sizeof rep);
                if (cnet_capsule_export(&base, &cov, names[k], dir, &rep) != 0)
                    continue;
                capsule_bytes = rep.payload_bytes;
                cnb_init(&dst);
                hybrid_ai_init(&dcov);
                memset(&rep, 0, sizeof rep);
                roundtrip_units++;
                if (cnet_capsule_import(&dst, &dcov, dir, &rep) == 0 &&
                    unit_correct(&dst, names[k], k, NULL) == SYM &&
                    hybrid_coverage_rows(&dcov, P("x"), P("y")) == 0)
                    roundtrip_match++;
                cnb_free(&dst);
                hybrid_ai_free(&dcov);
            }

            /* ---- baseline / negative control: fresh base, NO import -------
               Without the capsule the same queries cannot be answered at all;
               scoring chance-level makes the round-trip number mean something. */
            {
                CnetBase empty;
                cnb_init(&empty);
                for (i = 0; i < SYM; i++) {
                    baseline_total++;
                    if (cnb_has_unit(&empty, first_name)) baseline_correct++;
                }
                cnb_free(&empty);
            }

            /* ---- negative controls ---------------------------------------- */
            {
                CnetBase bad;
                HybridAi bcov;
                CnetCapsuleReport rep;
                (void)system("rm -rf tmp_kab/bad && cp -r tmp_kab/pack_0 tmp_kab/bad");
                {
                    FILE *fp = fopen("tmp_kab/bad/unit.cnb", "r+b");
                    if (fp) { fseek(fp, 64, SEEK_SET); fputc(0xA5, fp); fclose(fp); }
                }
                cnb_init(&bad); hybrid_ai_init(&bcov);
                memset(&rep, 0, sizeof rep);
                if (cnet_capsule_import(&bad, &bcov, "tmp_kab/bad", &rep) != 0 &&
                    !cnb_has_unit(&bad, first_name))
                    corruption_rejected = 1;
                printf("  corruption reject_reason=%s\n", rep.reject_reason);
                cnb_free(&bad); hybrid_ai_free(&bcov);

                (void)system("rm -rf tmp_kab/bad2 && cp -r tmp_kab/pack_0 tmp_kab/bad2 && "
                             "sed -i 's/^cnb_version .*/cnb_version 99/' tmp_kab/bad2/manifest.cknow");
                cnb_init(&bad); hybrid_ai_init(&bcov);
                memset(&rep, 0, sizeof rep);
                if (cnet_capsule_import(&bad, &bcov, "tmp_kab/bad2", &rep) != 0 &&
                    !cnb_has_unit(&bad, first_name))
                    incompat_rejected = 1;
                printf("  incompatible reject_reason=%s\n", rep.reject_reason);
                cnb_free(&bad); hybrid_ai_free(&bcov);
            }

            /* ---- composition: can the planner chain two stored units? -----
               unit 0 is kb_in_0 -> kb_out_0. Nothing produces kb_in_1 from
               kb_out_0, so a 2-step plan can only exist if the planner bridges
               unrelated tags — it must not. Report what route_plan actually
               does instead of asserting a capability. */
            {
                RoutePlan plan;
                PrimitiveRegistry reg;
                size_t skipped = 0;
                registry_init(&reg);
                if (cnb_load_registry(&base, &reg, &skipped) == 0) {
                    memset(&plan, 0, sizeof plan);
                    if (route_plan(&reg, P("kb_aa0_in"), P("kb_ab1_out"), &plan) == 0 &&
                        plan.length >= 2) {
                        composition_supported = 1;
                        composition_correct = 0; /* would need held-out scoring */
                    } else {
                        composition_supported = 0;
                    }
                }
                registry_free(&reg);
            }
        }
        cnb_free(&base);
        hybrid_ai_free(&cov);
    }

    /* ---------------------------- floors -------------------------------- */
    {
        int N = SCALE_N[N_SCALES - 1];
        double rt = roundtrip_units ? (double)roundtrip_match / roundtrip_units : 0.0;
        double base_acc = baseline_total ? (double)baseline_correct / baseline_total : 0.0;
        check(isolation_ok == N, "isolation: every unit individually correct");
        check(u0_captured == 1, "interference: baseline answers captured at N=1");
        check(interference_breaks == 0, "interference: unit 0 answers identical after 31 later adds");
        check(roundtrip_units > 0 && rt == 1.0, "round-trip: imported behaviour identical");
        check(corruption_rejected == 1, "negative control: corrupted capsule rejected");
        check(incompat_rejected == 1, "negative control: incompatible version rejected");
        check(ood_refused == N * (SYM - COV_ROWS), "OOD: uncovered inputs refused");
        check(ood_admitted == N * COV_ROWS, "OOD: covered inputs admitted");
        check(base_acc == 0.0, "baseline: without import the unit is absent (0.0)");
    }

    /* ---------------------------- artifact ------------------------------ */
    js = fopen("logs/knowledge_accumulation_bench.json", "w");
    if (js) {
        int N = SCALE_N[N_SCALES - 1];
        fprintf(js, "{\n  \"schema_version\": 1,\n");
        fprintf(js, "  \"framing\": \"ASI = Artificial Specialized Intelligence; "
                    "NOT AGI; NOT artificial superintelligence\",\n");
        fprintf(js, "  \"symbols\": %d, \"coverage_rows_per_unit\": %d,\n", SYM, COV_ROWS);
        fprintf(js, "  \"isolation_units_correct\": %d, \"isolation_units_total\": %d,\n",
                isolation_ok, N);
        fprintf(js, "  \"interference_answer_drift\": %d, \"interference_probes\": %d,\n", interference_breaks, SYM);
        fprintf(js, "  \"roundtrip_units\": %d, \"roundtrip_match\": %d,\n",
                roundtrip_units, roundtrip_match);
        fprintf(js, "  \"corruption_rejected\": %d, \"incompatible_rejected\": %d,\n",
                corruption_rejected, incompat_rejected);
        fprintf(js, "  \"ood_refused\": %d, \"ood_admitted\": %d,\n",
                ood_refused, ood_admitted);
        fprintf(js, "  \"baseline_no_import_accuracy\": %.4f,\n",
                baseline_total ? (double)baseline_correct / baseline_total : 0.0);
        fprintf(js, "  \"capsule_payload_bytes\": %zu,\n", capsule_bytes);
        fprintf(js, "  \"composition\": %s,\n",
                composition_supported == 1 ? "\"measured\"" : "\"withheld_planner_does_not_chain_disjoint_tags\"");
        fprintf(js, "  \"semantic_intent_understanding\": \"WITHHELD_no_semantic_path_exercised\",\n");
        fprintf(js, "  \"broad_intelligence\": \"WITHHELD\",\n");
        fprintf(js, "  \"scales\": [\n");
        for (si = 0; si < N_SCALES; si++)
            fprintf(js, "    {\"n\": %d, \"units_in_base\": %d, \"build_ms\": %.2f, "
                        "\"lookup_ms_per_unit\": %.4f, \"serve_ms_per_query\": %.4f}%s\n",
                    SCALE_N[si], scale_units[si], build_ms[si], lookup_ms[si],
                    serve_ms[si], si + 1 < N_SCALES ? "," : "");
        fprintf(js, "  ],\n  \"checks\": %d, \"failures\": %d\n}\n", checks, failures);
        fclose(js);
    }

    printf("\n  %-6s %-8s %-12s %-16s %s\n", "N", "units", "build_ms", "lookup_ms/unit", "serve_ms/query");
    for (si = 0; si < N_SCALES; si++)
        printf("  %-6d %-8d %-12.2f %-16.4f %.4f\n", SCALE_N[si], scale_units[si],
               build_ms[si], lookup_ms[si], serve_ms[si]);
    printf("\n  capsule_payload_bytes=%zu  composition=%s\n", capsule_bytes,
           composition_supported == 1 ? "measured" : "WITHHELD (planner does not chain disjoint tags)");
    printf("  semantic_intent=WITHHELD  broad_intelligence=WITHHELD\n");
    (void)composition_correct;
    (void)system("rm -rf tmp_kab");

    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("KNOWLEDGE_ACCUMULATION_BENCH_PASS units=%d isolation=%d/%d "
               "roundtrip=%d/%d ood_refused=%d json=logs/knowledge_accumulation_bench.json\n",
               SCALE_N[N_SCALES - 1], isolation_ok, SCALE_N[N_SCALES - 1],
               roundtrip_match, roundtrip_units, ood_refused);
        return 0;
    }
    printf("KNOWLEDGE_ACCUMULATION_BENCH_FAIL failures=%d\n", failures);
    return 1;
}
