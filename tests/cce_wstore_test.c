/* cce_weight_store: content-addressed specialists, models as manifests.
 *
 * The thesis gate ("reuse makes the overall model smaller"), measured:
 *  - first ingest: everything new except the tied output tensor (dedups
 *    against tok_emb automatically — same bytes, same digest)
 *  - same model again: 0 new, 100% reused, store unchanged
 *  - same weights via the OTHER container (gguf): 0 new — identity is
 *    content, not file format
 *  - one-matrix fine-tune: store grows by exactly ONE payload
 *  - restore from manifest -> forward is bit-identical to the original
 *  - reuse verdicts are byte-verified; a forged payload under a digest is
 *    refused, not trusted
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_weight_store.h"
#include "../include/cce/cce_specgraph.h"
#include "tiny_model_fixture.h"

#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
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
    DIR* d = opendir(dir);
    if (d) {
        struct dirent* ent;
        char path[700];
        while ((ent = readdir(d)) != NULL) {
            size_t n = strlen(ent->d_name);
            if (n > 5 && strcmp(ent->d_name + n - 5, ".spec") == 0) {
                snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
                remove(path);
            }
        }
        closedir(d);
    }
#endif
    remove(dir);
}

int main(void) {
    printf("=== cce_weight_store: store once, reference everywhere ===\n");

    tl_weights* w = (tl_weights*)malloc(sizeof(tl_weights));
    tl_gen(w, 0);
    tl_write_st_dir("ws_a", w);
    tl_entry ents[64];
    int n_gg = tl_entries(w, ents, 0);
    tl_write_gguf("ws_a.gguf", ents, n_gg);

    wipe_store_dir("ws_store");
    cce_weight_store* s = NULL;
    CHECK(cce_weight_store_open(&s, "ws_store") == CCE_OK && s, "store opens");
    if (!s) return 1;
    CHECK(cce_weight_store_count(s) == 0, "fresh store is empty");

    const int EXPECT_SPECS = 7 * TL_L + 1;             /* 15 */
    const int EXPECT_TENSORS = 3 + 2 * TL_L;           /* tok_emb, output_norm, output + 2 norms/layer */
    const int EXPECT_TOTAL = EXPECT_SPECS + EXPECT_TENSORS;

    /* 1. first ingest: everything new except the tied output tensor */
    int nt = 0, nn = 0, nr = 0;
    {
        cce_anymodel* ma = NULL;
        CHECK(cce_anymodel_open(&ma, "ws_a/model.safetensors") == CCE_OK && ma, "model A opens");
        if (!ma) return 1;
        CHECK(cce_weight_store_ingest_model(s, ma, "modelA", "ws_a.manifest", &nt, &nn, &nr) == CCE_OK,
              "ingest A ok");
        CHECK(nt == EXPECT_TOTAL, "ingest touches every specialist + parameter tensor");
        CHECK(nr == 1 && nn == EXPECT_TOTAL - 1,
              "tied output tensor dedups against tok_emb on FIRST ingest (same bytes)");
        CHECK(cce_weight_store_count(s) == EXPECT_TOTAL - 1, "store holds unique payloads only");
        cce_anymodel_free(ma);
    }
    size_t bytes_a = cce_weight_store_bytes(s);
    CHECK(bytes_a > 0, "store bytes accounted");

    /* 2. same model again: 100% reuse */
    {
        cce_anymodel* ma = NULL;
        CHECK(cce_anymodel_open(&ma, "ws_a/model.safetensors") == CCE_OK && ma, "model A reopens");
        CHECK(cce_weight_store_ingest_model(s, ma, "modelA", "ws_a2.manifest", &nt, &nn, &nr) == CCE_OK,
              "re-ingest ok");
        CHECK(nn == 0 && nr == EXPECT_TOTAL, "same model twice => 0 new, 100% reused");
        CHECK(cce_weight_store_bytes(s) == bytes_a, "store bytes unchanged");
        cce_anymodel_free(ma);
        remove("ws_a2.manifest");
    }

    /* 3. cross-container: same weights via gguf => 0 new */
    {
        cce_anymodel* mg = NULL;
        CHECK(cce_anymodel_open(&mg, "ws_a.gguf") == CCE_OK && mg, "model A opens via gguf");
        CHECK(cce_weight_store_ingest_model(s, mg, "modelA-gguf", "ws_ag.manifest", &nt, &nn, &nr) == CCE_OK,
              "gguf ingest ok");
        CHECK(nn == 0 && nr == EXPECT_TOTAL,
              "identity is content, not container: gguf ingest reuses everything");
        cce_anymodel_free(mg);
        remove("ws_ag.manifest");
    }

    /* 4. one-matrix fine-tune: store grows by exactly one payload */
    {
        tl_weights* wv = (tl_weights*)malloc(sizeof(tl_weights));
        tl_gen(wv, 1);
        tl_write_st_dir("ws_b", wv);
        cce_anymodel* mb = NULL;
        CHECK(cce_anymodel_open(&mb, "ws_b/model.safetensors") == CCE_OK && mb, "fine-tune opens");
        int count_before = cce_weight_store_count(s);
        size_t bytes_before = cce_weight_store_bytes(s);
        CHECK(cce_weight_store_ingest_model(s, mb, "modelB", "ws_b.manifest", &nt, &nn, &nr) == CCE_OK,
              "fine-tune ingests");
        CHECK(nn == 1 && nr == EXPECT_TOTAL - 1,
              "one-matrix fine-tune costs the store exactly ONE new payload");
        CHECK(cce_weight_store_count(s) == count_before + 1, "count grew by one");
        size_t delta = cce_weight_store_bytes(s) - bytes_before;
        /* header + block header + weights + always-allocated bias */
        size_t expect_payload = 12 + 4 + 16 + (size_t)TL_FFN * TL_D * 4 + (size_t)TL_FFN * 4;
        CHECK(delta == expect_payload, "bytes grew by exactly the changed matrix's payload");
        cce_anymodel_free(mb);
        tl_cleanup_st_dir("ws_b");
        remove("ws_b.manifest");
        free(wv);
    }

    /* 5. restore from manifest -> bit-identical forward */
    {
        static const int tokens[4] = { 3, 17, 9, 22 };
        float orig[TL_V], rest[TL_V];

        cce_anymodel* ma = NULL;
        CHECK(cce_anymodel_open(&ma, "ws_a/model.safetensors") == CCE_OK && ma, "original loads");
        CHECK(cce_gguf_qwen2_forward(ma->transformer, tokens, 4, orig, TL_V) == CCE_OK,
              "original forward");
        cce_anymodel_free(ma);

        cce_gguf_qwen2* mr = NULL;
        CHECK(cce_weight_store_restore_transformer(s, "ws_a.manifest", "ws_restore.cce", &mr) == CCE_OK && mr,
              "model restores from manifest + store");
        if (mr) {
            CHECK(mr->n_layer == TL_L && mr->n_embd == TL_D && mr->vocab_size == TL_V &&
                  mr->rope_freq_base == TL_ROPE, "hparams + numerics restored");
            CHECK(cce_gguf_qwen2_forward(mr, tokens, 4, rest, TL_V) == CCE_OK, "restored forward");
            float dmax = 0;
            for (int v = 0; v < TL_V; v++) { float d = fabsf(rest[v] - orig[v]); if (d > dmax) dmax = d; }
            printf("  restore |diff|max = %g\n", (double)dmax);
            CHECK(dmax == 0.0f, "restored model is bit-identical to the original");
            cce_gguf_qwen2_free(mr);
        }
        remove("ws_restore.cce");
    }

    /* 6. lookups honest on absence */
    {
        cce_cascade* cas = NULL;
        CHECK(cce_weight_store_get(s, 0xdeadbeefdeadbeefULL, &cas) == CCE_ERR_NOT_FOUND && cas == NULL,
              "missing digest refuses");
        CHECK(cce_weight_store_contains(s, 0xdeadbeefdeadbeefULL) == 0, "contains says no");
    }

    /* 7. persistence across handles */
    {
        int count_now = cce_weight_store_count(s);
        cce_weight_store_close(s);
        s = NULL;
        CHECK(cce_weight_store_open(&s, "ws_store") == CCE_OK && s, "store reopens");
        CHECK(cce_weight_store_count(s) == count_now, "reopen recounts payloads from disk");
        cce_anymodel* ma = NULL;
        CHECK(cce_anymodel_open(&ma, "ws_a/model.safetensors") == CCE_OK && ma, "model A opens again");
        CHECK(cce_weight_store_ingest_model(s, ma, "modelA", "ws_a3.manifest", &nt, &nn, &nr) == CCE_OK &&
              nn == 0, "after reopen: still 0 new (identity persisted)");
        cce_anymodel_free(ma);
        remove("ws_a3.manifest");
    }

    /* 8. reuse verdicts are byte-verified: forged payload refused */
    {
        wipe_store_dir("ws_forge");
        cce_weight_store* s2 = NULL;
        CHECK(cce_weight_store_open(&s2, "ws_forge") == CCE_OK && s2, "forge store opens");
        cce_anymodel* ma = NULL;
        cce_anymodel_open(&ma, "ws_a/model.safetensors");
        if (ma && ma->transformer && ma->transformer->forest->num_branches > 0) {
            cce_cascade* cas = ma->transformer->forest->branches[0].cascade;
            uint64_t d = cce_spec_digest(cas);
            char path[700];
            snprintf(path, sizeof(path), "ws_forge/%016llx.spec", (unsigned long long)d);
            FILE* f = fopen(path, "wb");
            fwrite("JUNKJUNK", 1, 8, f);
            fclose(f);
            uint64_t dg = 0; int r = -1;
            CHECK(cce_weight_store_put(s2, cas, &dg, &r) == CCE_ERR_UNSUPPORTED,
                  "digest hit with different bytes is REFUSED, not trusted");
            remove(path);
        }
        if (ma) cce_anymodel_free(ma);
        cce_weight_store_close(s2);
        wipe_store_dir("ws_forge");
    }

    cce_weight_store_close(s);
    wipe_store_dir("ws_store");
    tl_cleanup_st_dir("ws_a");
    remove("ws_a.gguf");
    remove("ws_a.manifest");
    free(w);

    printf("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    return fails ? 1 : 0;
}
