/* qwythos_load: load the CNET int8 PTQ data (.int8data + manifest) into a cce_forest.
 * This makes the model "runnable" in CNET format: the 284 specialists are now
 * CCE blocks (int8) in a forest that can be used with cce_forest_forward on
 * individual cascades, or integrated into the agent/specialist router.
 *
 * For full end-to-end generation, the hybrid forward (attn/ssm/ffn wiring,
 * norms, RoPE, KV cache, etc.) still needs to be implemented using these blocks
 * for the matmuls.
 *
 * Usage: ./bin/qwythos_load artifacts/qwythos-9b-hybrid-linears.cce.int8data \
 *          artifacts/qwythos-9b-hybrid-linears.cce.manifest.txt
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "../include/cce/cce_forest.h"
#include "../include/cce/cce_block.h"
#include "../include/cce/cce_cascade.h"

typedef struct {
    char name[256];
    int32_t idim;
    int32_t odim;
    int8_t hasb;
    float* scales;
    int8_t* codes;
    float* bias;
} spec_t;

static int read_int8data(const char* path, spec_t** out_specs, int* out_n) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    uint32_t magic, ver, nb;
    if (fread(&magic, 4, 1, f) != 1 || magic != 0x3843544e) { fclose(f); return -2; }
    fread(&ver, 4, 1, f);
    fread(&nb, 4, 1, f);

    spec_t* specs = calloc(nb, sizeof(spec_t));
    for (uint32_t i = 0; i < nb; i++) {
        uint32_t nlen;
        fread(&nlen, 4, 1, f);
        fread(specs[i].name, 1, nlen, f);
        specs[i].name[nlen] = 0;
        fread(&specs[i].idim, 4, 1, f);
        fread(&specs[i].odim, 4, 1, f);
        fread(&specs[i].hasb, 1, 1, f);

        size_t osz = specs[i].odim * sizeof(float);
        specs[i].scales = malloc(osz);
        fread(specs[i].scales, 1, osz, f);

        size_t csz = (size_t)specs[i].idim * specs[i].odim;
        specs[i].codes = malloc(csz);
        fread(specs[i].codes, 1, csz, f);

        if (specs[i].hasb) {
            specs[i].bias = malloc(osz);
            fread(specs[i].bias, 1, osz, f);
        }
    }
    fclose(f);
    *out_specs = specs;
    *out_n = nb;
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <file.int8data> <file.manifest.txt>\n", argv[0]);
        return 1;
    }
    const char* datapath = argv[1];
    const char* manpath = argv[2];

    spec_t* specs = NULL;
    int nspecs = 0;
    if (read_int8data(datapath, &specs, &nspecs) != 0) {
        fprintf(stderr, "failed to read int8data\n");
        return 2;
    }
    printf("Loaded %d specialists from int8data\n", nspecs);

    // Read manifest to get/verify names (optional, we use the order from data)
    // For simplicity, we use the order in the int8data file.

    cce_forest* forest = NULL;
    if (cce_forest_open(&forest, "/tmp/qwythos_temp.cce", nspecs + 10) != CCE_OK) {
        fprintf(stderr, "forest open failed\n");
        return 3;
    }

    for (int i = 0; i < nspecs; i++) {
        spec_t* s = &specs[i];
        int idx = -1;
        // Use the generic linear add to set up the cascade/block structure correctly
        if (cce_forest_add_linear_branch(forest, s->name, s->idim, 0, s->odim, 0.0f, &idx) != CCE_OK) {
            continue;
        }
        cce_cascade* cas = forest->branches[idx].cascade;
        if (!cas || cas->num_blocks == 0) continue;
        cce_block* b = &cas->blocks[0];

        // Override with our int8 PTQ data (free any random FP created by add)
        if (b->weights.data) { free(b->weights.data); b->weights.data = NULL; }
        b->w_scale = s->scales;
        b->w_q = s->codes;
        s->scales = NULL;
        s->codes = NULL;

        if (s->hasb && b->bias.data) {
            // keep or replace bias if the add created one
            // for simplicity, if we have bias, assume structure matches
            if (b->bias.data) free(b->bias.data);
            b->bias.data = s->bias;
            s->bias = NULL;
        }

        // mark as int8 style
        b->weights.numel = (size_t)s->idim * s->odim;
    }

    printf("Built forest with %d branches\n", forest->num_branches);

    // The forest is now in memory with int8 blocks.
    // You can use cce_forest_get_resident, cce_cascade_forward on them, etc.
    // For agent integration, pass the forest to your specialist router or custom generator.

    // For demo, just list a few
    for (int i = 0; i < forest->num_branches && i < 5; i++) {
        printf("  [%d] %s\n", i, forest->branches[i].name);
    }
    if (forest->num_branches > 5) printf("  ... and %d more\n", forest->num_branches - 5);

    // To persist structure (note: data not in standard .cce because int8; keep .int8data)
    cce_forest_close(forest);

    // cleanup
    for (int i = 0; i < nspecs; i++) {
        free(specs[i].scales);
        free(specs[i].codes);
        free(specs[i].bias);
    }
    free(specs);

    printf("Forest built successfully in CNET format (int8 blocks).\n");
    printf("To use in agent: load this way and wire the matmuls in your generation loop.\n");
    return 0;
}
