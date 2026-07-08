/* hybrid_catalog: two genuinely different architectures (tiny-llama
 * transformer + tiny-mamba SSM) served from ONE content-addressed store
 * behind a query-level catalog (model-merge scope M1).
 *
 * Claims gated:
 *   1. both families ingest into the SAME store; the new ssm restore path
 *      round-trips BIT-identical logits (closes the header's stated limit)
 *   2. a task tag selects the right family + manifest from a catalog file,
 *      and the selected model's store-served forward is bit-identical to
 *      its standalone self (model-granularity routing = the honest claim)
 *   3. HONESTY gate: cross-architecture dedup is measured and REPORTED —
 *      expected 0 payloads shared. The runtime win is real; the
 *      "different models share weights" story is not. Publish the truth.
 *
 * Hermetic: fixtures only, no model files, no network.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_weight_store.h"
#include "tiny_model_fixture.h"

#ifdef _WIN32
#include <io.h>
#endif

static int checks = 0, fails = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } } while (0)

static void wipe_store_dir(const char* dir) {
#ifdef _WIN32
    struct _finddata_t fd;
    char pat[600], path[700];
    snprintf(pat, sizeof(pat), "%s/*.spec", dir);
    intptr_t h = _findfirst(pat, &fd);
    if (h != -1) {
        do {
            snprintf(path, sizeof(path), "%s/%s", dir, fd.name);
            remove(path);
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
#else
    (void)dir;
#endif
    remove(dir);
}

static const int LT[4] = { 3, 17, 9, 22 };   /* llama tokens (< TL_V) */
static const int MT[4] = { 3, 7, 1, 9 };     /* mamba tokens (< TM_V) */

/* ---- the catalog: task tag -> family + manifest (flat text) ---- */

typedef struct { char task[32]; char family[16]; char manifest[128]; } cat_row;

static int catalog_select(const char* path, const char* task, cat_row* out) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    cat_row r;
    int found = 0;
    while (fscanf(f, "task %31s family %15s manifest %127s\n",
                  r.task, r.family, r.manifest) == 3) {
        if (strcmp(r.task, task) == 0) { *out = r; found = 1; break; }
    }
    fclose(f);
    return found;
}

int main(void) {
    printf("=== hybrid_catalog: transformer + SSM from ONE store (M1) ===\n");

    /* ---- fixtures ---- */
    tl_weights* wl = (tl_weights*)malloc(sizeof(tl_weights));
    tm_weights* wm = (tm_weights*)malloc(sizeof(tm_weights));
    tl_gen(wl, 0);
    tm_gen(wm);
    tl_write_st_dir("hc_llama", wl);
    tm_write_st("hc_mamba.safetensors", wm);

    /* ---- standalone references ---- */
    float llama_ref[TL_V], mamba_ref[TM_V];
    {
        cce_anymodel* m = NULL;
        CHECK(cce_anymodel_open(&m, "hc_llama/model.safetensors") == CCE_OK && m && m->transformer,
              "llama opens via anymodel");
        CHECK(m && cce_gguf_qwen2_forward(m->transformer, LT, 4, llama_ref, TL_V) == CCE_OK,
              "llama standalone forward");
        if (m) cce_anymodel_free(m);

        cce_ssm_model* sm = NULL;
        CHECK(cce_ssm_load(&sm, "hc_mamba.safetensors") == CCE_OK && sm, "mamba opens");
        CHECK(sm && cce_ssm_forward(sm, MT, 4, mamba_ref, TM_V) == CCE_OK,
              "mamba standalone forward");
        if (sm) cce_ssm_free(sm);
    }

    /* ---- one store, both families ---- */
    wipe_store_dir("hc_store");
    cce_weight_store* s = NULL;
    CHECK(cce_weight_store_open(&s, "hc_store") == CCE_OK && s, "hybrid store opens");
    if (!s) return 1;

    int lt = 0, ln = 0, lr = 0, mt = 0, mn = 0, mr = 0;
    size_t llama_bytes = 0;
    {
        cce_anymodel* m = NULL;
        cce_anymodel_open(&m, "hc_llama/model.safetensors");
        CHECK(m && cce_weight_store_ingest_model(s, m, "tiny-llama", "hc_llama.manifest",
                                                 &lt, &ln, &lr) == CCE_OK, "llama ingests");
        if (m) cce_anymodel_free(m);
        llama_bytes = cce_weight_store_bytes(s);

        cce_anymodel* mm = NULL;
        CHECK(cce_anymodel_open(&mm, "hc_mamba.safetensors") == CCE_OK && mm && mm->ssm,
              "mamba opens via anymodel (runner registry)");
        CHECK(mm && cce_weight_store_ingest_model(s, mm, "tiny-mamba", "hc_mamba.manifest",
                                                  &mt, &mn, &mr) == CCE_OK, "mamba ingests into the SAME store");
        if (mm) cce_anymodel_free(mm);
    }

    /* ---- the HONESTY gate: measure cross-architecture dedup ---- */
    {
        size_t total = cce_weight_store_bytes(s);
        printf("  cross-arch dedup: mamba reused %d of %d payloads against the llama store\n", mr, mt);
        printf("  storage: llama %zu B + mamba %zu B = %zu B (no cross-arch sharing)\n",
               llama_bytes, total - llama_bytes, total);
        CHECK(mr == 0, "HONESTY: different architectures share ZERO payloads — "
                       "the win is one store + one runtime, not magic weight sharing");
        CHECK(total > llama_bytes, "mamba payloads actually stored");
    }

    /* ---- catalog + query-level selection ---- */
    {
        FILE* f = fopen("hc_catalog.txt", "wb");
        fprintf(f, "task text-gen family transformer manifest hc_llama.manifest\n");
        fprintf(f, "task seq-scan family ssm manifest hc_mamba.manifest\n");
        fclose(f);

        cat_row r;
        CHECK(catalog_select("hc_catalog.txt", "text-gen", &r) &&
              strcmp(r.family, "transformer") == 0, "catalog selects transformer for text-gen");
        if (strcmp(r.family, "transformer") == 0) {
            cce_gguf_qwen2* m = NULL;
            CHECK(cce_weight_store_restore_transformer(s, r.manifest, "hc_l.cce", &m) == CCE_OK && m,
                  "selected transformer restores from the hybrid store");
            if (m) {
                float lg[TL_V];
                CHECK(cce_gguf_qwen2_forward(m, LT, 4, lg, TL_V) == CCE_OK, "restored llama forward");
                float dmax = 0;
                for (int v = 0; v < TL_V; v++) { float d = fabsf(lg[v] - llama_ref[v]); if (d > dmax) dmax = d; }
                CHECK(dmax == 0.0f, "text-gen query: store-served llama BIT-identical to standalone");
                cce_gguf_qwen2_free(m);
            }
            remove("hc_l.cce");
        }

        CHECK(catalog_select("hc_catalog.txt", "seq-scan", &r) &&
              strcmp(r.family, "ssm") == 0, "catalog selects ssm for seq-scan");
        if (strcmp(r.family, "ssm") == 0) {
            cce_ssm_model* m = NULL;
            CHECK(cce_weight_store_restore_ssm(s, r.manifest, "hc_m.cce", &m) == CCE_OK && m,
                  "selected SSM restores from the hybrid store (NEW: closes the stated limit)");
            if (m) {
                CHECK(m->n_layer == TM_L && m->d_model == TM_D && m->d_inner == TM_E &&
                      m->d_state == TM_N && m->d_conv == TM_K && m->dt_rank == TM_R &&
                      m->vocab_size == TM_V, "ssm hparams restored");
                float lg[TM_V];
                CHECK(cce_ssm_forward(m, MT, 4, lg, TM_V) == CCE_OK, "restored mamba forward");
                float dmax = 0;
                for (int v = 0; v < TM_V; v++) { float d = fabsf(lg[v] - mamba_ref[v]); if (d > dmax) dmax = d; }
                printf("  ssm restore |diff|max = %g\n", (double)dmax);
                CHECK(dmax == 0.0f, "seq-scan query: store-served mamba BIT-identical to standalone");
                cce_ssm_free(m);
            }
            remove("hc_m.cce");
        }

        CHECK(!catalog_select("hc_catalog.txt", "vision", &r),
              "unknown task refuses honestly (no silent fallback)");
        remove("hc_catalog.txt");
    }

    /* ---- family-mismatch refusals: the wrong restorer refuses the manifest ---- */
    {
        cce_gguf_qwen2* mt2 = NULL;
        CHECK(cce_weight_store_restore_transformer(s, "hc_mamba.manifest", "hc_x.cce", &mt2) != CCE_OK &&
              mt2 == NULL, "transformer restore REFUSES the ssm manifest");
        cce_ssm_model* ms2 = NULL;
        CHECK(cce_weight_store_restore_ssm(s, "hc_llama.manifest", "hc_y.cce", &ms2) != CCE_OK &&
              ms2 == NULL, "ssm restore REFUSES the transformer manifest");
        remove("hc_x.cce"); remove("hc_y.cce");
    }

    /* ---- cleanup ---- */
    cce_weight_store_close(s);
    wipe_store_dir("hc_store");
    tl_cleanup_st_dir("hc_llama");
    remove("hc_mamba.safetensors");
    remove("hc_llama.manifest");
    remove("hc_mamba.manifest");
    free(wl); free(wm);

    printf("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    return fails ? 1 : 0;
}
