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
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
#define TIMING_REPS 200
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

/* Unit k is a distinct permutation of the 8 symbols, drawn in factorial
   (Lehmer) order. Rotations were WRONG here: (x+k+1)%8 repeats every 8, so the
   previous "32 disjoint units" was really 8 functions with 4 copies each.
   8! = 40320 permutations exist; pairwise distinctness is asserted below
   rather than assumed. */
static void perm_of(int k, int out[SYM]) {
    int pool[SYM], i, n = SYM;
    long code = (long)k * 1237L; /* spread indices apart in Lehmer space */
    for (i = 0; i < SYM; i++) pool[i] = i;
    for (i = 0; i < SYM; i++) {
        long fact = 1, j;
        int pick;
        for (j = 2; j <= n - 1; j++) fact *= j;
        pick = (int)((code / (fact ? fact : 1)) % (n ? n : 1));
        code %= (fact ? fact : 1);
        out[i] = pool[pick];
        for (j = pick; j < n - 1; j++) pool[j] = pool[j + 1];
        n--;
    }
}

static int fn_k(int k, int x) {
    int p[SYM];
    perm_of(k, p);
    return p[x];
}

static unsigned long long behav_digest(int k) {
    unsigned long long h = 14695981039346656037ULL;
    int i;
    for (i = 0; i < SYM; i++) {
        h ^= (unsigned long long)fn_k(k, i);
        h *= 1099511628211ULL;
    }
    return h;
}

static const HybridCoverage *cov_of(const HybridAi *h, const char *unit) {
    size_t i;
    for (i = 0; i < h->coverage_count; i++)
        if (h->coverage[i].active && strcmp(h->coverage[i].unit, unit) == 0)
            return &h->coverage[i];
    return NULL;
}

static void rm_pack(const char *dir) {
    char q[512];
    snprintf(q, sizeof q, "%s/unit.cnb", dir);
    (void)remove(q);
    snprintf(q, sizeof q, "%s/manifest.cknow", dir);
    (void)remove(q);
    (void)rmdir(dir);
}

static int copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb"), *b;
    char buf[4096];
    size_t n;
    if (!a) return -1;
    b = fopen(to, "wb");
    if (!b) { fclose(a); return -1; }
    while ((n = fread(buf, 1, sizeof buf, a)) > 0)
        if (fwrite(buf, 1, n, b) != n) { fclose(a); fclose(b); return -1; }
    fclose(a);
    return fclose(b);
}

static int clone_pack(const char *src, const char *dst) {
    char q[512], w[512];
    (void)mkdir(dst, 0777);
    snprintf(q, sizeof q, "%s/unit.cnb", src);
    snprintf(w, sizeof w, "%s/unit.cnb", dst);
    if (copy_file(q, w) != 0) return -1;
    snprintf(q, sizeof q, "%s/manifest.cknow", src);
    snprintf(w, sizeof w, "%s/manifest.cknow", dst);
    return copy_file(q, w);
}

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

/* Exhaustive replay of the certified exemplars: serialization fidelity,
   NOT held-out generalisation. Labelled that way everywhere it is reported. */
static int replay_exact(const CnetBase *b, const char *name, int k) {
    BinaryTransformNetwork btn;
    Contract c;
    int i, ok = 0;
    memset(&btn, 0, sizeof btn);
    memset(&c, 0, sizeof c);
    if (cnb_get_unit(b, name, &btn, &c) != 0) return -1;
    for (i = 0; i < SYM; i++) {
        double probe[SYM];
        const double *out;
        oh(probe, i);
        out = btn_forward(&btn, probe);
        if (out && argmax(out) == fn_k(k, i)) ok++;
    }
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
    int pre_refused = 0, pre_total = 0, post_served = 0;
    int roundtrip_cov_exact = 0, distinct_ok = 1;
    unsigned long long digests[MAX_UNITS];
    int composition_supported = -1, composition_correct = 0;
    size_t capsule_bytes = 0;
    int u0_before[SYM], u0_after[SYM], u0_captured = 0, u0_drift = 0;
    double build_ms[N_SCALES], serve_ms[N_SCALES], lookup_ms[N_SCALES];
    int scale_units[N_SCALES];
    char first_name[96];

    printf("== knowledge accumulation bench (ASI = Artificial Specialized "
           "Intelligence; NOT AGI) ==\n");
    (void)mkdir("tmp_kab", 0777);
    (void)mkdir("logs", 0777);

    /* Assert the 32 functions really differ before training anything. */
    for (k = 0; k < MAX_UNITS; k++) digests[k] = behav_digest(k);
    for (k = 0; k < MAX_UNITS && distinct_ok; k++)
        for (i = k + 1; i < MAX_UNITS; i++)
            if (digests[k] == digests[i]) { distinct_ok = 0; break; }

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
        /* Lookup = REAL materialisation (cnb_get_unit + CNU1 seal verify),
           repeated for a stable mean. cnb_has_unit was a name scan and timed
           almost nothing. */
        {
            double l0 = now_ms();
            int reps = 0;
            while (reps < TIMING_REPS) {
                for (k = 0; k < N && reps < TIMING_REPS; k++) {
                    BinaryTransformNetwork b2;
                    Contract c2;
                    memset(&b2, 0, sizeof b2);
                    memset(&c2, 0, sizeof c2);
                    if (cnb_get_unit(&base, names[k], &b2, &c2) == 0) {
                        btn_free(&b2);
                        contract_free(&c2);
                    }
                    reps++;
                }
            }
            lookup_ms[si] = (now_ms() - l0) / TIMING_REPS;
        }
        {
            BinaryTransformNetwork b2;
            Contract c2;
            memset(&b2, 0, sizeof b2);
            memset(&c2, 0, sizeof c2);
            serve_ms[si] = 0.0;
            if (cnb_get_unit(&base, names[0], &b2, &c2) == 0) {
                double s0 = now_ms();
                for (i = 0; i < TIMING_REPS; i++) {
                    double probe[SYM];
                    oh(probe, i % SYM);
                    (void)btn_forward(&b2, probe);
                }
                serve_ms[si] = (now_ms() - s0) / TIMING_REPS;
                btn_free(&b2);
                contract_free(&c2);
            }
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
                if (replay_exact(&base, names[k], k) == SYM) isolation_ok++;

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
                /* Behaviourally meaningful baseline: the SAME query set must be
                   unservable on the fresh target before import, so the
                   post-import number measures transfer and not a target that
                   already knew the answer. (The old "accuracy 0.0" on an empty
                   base was tautological.) */
                pre_total++;
                if (replay_exact(&dst, names[k], k) < 0) pre_refused++;
                memset(&rep, 0, sizeof rep);
                roundtrip_units++;
                if (cnet_capsule_import(&dst, &dcov, dir, &rep) == 0) {
                    const HybridCoverage *a = cov_of(&cov, names[k]);
                    const HybridCoverage *b = cov_of(&dcov, names[k]);
                    if (replay_exact(&dst, names[k], k) == SYM) {
                        roundtrip_match++;
                        post_served++;
                    }
                    /* Compare the ACTUAL imported coverage against the source
                       record — rows, targets and port tags — not an unrelated
                       P("x")/P("y") probe that could never have matched. */
                    if (a && b && a->n_rows == b->n_rows &&
                        a->in_dim == b->in_dim && a->out_dim == b->out_dim &&
                        memcmp(a->rows, b->rows,
                               a->n_rows * a->in_dim * sizeof(double)) == 0 &&
                        a->targets && b->targets &&
                        memcmp(a->targets, b->targets,
                               a->n_rows * a->out_dim * sizeof(double)) == 0 &&
                        strcmp(a->input_port.tag, b->input_port.tag) == 0 &&
                        strcmp(a->goal_port.tag, b->goal_port.tag) == 0)
                        roundtrip_cov_exact++;
                }
                cnb_free(&dst);
                hybrid_ai_free(&dcov);
            }


            /* ---- negative controls ---------------------------------------- */
            {
                CnetBase bad;
                HybridAi bcov;
                CnetCapsuleReport rep;
                rm_pack("tmp_kab/bad");
                if (clone_pack("tmp_kab/pack_0", "tmp_kab/bad") == 0) {
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

                /* Truncating the payload is a real corruption a manifest
                   cannot describe; rewriting cnb_version by hand would now be
                   caught by the manifest checksum instead of the version
                   check, which is a different assertion. */
                rm_pack("tmp_kab/bad2");
                if (clone_pack("tmp_kab/pack_0", "tmp_kab/bad2") == 0) {
                    FILE *fp = fopen("tmp_kab/bad2/unit.cnb", "r+b");
                    if (fp) { if (ftruncate(fileno(fp), 16) != 0) { /* refuse anyway */ } fclose(fp); }
                }
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
        check(isolation_ok == N, "isolation: every unit individually correct");
        check(u0_captured == 1, "interference: baseline answers captured at N=1");
        check(interference_breaks == 0, "interference: unit 0 answers identical after 31 later adds");
        check(roundtrip_units > 0 && rt == 1.0,
              "round-trip: contract replay identical after transfer");
        check(corruption_rejected == 1, "negative control: corrupted capsule rejected");
        check(incompat_rejected == 1, "negative control: truncated payload rejected");
        check(ood_refused == N * (SYM - COV_ROWS), "OOD: uncovered inputs refused");
        check(ood_admitted == N * COV_ROWS, "OOD: covered inputs admitted");
        check(distinct_ok, "distinctness: all 32 unit functions differ pairwise");
        check(roundtrip_cov_exact == roundtrip_units,
              "round-trip: coverage rows+targets+tags bit-identical to source");
        check(pre_total > 0 && pre_refused == pre_total,
              "pre-import: same queries unservable on the fresh target");
        check(post_served == roundtrip_units,
              "post-import: same queries served after transfer");
    }

    /* ---------------------------- artifact ------------------------------ */
    js = fopen("logs/knowledge_accumulation_bench.json", "w");
    if (js) {
        int N = SCALE_N[N_SCALES - 1];
        fprintf(js, "{\n  \"schema_version\": 2,\n");
        fprintf(js, "  \"framing\": \"ASI = Artificial Specialized Intelligence; "
                    "NOT AGI; NOT artificial superintelligence\",\n");
        fprintf(js, "  \"symbols\": %d, \"coverage_rows_per_unit\": %d,\n", SYM, COV_ROWS);
        fprintf(js, "  \"isolation_units_correct\": %d, \"isolation_units_total\": %d,\n",
                isolation_ok, N);
        fprintf(js, "  \"interference_answer_drift\": %d, \"interference_probes\": %d,\n", interference_breaks, SYM);
        fprintf(js, "  \"contract_replay_units\": %d, \"contract_replay_match\": %d,\n",
                roundtrip_units, roundtrip_match);
        fprintf(js, "  \"corruption_rejected\": %d, \"incompatible_rejected\": %d,\n",
                corruption_rejected, incompat_rejected);
        fprintf(js, "  \"ood_refused\": %d, \"ood_admitted\": %d,\n",
                ood_refused, ood_admitted);
        fprintf(js, "  \"pre_import_unservable\": %d, \"pre_import_total\": %d, "
                    "\"post_import_served\": %d,\n", pre_refused, pre_total, post_served);
        fprintf(js, "  \"roundtrip_coverage_bit_identical\": %d,\n", roundtrip_cov_exact);
        fprintf(js, "  \"unit_functions\": \"distinct permutations of 8 symbols\", "
                    "\"pairwise_distinct\": %d,\n", distinct_ok);
        fprintf(js, "  \"contract_replay_note\": \"exhaustive replay of the certified "
                    "exemplars = serialization fidelity, NOT held-out generalisation\",\n");
        fprintf(js, "  \"trust_boundary\": \"local transfer object; unkeyed checksums "
                    "detect accident, NOT forgery — no signing\",\n");
        fprintf(js, "  \"timing_note\": \"lookup = cnb_get_unit materialise incl. CNU1 "
                    "verify; serve = btn_forward only; mean of %d reps, single run, "
                    "no variance estimate\",\n", TIMING_REPS);
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

    printf("\n  %-6s %-8s %-12s %-22s %s\n", "N", "units", "build_ms",
           "lookup_ms(materialise)", "serve_ms/query");
    for (si = 0; si < N_SCALES; si++)
        printf("  %-6d %-8d %-12.2f %-22.4f %.5f\n", SCALE_N[si], scale_units[si],
               build_ms[si], lookup_ms[si], serve_ms[si]);
    printf("\n  capsule_payload_bytes=%zu  composition=%s\n", capsule_bytes,
           composition_supported == 1 ? "measured" : "WITHHELD (planner does not chain disjoint tags)");
    printf("  contract replay = serialization fidelity, NOT generalisation\n");
    printf("  semantic_intent=WITHHELD  broad_intelligence=WITHHELD\n");
    (void)composition_correct;
    for (k = 0; k < MAX_UNITS; k++) {
        char d[128];
        snprintf(d, sizeof d, "tmp_kab/pack_%d", k);
        rm_pack(d);
    }
    rm_pack("tmp_kab/bad");
    rm_pack("tmp_kab/bad2");
    (void)rmdir("tmp_kab");

    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("KNOWLEDGE_ACCUMULATION_BENCH_PASS units=%d distinct=%d isolation=%d/%d "
               "replay=%d/%d cov_exact=%d pre_unservable=%d/%d ood_refused=%d "
               "json=logs/knowledge_accumulation_bench.json\n",
               SCALE_N[N_SCALES - 1], distinct_ok, isolation_ok,
               SCALE_N[N_SCALES - 1], roundtrip_match, roundtrip_units,
               roundtrip_cov_exact, pre_refused, pre_total, ood_refused);
        return 0;
    }
    printf("KNOWLEDGE_ACCUMULATION_BENCH_FAIL failures=%d\n", failures);
    return 1;
}
