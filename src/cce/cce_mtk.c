#include "../../include/cce/cce_mtk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t now_us(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000000ull +
               (uint64_t)ts.tv_nsec / 1000ull;
#endif
    return (uint64_t)clock() * 1000000ull / (uint64_t)CLOCKS_PER_SEC;
}

cce_result cce_mtk_open(cce_mtk **out) {
    cce_mtk *m;
    if (!out) return CCE_ERR_INVALID_ARG;
    m = (cce_mtk *)calloc(1, sizeof *m);
    if (!m) return CCE_ERR_OOM;
    *out = m;
    return CCE_OK;
}

void cce_mtk_close(cce_mtk *m) {
    int i;
    if (!m) return;
    (void)cce_mtk_revert(m);
    for (i = 0; i < m->n_sites; ++i) free(m->sites[i].base_snap);
    free(m->sites);
    free(m);
}

static cce_mtk_site *find_site(cce_mtk *m, const char *name) {
    int i;
    if (!m || !name) return NULL;
    for (i = 0; i < m->n_sites; ++i)
        if (strcmp(m->sites[i].name, name) == 0) return &m->sites[i];
    return NULL;
}

cce_result cce_mtk_register(cce_mtk *m, const char *name, float *live,
                            size_t n_elem) {
    cce_mtk_site *s;
    if (!m || !name || !name[0] || !live || n_elem < 1)
        return CCE_ERR_INVALID_ARG;
    if (find_site(m, name)) return CCE_OK; /* idempotent */
    if (m->n_sites >= m->cap_sites) {
        int nc = m->cap_sites ? m->cap_sites * 2 : 32;
        cce_mtk_site *ns =
            (cce_mtk_site *)realloc(m->sites, (size_t)nc * sizeof *ns);
        if (!ns) return CCE_ERR_OOM;
        m->sites = ns;
        m->cap_sites = nc;
    }
    s = &m->sites[m->n_sites++];
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->live = live;
    s->n = n_elem;
    return CCE_OK;
}

static int ensure_base_snap(cce_mtk_site *s) {
    if (s->base_snap) return 0;
    s->base_snap = (float *)malloc(s->n * sizeof(float));
    if (!s->base_snap) return -1;
    memcpy(s->base_snap, s->live, s->n * sizeof(float));
    return 0;
}

cce_result cce_mtk_bind_forest(cce_mtk *m, cce_forest *f) {
    int b, bi;
    if (!m || !f) return CCE_ERR_INVALID_ARG;
    m->forest = f;
    for (b = 0; b < f->num_branches; ++b) {
        cce_cascade *cas;
        char bname[64];
        if (cce_forest_branch_name(f, b, bname, (int)sizeof bname) != CCE_OK)
            continue;
        cas = cce_forest_get_resident(f, bname);
        if (!cas) continue;
        for (bi = 0; bi < cas->num_blocks; ++bi) {
            cce_block *blk = &cas->blocks[bi];
            char wname[96], bname2[96];
            if (blk->weights.data && blk->weights.numel > 0) {
                snprintf(wname, sizeof wname, "%s.b%d.w", bname, bi);
                if (cce_mtk_register(m, wname, blk->weights.data,
                                     blk->weights.numel) != CCE_OK)
                    return CCE_ERR_OOM;
            }
            if (blk->bias.data && blk->bias.numel > 0) {
                snprintf(bname2, sizeof bname2, "%s.b%d.bias", bname, bi);
                if (cce_mtk_register(m, bname2, blk->bias.data,
                                     blk->bias.numel) != CCE_OK)
                    return CCE_ERR_OOM;
            }
        }
    }
    return CCE_OK;
}

cce_result cce_mtk_revert(cce_mtk *m) {
    int i;
    uint64_t t0;
    if (!m) return CCE_ERR_INVALID_ARG;
    if (!m->skill_active) return CCE_OK;
    t0 = now_us();
    for (i = 0; i < m->n_sites; ++i) {
        cce_mtk_site *s = &m->sites[i];
        if (!s->skill_on || !s->base_snap || !s->live) continue;
        memcpy(s->live, s->base_snap, s->n * sizeof(float));
        s->skill_on = 0;
    }
    m->skill_active = 0;
    m->skill_name[0] = 0;
    m->tensors_patched = 0;
    m->nnz_applied = 0;
    m->revert_us = now_us() - t0;
    return CCE_OK;
}

/* CMSK on-disk layout (little-endian):
 *   u32 magic, u32 version, u32 method, u32 num_tensors, u32 _pad
 *   per tensor:
 *     u16 name_len, u16 _pad
 *     u64 n_elem, u64 nnz
 *     char name[name_len]  (not NUL-padded required; we write with NUL)
 *     u32 idx[nnz]
 *     f32 val[nnz]
 */
cce_result cce_mtk_apply_file(cce_mtk *m, const char *path, float scale) {
    FILE *f;
    uint32_t magic = 0, ver = 0, method = 0, nten = 0, pad = 0;
    uint32_t ti;
    uint64_t t0;
    int patched = 0, nnz_tot = 0;
    if (!m || !path) return CCE_ERR_INVALID_ARG;
    if (scale == 0.f) scale = 1.f;

    /* Revert previous skill so apply is always from base. */
    (void)cce_mtk_revert(m);

    f = fopen(path, "rb");
    if (!f) return CCE_ERR_IO;
    if (fread(&magic, 4, 1, f) != 1 || fread(&ver, 4, 1, f) != 1 ||
        fread(&method, 4, 1, f) != 1 || fread(&nten, 4, 1, f) != 1 ||
        fread(&pad, 4, 1, f) != 1) {
        fclose(f);
        return CCE_ERR_IO;
    }
    if (magic != CCE_MTK_CMSK_MAGIC || ver != CCE_MTK_CMSK_VER) {
        fclose(f);
        return CCE_ERR_UNSUPPORTED;
    }
    if (method != CCE_MTK_METHOD_DELTA && method != CCE_MTK_METHOD_OVERWRITE) {
        fclose(f);
        return CCE_ERR_UNSUPPORTED;
    }

    t0 = now_us();
    for (ti = 0; ti < nten; ++ti) {
        uint16_t name_len = 0, npad = 0;
        uint64_t n_elem = 0, nnz = 0;
        char name[96];
        cce_mtk_site *s;
        uint32_t *idx = NULL;
        float *val = NULL;
        size_t k;

        if (fread(&name_len, 2, 1, f) != 1 || fread(&npad, 2, 1, f) != 1 ||
            fread(&n_elem, 8, 1, f) != 1 || fread(&nnz, 8, 1, f) != 1) {
            fclose(f);
            return CCE_ERR_IO;
        }
        if (name_len == 0 || name_len >= sizeof name) {
            fclose(f);
            return CCE_ERR_UNSUPPORTED;
        }
        memset(name, 0, sizeof name);
        if (fread(name, 1, name_len, f) != name_len) {
            fclose(f);
            return CCE_ERR_IO;
        }
        name[name_len < sizeof name ? name_len : sizeof name - 1] = 0;

        s = find_site(m, name);
        if (!s || s->n != (size_t)n_elem) {
            /* Skip unknown or size-mismatched tensors (soft). */
            if (fseek(f, (long)(nnz * 4 + nnz * 4), SEEK_CUR) != 0) {
                fclose(f);
                return CCE_ERR_IO;
            }
            continue;
        }
        if (ensure_base_snap(s) != 0) {
            fclose(f);
            return CCE_ERR_OOM;
        }
        /* Start from base for this site. */
        memcpy(s->live, s->base_snap, s->n * sizeof(float));

        if (nnz > 0) {
            idx = (uint32_t *)malloc((size_t)nnz * sizeof(uint32_t));
            val = (float *)malloc((size_t)nnz * sizeof(float));
            if (!idx || !val ||
                fread(idx, 4, (size_t)nnz, f) != (size_t)nnz ||
                fread(val, 4, (size_t)nnz, f) != (size_t)nnz) {
                free(idx);
                free(val);
                fclose(f);
                return CCE_ERR_IO;
            }
            for (k = 0; k < (size_t)nnz; ++k) {
                uint32_t j = idx[k];
                if (j >= s->n) continue;
                if (method == CCE_MTK_METHOD_OVERWRITE)
                    s->live[j] = scale * val[k];
                else
                    s->live[j] += scale * val[k];
            }
            nnz_tot += (int)nnz;
            free(idx);
            free(val);
        }
        s->skill_on = 1;
        patched++;
    }
    fclose(f);

    m->skill_active = patched > 0 ? 1 : 0;
    snprintf(m->skill_name, sizeof m->skill_name, "%s", path);
    m->tensors_patched = patched;
    m->nnz_applied = nnz_tot;
    m->apply_us = now_us() - t0;
    return patched > 0 ? CCE_OK : CCE_ERR_NOT_FOUND;
}

int cce_mtk_skill_active(const cce_mtk *m) {
    return m && m->skill_active ? 1 : 0;
}
int cce_mtk_n_sites(const cce_mtk *m) { return m ? m->n_sites : 0; }
int cce_mtk_tensors_patched(const cce_mtk *m) {
    return m ? m->tensors_patched : 0;
}

cce_result cce_mtk_write_cmsk(const char *path, int method,
                              const cce_mtk_skill_tensor *tensors,
                              int n_tensors) {
    FILE *f;
    uint32_t magic = CCE_MTK_CMSK_MAGIC, ver = CCE_MTK_CMSK_VER, meth, nten, pad;
    int i;
    if (!path || !tensors || n_tensors < 1) return CCE_ERR_INVALID_ARG;
    if (method != CCE_MTK_METHOD_DELTA && method != CCE_MTK_METHOD_OVERWRITE)
        method = CCE_MTK_METHOD_DELTA;
    meth = (uint32_t)method;
    nten = (uint32_t)n_tensors;
    pad = 0;
    f = fopen(path, "wb");
    if (!f) return CCE_ERR_IO;
    fwrite(&magic, 4, 1, f);
    fwrite(&ver, 4, 1, f);
    fwrite(&meth, 4, 1, f);
    fwrite(&nten, 4, 1, f);
    fwrite(&pad, 4, 1, f);
    for (i = 0; i < n_tensors; ++i) {
        const cce_mtk_skill_tensor *t = &tensors[i];
        uint16_t nl, np = 0;
        uint64_t ne, nnz;
        if (!t->site_name || !t->idx || !t->val || t->nnz < 1) {
            fclose(f);
            return CCE_ERR_INVALID_ARG;
        }
        nl = (uint16_t)strlen(t->site_name);
        ne = (uint64_t)t->n_elem;
        nnz = (uint64_t)t->nnz;
        fwrite(&nl, 2, 1, f);
        fwrite(&np, 2, 1, f);
        fwrite(&ne, 8, 1, f);
        fwrite(&nnz, 8, 1, f);
        fwrite(t->site_name, 1, nl, f);
        fwrite(t->idx, 4, t->nnz, f);
        fwrite(t->val, 4, t->nnz, f);
    }
    fclose(f);
    return CCE_OK;
}

/* ---- kernel table stub ------------------------------------------------- */

static void mtk_k_noop(void *ctx) { (void)ctx; }

void cce_mtk_kernel_table_init(cce_mtk_kernel_table *t) {
    int i;
    if (!t) return;
    memset(t, 0, sizeof *t);
    for (i = 0; i < CCE_MTK_K_COUNT; ++i) {
        t->defaults[i] = mtk_k_noop;
        t->slots[i] = mtk_k_noop;
    }
}

cce_mtk_kernel_fn cce_mtk_kernel_set(cce_mtk_kernel_table *t,
                                     cce_mtk_kernel_id id,
                                     cce_mtk_kernel_fn fn) {
    cce_mtk_kernel_fn prev;
    if (!t || id < 0 || id >= CCE_MTK_K_COUNT) return NULL;
    prev = t->slots[id];
    t->slots[id] = fn ? fn : t->defaults[id];
    return prev;
}

cce_mtk_kernel_fn cce_mtk_kernel_get(const cce_mtk_kernel_table *t,
                                     cce_mtk_kernel_id id) {
    if (!t || id < 0 || id >= CCE_MTK_K_COUNT) return NULL;
    return t->slots[id];
}
