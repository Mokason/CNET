/* CNET-isolated importer: GGUF → .cnetpack (+ optional forest).
 * Usage: cnet_ds_import <model.gguf> <out.cnetpack> [out.cce]
 * Does not require DeepSeek/llama.cpp projects — uses CNET GGUF reader only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cce/cce_ds_runtime.h"
#include "../include/cce/cce_gguf.h"

int main(int argc, char** argv) {
    const char* gguf;
    const char* pack;
    const char* forest = NULL;
    cce_result rc;
    cce_gguf* g = NULL;
    cce_ds_hparams hp;
    cce_ds_map map;
    cce_ds_bind_report br;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <model.gguf> <out.cnetpack> [out.cce]\n",
                argv[0]);
        return 2;
    }
    gguf = argv[1];
    pack = argv[2];
    if (argc > 3) forest = argv[3];

    rc = cce_ds_import_gguf(gguf, pack, forest, 0 /* no cold bind */);
    if (rc != CCE_OK) {
        fprintf(stderr, "import failed rc=%d\n", (int)rc);
        return 1;
    }
    printf("CNET_DS_IMPORT_OK pack=%s forest=%s\n", pack,
           forest ? forest : "(none)");

    /* Report map stats from re-load meta */
    if (cce_gguf_load(gguf, &g) == CCE_OK && g) {
        cce_ds_hparams_from_gguf(g, &hp);
        if (cce_ds_map_build(&map, &hp) == CCE_OK) {
            int hot, warm, cold;
            cce_ds_map_residency_counts(&map, &hot, &warm, &cold);
            printf("  arch=%s layers=%d d_model=%d experts=%d leaves=%d "
                   "hot=%d warm=%d cold=%d\n",
                   hp.arch, hp.n_layer, hp.d_model, hp.n_expert, map.n_leaves,
                   hot, warm, cold);
            cce_ds_map_bind_check(&map, NULL, NULL, &br);
            cce_ds_map_free(&map);
        }
        cce_gguf_free(g);
    }
    return 0;
}
