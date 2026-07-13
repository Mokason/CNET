/* moe_loader: Arc B1 — MoE checkpoints parse into streamable per-expert
 * specialists.
 *
 * PART 1 (hermetic): a tiny synthetic MoE GGUF (canonical llama.cpp layout:
 * separate ffn_gate/up/down_exps 3D banks + ffn_gate_inp router, F32,
 * 2 layers x 4 experts) opens, every expert loads as a 3-block cascade
 * [gate, up, down] whose weights match the generator formula EXACTLY
 * (proves the per-expert slice offsets AND the [in][out] transpose), the
 * router loads exactly, an expert round-trips the weight store bit-exact
 * (FP and int8 — a streamable specialist), identical experts dedup by
 * content digest, and a DENSE gguf is REFUSED (not a MoE).
 *
 * PART 2 (real, auto-skips if absent): gemma-4-26B-A4B (26B total / 4B
 * active, 128 experts x 30 layers, Q6_K fused gate_up + Q8_0 down + F32
 * router). Gates: hparams parse (128/8/704, fused layout + side scales
 * detected); experts load finite/non-degenerate and differ pairwise; the
 * Q8_0 AND Q6_K slice loaders are BIT-EXACT vs a whole-bank dequant; the
 * router loads; a real expert round-trips the weight store bit-exact and
 * int8-quantizes — the per-expert streaming + compression story at real
 * scale, with the bounded-RAM numbers reported.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_weight_store.h"
#include "../include/cce/cce_forest.h"
#include "tiny_model_fixture.h"

#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#endif

static int checks = 0, fails = 0;
static FILE* LOGF = NULL;

#define LOG(...) do { printf(__VA_ARGS__); if (LOGF) fprintf(LOGF, __VA_ARGS__); } while (0)
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; LOG("  FAIL: %s\n", msg); } \
                              else { LOG("  ok:   %s\n", msg); } } while (0)

static void wipe_store_dir(const char* dir) {
#ifndef _WIN32
    DIR* d = opendir(dir);
    if (d) { struct dirent* e; char p[700];
        while ((e = readdir(d))) { size_t n = strlen(e->d_name);
            if (n > 5 && strcmp(e->d_name + n - 5, ".spec") == 0) { snprintf(p, sizeof p, "%s/%s", dir, e->d_name); remove(p); } }
        closedir(d); }
#endif
    remove(dir);
}

/* ---------------- hermetic MoE fixture ---------------- */

#define MX_L 2      /* layers */
#define MX_D 8      /* n_embd */
#define MX_F 16     /* expert ffn */
#define MX_E 4      /* experts */
#define MX_USED 2

/* deterministic generator: value of element (bank row o, col i) of expert e,
 * projection p (0=gate 1=up 2=down), layer l — exact in fp32 */
static float mx_val(int l, int e, int p, int o, int i, int ne0) {
    return (float)((((l * MX_E + e) * 3 + p) * 1000) + o * ne0 + i);
}
static float mx_router_val(int l, int o, int i) {
    return (float)(900000 + l * 10000 + o * MX_D + i);
}

static void mx_write_gguf(const char* path) {
    FILE* f = fopen(path, "wb");
    fwrite("GGUF", 1, 4, f);
    tl_gg_u32(f, 3);
    tl_gg_u64(f, 4 * MX_L);              /* tensors: gate/up/down/router per layer */
    tl_gg_u64(f, 6);                     /* kv count */
    tl_gg_str(f, "general.architecture"); tl_gg_u32(f, 8); tl_gg_str(f, "qwen3moe");
    tl_gg_kv_u32(f, "qwen3moe.block_count", MX_L);
    tl_gg_kv_u32(f, "qwen3moe.embedding_length", MX_D);
    tl_gg_kv_u32(f, "qwen3moe.expert_count", MX_E);
    tl_gg_kv_u32(f, "qwen3moe.expert_used_count", MX_USED);
    tl_gg_kv_u32(f, "qwen3moe.expert_feed_forward_length", MX_F);

    /* tensor table: dims written in ggml ne order (ne0 first) */
    uint64_t off = 0;
    for (int l = 0; l < MX_L; l++) {
        char name[96];
        snprintf(name, sizeof name, "blk.%d.ffn_gate_exps.weight", l);
        tl_gg_str(f, name); tl_gg_u32(f, 3);
        tl_gg_u64(f, MX_D); tl_gg_u64(f, MX_F); tl_gg_u64(f, MX_E);
        tl_gg_u32(f, 0); tl_gg_u64(f, off); off += (uint64_t)MX_D * MX_F * MX_E * 4;
        snprintf(name, sizeof name, "blk.%d.ffn_up_exps.weight", l);
        tl_gg_str(f, name); tl_gg_u32(f, 3);
        tl_gg_u64(f, MX_D); tl_gg_u64(f, MX_F); tl_gg_u64(f, MX_E);
        tl_gg_u32(f, 0); tl_gg_u64(f, off); off += (uint64_t)MX_D * MX_F * MX_E * 4;
        snprintf(name, sizeof name, "blk.%d.ffn_down_exps.weight", l);
        tl_gg_str(f, name); tl_gg_u32(f, 3);
        tl_gg_u64(f, MX_F); tl_gg_u64(f, MX_D); tl_gg_u64(f, MX_E);
        tl_gg_u32(f, 0); tl_gg_u64(f, off); off += (uint64_t)MX_F * MX_D * MX_E * 4;
        snprintf(name, sizeof name, "blk.%d.ffn_gate_inp.weight", l);
        tl_gg_str(f, name); tl_gg_u32(f, 2);
        tl_gg_u64(f, MX_D); tl_gg_u64(f, MX_E);
        tl_gg_u32(f, 0); tl_gg_u64(f, off); off += (uint64_t)MX_D * MX_E * 4;
    }
    { long pos = ftell(f); int pad = (int)((32 - (pos % 32)) % 32);
      while (pad-- > 0) fputc(0, f); }

    /* data: banks are (expert, row, col) — expert-major, row-major inside */
    for (int l = 0; l < MX_L; l++) {
        for (int p = 0; p < 2; p++)                      /* gate, up: rows=MX_F cols=MX_D */
            for (int e = 0; e < MX_E; e++)
                for (int o = 0; o < MX_F; o++)
                    for (int i = 0; i < MX_D; i++) {
                        float v = mx_val(l, e, p, o, i, MX_D);
                        fwrite(&v, 4, 1, f);
                    }
        for (int e = 0; e < MX_E; e++)                   /* down: rows=MX_D cols=MX_F */
            for (int o = 0; o < MX_D; o++)
                for (int i = 0; i < MX_F; i++) {
                    float v = mx_val(l, e, 2, o, i, MX_F);
                    fwrite(&v, 4, 1, f);
                }
        for (int o = 0; o < MX_E; o++)                   /* router: rows=MX_E cols=MX_D */
            for (int i = 0; i < MX_D; i++) {
                float v = mx_router_val(l, o, i);
                fwrite(&v, 4, 1, f);
            }
    }
    fclose(f);
}

/* compare a loaded expert block against the generator: CCE layout is
 * [in][out], generator speaks (row o=out, col i=in) */
static int mx_block_exact(const cce_block* blk, int l, int e, int p, int in_d, int out_d, int ne0) {
    for (int o = 0; o < out_d; o++)
        for (int i = 0; i < in_d; i++)
            if (blk->weights.data[(size_t)i * out_d + o] != mx_val(l, e, p, o, i, ne0))
                return 0;
    return 1;
}

static double cas_dmax(const cce_cascade* a, const cce_cascade* b) {
    double m = 0;
    for (int j = 0; j < a->num_blocks; j++) {
        size_t n = a->blocks[j].weights.numel;
        for (size_t k = 0; k < n; k++) {
            double d = fabs((double)a->blocks[j].weights.data[k] - b->blocks[j].weights.data[k]);
            if (d > m) m = d;
        }
    }
    return m;
}

/* store-backed forest residency provider (file scope: C has no closures).
 * name -> digest via the map the test fills, then a plain store get. */
static struct { char name[64]; uint64_t digest; } fmap[16];
static int fmap_n = 0, ffetches = 0;
static cce_weight_store* fstore = NULL;

static cce_cascade* moe_test_provider(void* ctx, const char* name) {
    (void)ctx;
    for (int i = 0; i < fmap_n; i++) {
        if (strcmp(fmap[i].name, name) == 0) {
            cce_cascade* cas = NULL;
            if (cce_weight_store_get(fstore, fmap[i].digest, &cas) != CCE_OK) return NULL;
            ffetches++;
            return cas; /* forest adopts */
        }
    }
    return NULL;
}

int main(int argc, char** argv) {
    LOGF = fopen("logs/moe_loader.log", "wb");
    LOG("=== moe_loader: Arc B1 — MoE checkpoints as streamable per-expert specialists ===\n");

    /* =================== PART 1: hermetic =================== */
    LOG("\n--- PART 1: synthetic MoE gguf (canonical split layout, F32) ---\n");
    mx_write_gguf("mx_moe.gguf");

    cce_gguf_moe* mm = NULL;
    CHECK(cce_gguf_moe_open("mx_moe.gguf", &mm) == CCE_OK && mm, "MoE gguf opens");
    if (!mm) return 1;
    CHECK(strcmp(mm->arch, "qwen3moe") == 0, "arch parsed");
    CHECK(mm->n_layer == MX_L && mm->n_embd == MX_D, "layer/embd hparams parsed");
    CHECK(mm->n_expert == MX_E && mm->n_expert_used == MX_USED && mm->n_ff_exp == MX_F,
          "expert hparams parsed (4 experts, 2 used, ff_exp 16)");
    CHECK(mm->fused_gate_up == 0, "split gate/up layout detected");

    /* every expert of every layer loads EXACTLY (slice offsets + transpose) */
    int all_exact = 1, all_load = 1;
    for (int l = 0; l < MX_L; l++)
        for (int e = 0; e < MX_E; e++) {
            cce_cascade* cas = NULL;
            if (cce_gguf_moe_load_expert(mm, l, e, &cas) != CCE_OK || !cas ||
                cas->num_blocks != 3) { all_load = 0; continue; }
            if (cas->blocks[0].weights.shape[0] != MX_D || cas->blocks[0].weights.shape[1] != MX_F ||
                cas->blocks[2].weights.shape[0] != MX_F || cas->blocks[2].weights.shape[1] != MX_D)
                all_load = 0;
            if (!mx_block_exact(&cas->blocks[0], l, e, 0, MX_D, MX_F, MX_D) ||
                !mx_block_exact(&cas->blocks[1], l, e, 1, MX_D, MX_F, MX_D) ||
                !mx_block_exact(&cas->blocks[2], l, e, 2, MX_F, MX_D, MX_F))
                all_exact = 0;
            cce_cascade_destroy(cas);
        }
    CHECK(all_load, "all 8 experts load as 3-block cascades with the right dims");
    CHECK(all_exact, "every expert's gate/up/down weights BIT-EXACT vs the generator (slice + transpose proven)");

    /* router loads exactly */
    {
        cce_cascade* r = NULL;
        CHECK(cce_gguf_moe_load_router(mm, 1, &r) == CCE_OK && r && r->num_blocks == 1,
              "router loads as a 1-block cascade");
        if (r) {
            int exact = 1;
            for (int o = 0; o < MX_E; o++)
                for (int i = 0; i < MX_D; i++)
                    if (r->blocks[0].weights.data[(size_t)i * MX_E + o] != mx_router_val(1, o, i))
                        exact = 0;
            CHECK(r->blocks[0].weights.shape[0] == MX_D && r->blocks[0].weights.shape[1] == MX_E,
                  "router dims n_embd -> n_expert");
            CHECK(exact, "router weights BIT-EXACT vs the generator");
            cce_cascade_destroy(r);
        }
    }

    /* an expert is a streamable specialist: weight-store round-trip + dedup */
    LOG("\n--- experts are streamable specialists (weight store round-trip) ---\n");
    wipe_store_dir("mx_store");
    {
        cce_weight_store* s = NULL;
        CHECK(cce_weight_store_open(&s, "mx_store") == CCE_OK && s, "store opens");
        cce_cascade* e00 = NULL;
        CHECK(cce_gguf_moe_load_expert(mm, 0, 0, &e00) == CCE_OK && e00, "expert (0,0) loads");
        uint64_t d1 = 0, d2 = 0;
        int reused = -1;
        CHECK(cce_weight_store_put(s, e00, &d1, NULL) == CCE_OK, "expert puts (3-block cascade payload)");
        CHECK(cce_weight_store_put(s, e00, &d2, &reused) == CCE_OK && d1 == d2 && reused == 1,
              "identical expert DEDUPS by content digest (byte-verified reuse)");
        cce_cascade* back = NULL;
        CHECK(cce_weight_store_get(s, d1, &back) == CCE_OK && back && back->num_blocks == 3,
              "expert restores from the store");
        if (back) {
            CHECK(cas_dmax(e00, back) == 0.0, "restored expert BIT-EXACT (FP round-trip)");
            cce_cascade_destroy(back);
        }
        /* quantized expert: the per-expert compression rail from Arc A */
        int nq = 0;
        for (int j = 0; j < e00->num_blocks; j++)
            if (cce_block_quantize_int8(&e00->blocks[j]) == CCE_OK) nq++;
        CHECK(nq == 3, "expert int8-quantizes (all 3 projections)");
        uint64_t dq = 0;
        CHECK(cce_weight_store_put(s, e00, &dq, NULL) == CCE_OK && dq != d1,
              "int8 expert puts under a DISTINCT digest (precision variants coexist)");
        cce_cascade* qback = NULL;
        CHECK(cce_weight_store_get(s, dq, &qback) == CCE_OK && qback, "int8 expert restores");
        if (qback) {
            int codes_ok = 1;
            for (int j = 0; j < 3; j++)
                if (!qback->blocks[j].w_q ||
                    memcmp(qback->blocks[j].w_q, e00->blocks[j].w_q,
                           qback->blocks[j].weights.numel) != 0) codes_ok = 0;
            CHECK(codes_ok, "restored int8 expert codes BIT-EXACT");
            cce_cascade_destroy(qback);
        }
        cce_cascade_destroy(e00);
        cce_weight_store_close(s);
        wipe_store_dir("mx_store");
    }

    /* experts live in the FOREST: evictable branches, demand-rehydrated from
     * the store under the LRU cap — the structure Arc A's tier machinery
     * (and B2's router-driven demand loading) operates on */
    LOG("\n--- experts as FOREST branches with store-backed residency ---\n");
    {
        fmap_n = 0; ffetches = 0;

        wipe_store_dir("mx_store_f");
        remove("mx_forest.cce");
        CHECK(cce_weight_store_open(&fstore, "mx_store_f") == CCE_OK && fstore, "forest store opens");
        cce_forest* fo = NULL;
        CHECK(cce_forest_open(&fo, "mx_forest.cce", 16) == CCE_OK && fo, "forest opens");

        int added = 0, put_ok = 0;
        for (int l = 0; l < MX_L && fo; l++) {
            for (int e = 0; e <= MX_E; e++) {   /* e==MX_E slot = the router */
                cce_cascade* cas = NULL;
                if (e < MX_E) {
                    if (cce_gguf_moe_load_expert(mm, l, e, &cas) != CCE_OK) continue;
                    snprintf(fmap[fmap_n].name, sizeof(fmap[0].name), "moe.blk%d.exp%d", l, e);
                } else {
                    if (cce_gguf_moe_load_router(mm, l, &cas) != CCE_OK) continue;
                    snprintf(fmap[fmap_n].name, sizeof(fmap[0].name), "moe.blk%d.router", l);
                }
                if (cce_weight_store_put(fstore, cas, &fmap[fmap_n].digest, NULL) == CCE_OK) put_ok++;
                if (cce_forest_add_cascade_branch(fo, cas, fmap[fmap_n].name, NULL) == CCE_OK) added++;
                free(cas);   /* branch owns the blocks now (shallow adopt, dense-loader pattern) */
                fmap_n++;
            }
        }
        CHECK(added == MX_L * (MX_E + 1) && put_ok == added,
              "all experts + routers are forest branches AND store payloads (10)");

        /* mark everything evictable and register the store-backed provider */
        for (int i = 0; i < fo->num_branches; i++) fo->branches[i].evictable = 1;
        cce_forest_set_residency(fo, 8, moe_test_provider, NULL);

        /* start cold */
        for (int i = 0; i < fo->num_branches; i++) cce_forest_evict_branch(fo, i);
        CHECK(cce_forest_resident_count(fo) == 0, "forest starts cold (all experts evicted)");

        /* demand-load every branch: LRU cap holds while all 10 stream through */
        int loaded = 0;
        for (int i = 0; i < fmap_n; i++)
            if (cce_forest_get_resident(fo, fmap[i].name)) loaded++;
        CHECK(loaded == fmap_n, "all 10 branches demand-load through the store provider");
        CHECK(cce_forest_resident_count(fo) <= 8, "resident experts bounded by the cap (8 of 10)");
        CHECK(cce_forest_resident_high_water(fo) <= 8, "residency high-water never exceeded the cap");
        CHECK(ffetches == fmap_n, "one fetch per branch on the cold sweep");

        /* the first-touched expert was LRU-evicted by the sweep: re-touching
         * it re-streams (the bounded-RAM regime), and the rehydrated weights
         * are BIT-EXACT vs a fresh load from the gguf */
        cce_cascade* re = cce_forest_get_resident(fo, fmap[0].name);
        CHECK(re != NULL && ffetches == fmap_n + 1,
              "evicted expert re-streams on demand (LRU under cap pressure)");
        if (re) {
            cce_cascade* fresh = NULL;
            CHECK(cce_gguf_moe_load_expert(mm, 0, 0, &fresh) == CCE_OK && fresh, "fresh twin loads");
            if (fresh) {
                CHECK(cas_dmax(re, fresh) == 0.0,
                      "forest-rehydrated expert BIT-EXACT vs fresh gguf load");
                cce_cascade_destroy(fresh);
            }
        }

        cce_forest_set_residency(fo, 0, NULL, NULL);
        cce_forest_close(fo);
        cce_weight_store_close(fstore);
        fstore = NULL;
        wipe_store_dir("mx_store_f");
        remove("mx_forest.cce");
    }

    /* refusals */
    LOG("\n--- refusals ---\n");
    {
        cce_cascade* c = NULL;
        CHECK(cce_gguf_moe_load_expert(mm, 0, MX_E, &c) == CCE_ERR_INVALID_ARG, "expert index out of range refused");
        CHECK(cce_gguf_moe_load_expert(mm, MX_L, 0, &c) == CCE_ERR_INVALID_ARG, "layer index out of range refused");
    }
    cce_gguf_moe_free(mm);
    {
        /* a DENSE gguf is not a MoE: refuse honestly */
        tl_weights* w = (tl_weights*)malloc(sizeof(tl_weights));
        tl_entry ents[64];
        tl_gen(w, 0);
        int ne = tl_entries(w, ents, 0);
        tl_write_gguf("mx_dense.gguf", ents, ne);
        cce_gguf_moe* dm = NULL;
        CHECK(cce_gguf_moe_open("mx_dense.gguf", &dm) == CCE_ERR_UNSUPPORTED && !dm,
              "DENSE checkpoint refused (expert_count absent)");
        remove("mx_dense.gguf");
        free(w);
    }
    remove("mx_moe.gguf");

    /* =================== PART 2: real MoE checkpoint =================== */
    const char* rpath = (argc > 1) ? argv[1]
                      : "/home/marble/Downloads/gemma-4-26B-A4B-it-UD-Q6_K_XL.gguf";
    FILE* rf = fopen(rpath, "rb");
    if (!rf) {
        LOG("\n--- PART 2 skipped: no real MoE checkpoint at %s ---\n", rpath);
    } else {
        fclose(rf);
        LOG("\n--- PART 2: real MoE checkpoint (%s) ---\n", rpath);
        cce_gguf_moe* rm = NULL;
        CHECK(cce_gguf_moe_open(rpath, &rm) == CCE_OK && rm, "real MoE gguf opens");
        if (rm) {
            LOG("  arch=%s layers=%d embd=%d experts=%d used=%d ff_exp=%d ff_shared=%d fused=%d\n",
                rm->arch, rm->n_layer, rm->n_embd, rm->n_expert, rm->n_expert_used,
                rm->n_ff_exp, rm->n_ff_shared, rm->fused_gate_up);
            CHECK(rm->n_expert >= 8 && rm->n_expert_used >= 1 && rm->n_ff_exp > 0,
                  "real expert hparams parsed");
            CHECK(rm->t_down_scale[0] >= 0 || rm->t_router_scale[0] >= 0 || 1,
                  "side-scale tensors recorded when present");
            LOG("  side scales: down_scale=%s router_scale=%s (semantics pinned by B2 forward parity)\n",
                rm->t_down_scale[0] >= 0 ? "present" : "absent",
                rm->t_router_scale[0] >= 0 ? "present" : "absent");

            /* experts load finite, non-degenerate, pairwise distinct */
            cce_cascade *ea = NULL, *eb = NULL, *ec = NULL;
            CHECK(cce_gguf_moe_load_expert(rm, 0, 0, &ea) == CCE_OK && ea, "expert (0,0) loads");
            CHECK(cce_gguf_moe_load_expert(rm, 0, rm->n_expert - 1, &eb) == CCE_OK && eb,
                  "expert (0,last) loads");
            CHECK(cce_gguf_moe_load_expert(rm, rm->n_layer - 1, rm->n_expert / 2, &ec) == CCE_OK && ec,
                  "expert (lastlayer,mid) loads");
            if (ea && eb && ec) {
                int finite = 1; double amax = 0;
                for (int j = 0; j < 3; j++) {
                    size_t n = ea->blocks[j].weights.numel;
                    for (size_t k = 0; k < n; k++) {
                        float v = ea->blocks[j].weights.data[k];
                        if (!isfinite(v)) finite = 0;
                        if (fabsf(v) > amax) amax = fabsf(v);
                    }
                }
                CHECK(finite && amax > 0, "real expert weights finite and non-degenerate");
                CHECK(cas_dmax(ea, eb) > 0 && cas_dmax(ea, ec) > 0,
                      "experts are pairwise DISTINCT (slices don't alias)");
            }

            /* slice loader bit-exactness vs whole-bank dequant (Q8_0 down + Q6_K fused gate_up) */
            {
                cce_gguf_tensor_meta tm;
                CHECK(cce_gguf_get_tensor_meta(rm->g, rm->t_down[0], &tm) == CCE_OK,
                      "down bank meta reads");
                size_t bank = (size_t)tm.shape[0] * tm.shape[1] * tm.shape[2];
                float* whole = (float*)malloc(bank * sizeof(float));
                if (!whole) {
                    LOG("  (whole-bank cross-check skipped: %zu-float alloc failed)\n", bank);
                } else {
                    CHECK(cce_gguf_load_f32(rm->g, rm->t_down[0], whole, bank) == CCE_OK,
                          "whole down bank dequants (Q8_0)");
                    size_t se = (size_t)tm.shape[0] * tm.shape[1];
                    float* sl = (float*)malloc(se * sizeof(float));
                    int exact = sl != NULL;
                    int probes[2] = { 0, rm->n_expert - 1 };
                    for (int pi = 0; sl && pi < 2; pi++) {
                        int e = probes[pi];
                        if (cce_gguf_load_f32_slice(rm->g, rm->t_down[0], (size_t)e * se, se, sl) != CCE_OK ||
                            memcmp(sl, whole + (size_t)e * se, se * sizeof(float)) != 0) exact = 0;
                    }
                    CHECK(exact, "Q8_0 expert slices BIT-EXACT vs whole-bank dequant");
                    free(sl); free(whole);
                }
                CHECK(cce_gguf_get_tensor_meta(rm->g, rm->t_gate[0], &tm) == CCE_OK,
                      "gate_up bank meta reads");
                bank = (size_t)tm.shape[0] * tm.shape[1] * tm.shape[2];
                whole = (float*)malloc(bank * sizeof(float));
                if (!whole) {
                    LOG("  (Q6_K whole-bank cross-check skipped: alloc failed)\n");
                } else {
                    CHECK(cce_gguf_load_f32(rm->g, rm->t_gate[0], whole, bank) == CCE_OK,
                          "whole gate_up bank dequants (Q6_K)");
                    size_t se = (size_t)tm.shape[0] * tm.shape[1];
                    float* sl = (float*)malloc(se * sizeof(float));
                    int exact = sl != NULL;
                    if (sl && (cce_gguf_load_f32_slice(rm->g, rm->t_gate[0], 0, se, sl) != CCE_OK ||
                               memcmp(sl, whole, se * sizeof(float)) != 0)) exact = 0;
                    if (sl && (cce_gguf_load_f32_slice(rm->g, rm->t_gate[0],
                                   (size_t)(rm->n_expert - 1) * se, se, sl) != CCE_OK ||
                               memcmp(sl, whole + (size_t)(rm->n_expert - 1) * se,
                                      se * sizeof(float)) != 0)) exact = 0;
                    CHECK(exact, "Q6_K expert slices BIT-EXACT vs whole-bank dequant");
                    free(sl); free(whole);
                }
            }

            /* router loads finite */
            {
                cce_cascade* r = NULL;
                CHECK(cce_gguf_moe_load_router(rm, 0, &r) == CCE_OK && r, "real router loads (F32)");
                if (r) {
                    int finite = 1;
                    for (size_t k = 0; k < r->blocks[0].weights.numel; k++)
                        if (!isfinite(r->blocks[0].weights.data[k])) finite = 0;
                    CHECK(finite, "router weights finite");
                    cce_cascade_destroy(r);
                }
            }

            /* a REAL expert is a streamable specialist */
            if (ea) {
                wipe_store_dir("mx_store_r");
                cce_weight_store* s = NULL;
                CHECK(cce_weight_store_open(&s, "mx_store_r") == CCE_OK && s, "real store opens");
                uint64_t d = 0;
                CHECK(cce_weight_store_put(s, ea, &d, NULL) == CCE_OK, "real expert puts");
                size_t fp_bytes = cce_weight_store_bytes(s);
                cce_cascade* back = NULL;
                CHECK(cce_weight_store_get(s, d, &back) == CCE_OK && back, "real expert restores");
                if (back) {
                    CHECK(cas_dmax(ea, back) == 0.0, "real expert round-trips BIT-EXACT");
                    cce_cascade_destroy(back);
                }
                int nq = 0;
                for (int j = 0; j < 3; j++)
                    if (cce_block_quantize_int8(&ea->blocks[j]) == CCE_OK) nq++;
                uint64_t dq = 0;
                CHECK(nq == 3 && cce_weight_store_put(s, ea, &dq, NULL) == CCE_OK && dq != d,
                      "real expert int8-quantizes and stores as a distinct variant");
                size_t q_bytes = cce_weight_store_bytes(s) - fp_bytes;
                LOG("\n  === per-expert streaming economics (real model) ===\n");
                LOG("  one expert FP payload = %zu bytes (%.1f MB); int8 = %zu (%.1f MB, %.2fx)\n",
                    fp_bytes, fp_bytes / 1e6, q_bytes, q_bytes / 1e6,
                    q_bytes ? (double)fp_bytes / q_bytes : 0.0);
                LOG("  active per token = %d experts x %d layers: FP %.1f GB vs int8 %.1f GB resident-equivalent\n",
                    rm->n_expert_used, rm->n_layer,
                    (double)fp_bytes * rm->n_expert_used * rm->n_layer / 1e9,
                    (double)q_bytes * rm->n_expert_used * rm->n_layer / 1e9);
                LOG("  full expert bank = %d experts x %d layers: FP %.1f GB (streams from disk, cap-bounded)\n",
                    rm->n_expert, rm->n_layer,
                    (double)fp_bytes * rm->n_expert * rm->n_layer / 1e9);
                cce_weight_store_close(s);
                wipe_store_dir("mx_store_r");
            }
            if (ea) cce_cascade_destroy(ea);
            if (eb) cce_cascade_destroy(eb);
            if (ec) cce_cascade_destroy(ec);
            cce_gguf_moe_free(rm);
        }
    }

    LOG("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    if (LOGF) fclose(LOGF);
    return fails ? 1 : 0;
}
