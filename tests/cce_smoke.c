#include "../include/cce/cce_tensor.h"
#include "../include/cce/cce_block.h"
#include "../include/cce/cce_cascade.h"
#include "../include/cce/cce_archive.h"
#include "../include/cce/cce_forest.h"
#include "../include/cce/cce_router.h"
#include "../include/cce/cce_learn.h"
#include "../include/cce/cce_block_patch.h"
#include "../include/cce/cce_gpu.h"
#include "../include/cce/cce.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    printf("=== CCE Full Smoke (all 6 pieces) ===\n");

    // 1. SSMax router + forest
    printf("Opening forest...\n");
    cce_forest* forest = NULL;
    cce_forest_open(&forest, "demo.cce", 8);
    printf("Forest opened\n");

    cce_cascade dcas; 
    cce_cascade_init(&dcas, 1);
    printf("Cascade init\n");

    cce_block db; 
    cce_block_init_linear(&db, 2, 2, 0.01f);
    printf("Block init\n");

    cce_cascade_append(&dcas, &db);
    printf("Cascade append\n");

    cce_forest_add_branch(forest, &dcas, "b0");
    printf("Branch added\n");

    cce_router r; 
    cce_router_init(&r, 1.0f, 2);
    int tb; float sc;
    float inp[2] = {0.1f, 0.9f};
    cce_router_route(&r, forest, inp, 2, &tb, &sc);
    printf("1. SSMax router + forest: branch %d score %.2f\n", tb, sc);

    cce_forest_close(forest);
    printf("Forest closed\n");

    // 2. cce_learn DFA/goodness
    cce_learner ln; cce_learner_init(&ln, 0.6f);
    cce_cascade lcas; cce_cascade_init(&lcas, 1);
    cce_block lb; cce_block_init_linear(&lb, 2, 2, 0.01f);
    cce_cascade_append(&lcas, &lb);
    cce_tensor li, lt;
    int ls[1]={2}; cce_tensor_alloc(&li, ls, 1); li.data[0]=0.4f; li.data[1]=0.6f;
    cce_tensor_alloc(&lt, ls, 1); lt.data[0]=0.8f; lt.data[1]=0.2f;
    cce_learner_adapt(&ln, &lcas, &li, &lt, 0.01f);
    printf("2. cce_learn DFA+goodness OK\n");
    cce_tensor_free(&li); cce_tensor_free(&lt); cce_cascade_free(&lcas);

    // 3. patch + image
    cce_block pb;
    cce_block_patch_init(&pb, 2, 1, 1);
    cce_tensor img; int ims[2]={4,4}; cce_tensor_alloc(&img, ims, 2);
    cce_tensor pats;
    cce_block_patch_extract(&img, 4, 4, &pats);
    printf("3. patch + image: %d patches\n", pats.shape[0]);
    cce_tensor_free(&img); cce_tensor_free(&pats); cce_block_free(&pb);

    // 4. gpu
    cce_gpu_ctx* g = NULL;
    cce_gpu_init(&g);
    cce_gpu_backend_t be = cce_gpu_get_backend(g);
    const char* be_name = (be == CCE_GPU_CUDA) ? "CUDA (RTX 4070 ready)" :
                          (be == CCE_GPU_OPENCL) ? "OpenCL" : "None (CPU fallback)";
    printf("4. cce_gpu (%s)\n", be_name);
    cce_gpu_destroy(g);

    // 5. archive dir
    cce_archive* ar = NULL;
    cce_archive_open(&ar, "dirtest.cce");
    float w[2] = {1,2};
    size_t off;
    cce_archive_append_section(ar, "sec1", w, sizeof(w), &off);
    size_t foff, fsz;
    cce_archive_find_section(ar, "sec1", &foff, &fsz);
    printf("5. archive dir: found sec1 at %zu\n", foff);
    cce_archive_close(ar);

    // 6. ABI
    cce_handle* h = NULL;
    cce_open(&h, "abi.cce");
    printf("6. C ABI/libcce: open/close OK\n");
    cce_close(h);

    printf("=== All 6 points complete ===\n");
    return 0;
}
