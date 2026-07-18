#ifndef CCE_MTK_H
#define CCE_MTK_H

/*
 * CNET Micro-Trensor Kernel (MTK)
 * ==============================
 * LEGO knowledge swap on live Forest / GGUF weights without rewriting forward
 * logic. Skills are cartridges (CMSK f32 sparse, MTSK ternary-compatible).
 *
 * Phases:
 *   1  CMSK apply/revert + Forest bind
 *   2  MTSK/.tskill ternary reader → f32 sites
 *   3  Kernel vtable hooked into cce_block_forward (linear)
 *   4  GGUF named-tensor registry
 *   5  Auto-router + KV flush on swap
 *
 * Gate: make mtk → MTK_PASS
 */

#include "cce_defs.h"
#include "cce_forest.h"
#include "cce_block.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward decl — full type in cce_gguf.h; avoid heavy include when unused. */
struct cce_gguf_qwen2;

/* CMSK = CNET Micro Skill (f32 sparse). MTSK = Micro-Trensor Skill (ternary). */
#define CCE_MTK_CMSK_MAGIC 0x4B534D43u /* 'CMSK' LE */
#define CCE_MTK_CMSK_VER   1u
#define CCE_MTK_MTSK_MAGIC 0x4B53544Du /* 'MTSK' LE */
#define CCE_MTK_MTSK_VER   1u

#define CCE_MTK_METHOD_DELTA     1
#define CCE_MTK_METHOD_OVERWRITE 2

#define CCE_MTK_MAX_ROUTES 16
#define CCE_MTK_MAX_KW     8
#define CCE_MTK_KW_LEN     32

typedef struct cce_mtk cce_mtk;

typedef struct {
    char     name[96];
    float   *live;
    float   *base_snap;
    size_t   n;
    int      skill_on;
} cce_mtk_site;

typedef struct {
    char path[256];
    char keywords[CCE_MTK_MAX_KW][CCE_MTK_KW_LEN];
    int  n_kw;
} cce_mtk_route;

/* Called on every successful apply/revert so hosts can drop KV / cur_pos. */
typedef void (*cce_mtk_kv_flush_fn)(void *ctx);

struct cce_mtk {
    cce_mtk_site *sites;
    int           n_sites;
    int           cap_sites;
    char          skill_name[160];
    int           skill_active;
    uint64_t      apply_us;
    uint64_t      revert_us;
    int           tensors_patched;
    int           nnz_applied;
    cce_forest   *forest;
    struct cce_gguf_qwen2 *gguf; /* not owned */
    /* router */
    cce_mtk_route routes[CCE_MTK_MAX_ROUTES];
    int           n_routes;
    /* kv flush */
    cce_mtk_kv_flush_fn kv_flush;
    void               *kv_flush_ctx;
    int                 kv_flush_count;
};

/* ---- lifecycle --------------------------------------------------------- */

cce_result cce_mtk_open(cce_mtk **out);
void       cce_mtk_close(cce_mtk *m);

cce_result cce_mtk_register(cce_mtk *m, const char *name, float *live,
                            size_t n_elem);

/* Register Forest cascade sites: "{branch}.b{i}.w" / ".bias". */
cce_result cce_mtk_bind_forest(cce_mtk *m, cce_forest *f);

/* Register GGUF model tensors: tok_emb, output, norms + forest branches. */
cce_result cce_mtk_bind_gguf(cce_mtk *m, struct cce_gguf_qwen2 *model);

/* ---- skill apply / revert ---------------------------------------------- */

/* Auto-detect CMSK or MTSK by magic. Reverts prior skill first; runs KV flush. */
cce_result cce_mtk_apply_file(cce_mtk *m, const char *path, float scale);

/* Explicit MTSK/.tskill ternary (BitnetMamba-compatible layout). */
cce_result cce_mtk_apply_mtsk(cce_mtk *m, const char *path, float scale);

cce_result cce_mtk_revert(cce_mtk *m);

int  cce_mtk_skill_active(const cce_mtk *m);
int  cce_mtk_n_sites(const cce_mtk *m);
int  cce_mtk_tensors_patched(const cce_mtk *m);
int  cce_mtk_kv_flush_count(const cce_mtk *m);

void cce_mtk_set_kv_flush(cce_mtk *m, cce_mtk_kv_flush_fn fn, void *ctx);

/* Built-in flush for GGUF: zero cur_pos (+ dense k/v if present). */
void cce_mtk_gguf_kv_flush(void *gguf_qwen2_ctx);

/* ---- CMSK / MTSK writers (tests + export) ------------------------------ */

typedef struct {
    const char *site_name;
    size_t      n_elem;
    size_t      nnz;
    const uint32_t *idx;
    const float    *val;
} cce_mtk_skill_tensor;

cce_result cce_mtk_write_cmsk(const char *path, int method,
                              const cce_mtk_skill_tensor *tensors, int n_tensors);

/* Write minimal MTSK (ternary overwrite) for tests. val[i] in {-1,0,+1}. */
cce_result cce_mtk_write_mtsk(const char *path,
                              const cce_mtk_skill_tensor *tensors, int n_tensors);

/* ---- auto-router ------------------------------------------------------- */

/* Load routes file. Lines:
 *   path/to/skill.cmsk  keyword1,keyword2
 *   # comments and blank lines ignored
 */
cce_result cce_mtk_router_load(cce_mtk *m, const char *routes_path);

/* Match prompt keywords → apply first matching skill. Returns NOT_FOUND if none. */
cce_result cce_mtk_router_apply(cce_mtk *m, const char *prompt, float scale);

int cce_mtk_router_n_routes(const cce_mtk *m);

/* ---- LEGO kernel table (process-global, wired into cce_block_forward) -- */

typedef enum {
    CCE_MTK_K_GEMM = 0,
    CCE_MTK_K_RMS,
    CCE_MTK_K_ATTN,
    CCE_MTK_K_COUNT
} cce_mtk_kernel_id;

typedef void (*cce_mtk_kernel_fn)(void *ctx);

typedef struct {
    cce_mtk_kernel_fn slots[CCE_MTK_K_COUNT];
    cce_mtk_kernel_fn defaults[CCE_MTK_K_COUNT];
} cce_mtk_kernel_table;

void cce_mtk_kernel_table_init(cce_mtk_kernel_table *t);
cce_mtk_kernel_fn cce_mtk_kernel_set(cce_mtk_kernel_table *t,
                                     cce_mtk_kernel_id id,
                                     cce_mtk_kernel_fn fn);
cce_mtk_kernel_fn cce_mtk_kernel_get(const cce_mtk_kernel_table *t,
                                     cce_mtk_kernel_id id);

/* Process-global table used by cce_block linear hook. */
cce_mtk_kernel_table *cce_mtk_global_kernels(void);

/* Install / clear block linear hook (default GEMM path when unset).
 * custom_linear: if non-NULL, cce_block_forward may call it for LINEAR/HEAD.
 * Return CCE_OK to skip built-in matvec; CCE_ERR_NOT_FOUND to fall through.
 */
void cce_mtk_install_block_hook(void);
void cce_mtk_uninstall_block_hook(void);

#ifdef __cplusplus
}
#endif

#endif /* CCE_MTK_H */
