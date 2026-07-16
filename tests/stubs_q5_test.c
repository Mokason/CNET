/* stubs_q5_test.c — stub out external deps of cce_gguf.c that the Q5_K
   integrity test never exercises (model forward, forest, cascade, GPU, etc.).
   The test only calls cce_gguf_load / cce_gguf_free / cce_gguf_load_f32 /
   cce_gguf_tensor_count / cce_gguf_f16_to_f32, so these stubs are never
   entered.  Keeps the link self-contained. */
#include <stddef.h>
#include <stdint.h>

/* ---- cce_tensor ---- */
int cce_tensor_alloc(void* t, const int* shape, int nd) { (void)t;(void)shape;(void)nd; return -1; }
void cce_tensor_free(void* t) { (void)t; }

/* ---- cce_block ---- */
int cce_block_quantize_int8(void* b) { (void)b; return -1; }
int cce_block_quantize_ternary(void* b) { (void)b; return -1; }
int cce_block_pack_trits(void* b) { (void)b; return -1; }

/* ---- cce_cascade ---- */
void* cce_cascade_create(void) { return NULL; }
void cce_cascade_destroy(void* c) { (void)c; }
int cce_cascade_forward(void* c, const float* in, float* out) { (void)c;(void)in;(void)out; return -1; }
int cce_cascade_add_linear_head(void* c, const void* w, int r, int c_, int tr) {
    (void)c;(void)w;(void)r;(void)c_;(void)tr; return -1;
}

/* ---- cce_forest ---- */
void* cce_forest_open(const char* path, int n) { (void)path;(void)n; return NULL; }
int cce_forest_close(void* f) { (void)f; return 0; }
int cce_forest_add_cascade_branch(void* f, void* c, const char* n) {
    (void)f;(void)c;(void)n; return -1;
}
int cce_forest_set_residency(void* f, int idx, int r) { (void)f;(void)idx;(void)r; return -1; }
void* cce_forest_get_resident(void* f, int idx) { (void)f;(void)idx; return NULL; }

/* ---- cce_clgemm ---- */
int cce_clgemm_matmul(const void* h, const void* a, const void* b, void* o,
                      int M, int N, int K) { (void)h;(void)a;(void)b;(void)o;(void)M;(void)N;(void)K; return -1; }
int cce_clgemm_matmul_q8(const void* h, const void* a, const void* b, void* o,
                         int M, int N, int K) { (void)h;(void)a;(void)b;(void)o;(void)M;(void)N;(void)K; return -1; }

/* ---- cce_weight_store ---- */
int cce_weight_store_put(void* s, const char* key, const void* data, size_t n) {
    (void)s;(void)key;(void)data;(void)n; return -1;
}
const void* cce_weight_store_get_opt(void* s, const char* key, size_t* n) {
    (void)s;(void)key;(void)n; return NULL;
}

/* ---- cce_gguf_qwen35 ---- */
int cce_gguf_load_qwen35(void** out, const char* path) { (void)out;(void)path; return -6; }
void cce_gguf_qwen35_ext_free(void* e) { (void)e; }
int cce_gguf_qwen35_forward_impl(void* m, const int* t, int n, float* l, int c) {
    (void)m;(void)t;(void)n;(void)l;(void)c; return -6;
}