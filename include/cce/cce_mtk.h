#ifndef CCE_MTK_H
#define CCE_MTK_H

/*
 * CNET Micro-Trensor Kernel (MTK) — Phase 1
 * =========================================
 * LEGO knowledge swap on live Forest/cascade weights without rewriting
 * forward logic. Skills are cartridge files (CMSK) applied via shadow
 * snapshots; revert restores base exactly.
 *
 * Inspired by Marble-s-BitnetMamba hot_swap + .tskill (MTSK), adapted to
 * pure-C CCE tensors (no Python / no BitNetModel structs).
 *
 * Wire-up:
 *   cce_mtk_open → cce_mtk_bind_forest (or register sites) →
 *   cce_mtk_apply_file("skill.cmsk") → forward as usual →
 *   cce_mtk_revert → cce_mtk_close
 *
 * Gate: make mtk → MTK_PASS
 */

#include "cce_defs.h"
#include "cce_forest.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CMSK = CNET Micro Skill (f32 sparse delta). Compatible spirit of MTSK. */
#define CCE_MTK_CMSK_MAGIC 0x4B534D43u /* 'CMSK' little-endian */
#define CCE_MTK_CMSK_VER   1u

/* Method flags in header */
#define CCE_MTK_METHOD_DELTA 1 /* sparse index+delta added to base */
#define CCE_MTK_METHOD_OVERWRITE 2 /* sparse index+value replace base */

typedef struct cce_mtk cce_mtk;

typedef struct {
    char     name[96];
    float   *live;       /* points into cascade block weights */
    float   *base_snap;  /* owned copy of base at bind / first apply */
    size_t   n;
    int      skill_on;   /* 1 if live currently holds skill-applied weights */
} cce_mtk_site;

struct cce_mtk {
    cce_mtk_site *sites;
    int           n_sites;
    int           cap_sites;
    char          skill_name[160];
    int           skill_active;
    /* telemetry */
    uint64_t      apply_us;
    uint64_t      revert_us;
    int           tensors_patched;
    int           nnz_applied;
    /* optional forest (not owned) */
    cce_forest   *forest;
};

/* ---- lifecycle --------------------------------------------------------- */

cce_result cce_mtk_open(cce_mtk **out);
void       cce_mtk_close(cce_mtk *m);

/* Register a live f32 weight buffer under `name` (e.g. "q_proj.b0.w").
 * Does not take ownership of live; snapshots base on first apply. */
cce_result cce_mtk_register(cce_mtk *m, const char *name, float *live,
                            size_t n_elem);

/* Bind every HOT forest branch cascade linear weight as:
 *   "{branch}.b{block_idx}.w"  and  "{branch}.b{block_idx}.bias" (if present).
 * Re-bind after residency changes if needed. */
cce_result cce_mtk_bind_forest(cce_mtk *m, cce_forest *f);

/* ---- skill apply / revert ---------------------------------------------- */

/* Apply CMSK skill file. Reverts any active skill first. */
cce_result cce_mtk_apply_file(cce_mtk *m, const char *path, float scale);

/* Exact restore of all registered sites to base snapshots. */
cce_result cce_mtk_revert(cce_mtk *m);

int  cce_mtk_skill_active(const cce_mtk *m);
int  cce_mtk_n_sites(const cce_mtk *m);
int  cce_mtk_tensors_patched(const cce_mtk *m);

/* ---- CMSK writer (for tests / skill export) ---------------------------- */

typedef struct {
    const char *site_name;
    size_t      n_elem;     /* full tensor length (must match site) */
    size_t      nnz;
    const uint32_t *idx;    /* [nnz] */
    const float    *val;    /* [nnz] deltas or overwrites */
} cce_mtk_skill_tensor;

/* Write a CMSK file (METHOD_DELTA unless method=OVERWRITE). */
cce_result cce_mtk_write_cmsk(const char *path, int method,
                              const cce_mtk_skill_tensor *tensors, int n_tensors);

/* ---- optional LEGO kernel table (phase-1 stub, real swaps later) ------ */

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
/* Install override (NULL = restore default). Returns previous. */
cce_mtk_kernel_fn cce_mtk_kernel_set(cce_mtk_kernel_table *t,
                                     cce_mtk_kernel_id id,
                                     cce_mtk_kernel_fn fn);
cce_mtk_kernel_fn cce_mtk_kernel_get(const cce_mtk_kernel_table *t,
                                     cce_mtk_kernel_id id);

#ifdef __cplusplus
}
#endif

#endif /* CCE_MTK_H */
