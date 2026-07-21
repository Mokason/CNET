#include "../../include/cce/cce_mtk.h"
#include "../../include/cce/cce_gguf.h"
#include "../../include/cce/cce_kv_page.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#pragma pack(push, 1)
typedef struct {
    char     magic[4]; /* MTSK */
    uint32_t version;
    uint8_t  method;
    float    density;
    float    drop_rate;
    uint32_t seed;
    uint8_t  encoding;
    uint8_t  semantics; /* 0 overwrite, 1 delta */
    uint8_t  _pad[2];
    uint32_t num_tensors;
    uint64_t total_modified;
    uint64_t total_elements;
} cce_mtsk_header;

typedef struct {
    uint16_t name_len;
    uint8_t  ndim;
    uint8_t  _pad;
    uint64_t num_elements;
    uint64_t num_modified;
} cce_mtsk_tensor_header;
#pragma pack(pop)

static cce_mtk_kernel_table g_kernels;
static int g_kernels_ready;
static int g_hook_installed;
static int g_hook_hits;

static uint64_t now_us(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000000ull +
               (uint64_t)ts.tv_nsec / 1000ull;
#endif
    return (uint64_t)clock() * 1000000ull / (uint64_t)CLOCKS_PER_SEC;
}

static void fire_kv_flush(cce_mtk *m) {
    if (!m || !m->kv_flush) return;
    m->kv_flush(m->kv_flush_ctx);
    m->kv_flush_count++;
}

cce_result cce_mtk_open(cce_mtk **out) {
    cce_mtk *m;
    if (!out) return CCE_ERR_INVALID_ARG;
    m = (cce_mtk *)calloc(1, sizeof *m);
    if (!m) return CCE_ERR_OOM;
    if (!g_kernels_ready) {
        cce_mtk_kernel_table_init(&g_kernels);
        g_kernels_ready = 1;
    }
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
    if (find_site(m, name)) return CCE_OK;
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
            char wname[96], biasn[96];
            if (blk->weights.data && blk->weights.numel > 0) {
                snprintf(wname, sizeof wname, "%s.b%d.w", bname, bi);
                if (cce_mtk_register(m, wname, blk->weights.data,
                                     blk->weights.numel) != CCE_OK)
                    return CCE_ERR_OOM;
            }
            if (blk->bias.data && blk->bias.numel > 0) {
                snprintf(biasn, sizeof biasn, "%s.b%d.bias", bname, bi);
                if (cce_mtk_register(m, biasn, blk->bias.data,
                                     blk->bias.numel) != CCE_OK)
                    return CCE_ERR_OOM;
            }
        }
    }
    return CCE_OK;
}

cce_result cce_mtk_bind_gguf(cce_mtk *m, struct cce_gguf_qwen2 *model) {
    int l;
    if (!m || !model) return CCE_ERR_INVALID_ARG;
    m->gguf = model;
    if (model->tok_emb.data && model->tok_emb.numel > 0)
        (void)cce_mtk_register(m, "tok_emb", model->tok_emb.data,
                               model->tok_emb.numel);
    if (model->output.data && model->output.numel > 0)
        (void)cce_mtk_register(m, "output", model->output.data,
                               model->output.numel);
    if (model->output_norm.data && model->output_norm.numel > 0)
        (void)cce_mtk_register(m, "output_norm", model->output_norm.data,
                               model->output_norm.numel);
    if (model->attn_norm && model->ffn_norm) {
        for (l = 0; l < model->n_layer; ++l) {
            char an[64], fn[64];
            if (model->attn_norm[l].data && model->attn_norm[l].numel > 0) {
                snprintf(an, sizeof an, "blk.%d.attn_norm", l);
                (void)cce_mtk_register(m, an, model->attn_norm[l].data,
                                       model->attn_norm[l].numel);
            }
            if (model->ffn_norm[l].data && model->ffn_norm[l].numel > 0) {
                snprintf(fn, sizeof fn, "blk.%d.ffn_norm", l);
                (void)cce_mtk_register(m, fn, model->ffn_norm[l].data,
                                       model->ffn_norm[l].numel);
            }
        }
    }
    if (model->forest)
        return cce_mtk_bind_forest(m, model->forest);
    return CCE_OK;
}

void cce_mtk_set_kv_flush(cce_mtk *m, cce_mtk_kv_flush_fn fn, void *ctx) {
    if (!m) return;
    m->kv_flush = fn;
    m->kv_flush_ctx = ctx;
}

void cce_mtk_gguf_kv_flush(void *gguf_qwen2_ctx) {
    cce_gguf_qwen2 *model = (cce_gguf_qwen2 *)gguf_qwen2_ctx;
    if (!model) return;
    model->cur_pos = 0;
    if (model->k_cache && model->max_ctx > 0 && model->k_slot_floats > 0)
        memset(model->k_cache, 0,
               (size_t)model->max_ctx * model->k_slot_floats * sizeof(float));
    if (model->v_cache && model->max_ctx > 0 && model->v_slot_floats > 0)
        memset(model->v_cache, 0,
               (size_t)model->max_ctx * model->v_slot_floats * sizeof(float));
    if (model->kv_pager) {
        (void)cce_kv_pager_clear(model->kv_pager);
    }
}

cce_result cce_mtk_revert(cce_mtk *m) {
    int i, restored = 0, had_active;
    uint64_t t0;
    if (!m) return CCE_ERR_INVALID_ARG;
    had_active = m->skill_active;
    t0 = now_us();
    for (i = 0; i < m->n_sites; ++i) {
        cce_mtk_site *s = &m->sites[i];
        if (!s->skill_on || !s->base_snap || !s->live) continue;
        memcpy(s->live, s->base_snap, s->n * sizeof(float));
        s->skill_on = 0;
        restored = 1;
    }
    m->skill_active = 0;
    m->skill_name[0] = 0;
    m->tensors_patched = 0;
    m->nnz_applied = 0;
    m->revert_us = now_us() - t0;
    if (restored || had_active) fire_kv_flush(m);
    return CCE_OK;
}

static int skip_u64(FILE *f, uint64_t bytes) {
    while (bytes > 0) {
        long chunk = bytes > (uint64_t)LONG_MAX ? LONG_MAX : (long)bytes;
        if (fseek(f, chunk, SEEK_CUR) != 0) return -1;
        bytes -= (uint64_t)chunk;
    }
    return 0;
}

static cce_result apply_cmsk(cce_mtk *m, FILE *f, float scale) {
    uint32_t ver = 0, method = 0, nten = 0, pad = 0;
    uint32_t ti;
    int patched = 0, nnz_tot = 0;
    uint64_t t0;

    if (fread(&ver, 4, 1, f) != 1 || fread(&method, 4, 1, f) != 1 ||
        fread(&nten, 4, 1, f) != 1 || fread(&pad, 4, 1, f) != 1)
        return CCE_ERR_IO;
    if (ver != CCE_MTK_CMSK_VER) return CCE_ERR_UNSUPPORTED;
    if (method != CCE_MTK_METHOD_DELTA && method != CCE_MTK_METHOD_OVERWRITE)
        return CCE_ERR_UNSUPPORTED;

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
            fread(&n_elem, 8, 1, f) != 1 || fread(&nnz, 8, 1, f) != 1)
            return CCE_ERR_IO;
        if (name_len == 0 || name_len >= sizeof name) return CCE_ERR_UNSUPPORTED;
        memset(name, 0, sizeof name);
        if (fread(name, 1, name_len, f) != name_len) return CCE_ERR_IO;

        s = find_site(m, name);
        if (!s || s->n != (size_t)n_elem) {
            if (nnz > UINT64_MAX / 8u || skip_u64(f, nnz * 8u) != 0)
                return CCE_ERR_IO;
            continue;
        }
        if (ensure_base_snap(s) != 0) return CCE_ERR_OOM;
        memcpy(s->live, s->base_snap, s->n * sizeof(float));

        if (nnz > 0) {
            idx = (uint32_t *)malloc((size_t)nnz * 4);
            val = (float *)malloc((size_t)nnz * 4);
            if (!idx || !val || fread(idx, 4, (size_t)nnz, f) != (size_t)nnz ||
                fread(val, 4, (size_t)nnz, f) != (size_t)nnz) {
                free(idx);
                free(val);
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
    m->tensors_patched = patched;
    m->nnz_applied = nnz_tot;
    m->apply_us = now_us() - t0;
    m->skill_active = patched > 0 ? 1 : 0;
    return patched > 0 ? CCE_OK : CCE_ERR_NOT_FOUND;
}

/* MTSK: BitnetMamba-compatible ternary skill → f32 sites. */
cce_result cce_mtk_apply_mtsk(cce_mtk *m, const char *path, float scale) {
    FILE *f = NULL;
    cce_mtsk_header hdr;
    uint32_t t;
    int patched = 0, nnz_tot = 0;
    uint64_t t0;
    cce_result err = CCE_OK;
    uint8_t *mod_mask = NULL, *packed = NULL;
    static const float tmap[4] = {-1.f, 0.f, 0.f, 1.f};

    if (!m || !path) return CCE_ERR_INVALID_ARG;
    if (scale == 0.f) scale = 0.05f; /* mild ternary inject into f32 */

    (void)cce_mtk_revert(m);

    f = fopen(path, "rb");
    if (!f) return CCE_ERR_IO;
    if (fread(&hdr, sizeof hdr, 1, f) != 1 ||
        memcmp(hdr.magic, "MTSK", 4) != 0) {
        err = CCE_ERR_UNSUPPORTED;
        goto fail;
    }
    if (hdr.version != 0 && hdr.version != CCE_MTK_MTSK_VER) {
        err = CCE_ERR_UNSUPPORTED;
        goto fail;
    }

    t0 = now_us();
    for (t = 0; t < hdr.num_tensors; ++t) {
        cce_mtsk_tensor_header th;
        char name[256];
        int rlen;
        int64_t shape[8];
        size_t mask_bytes, val_bytes;
        cce_mtk_site *s;
        size_t i, val_idx = 0, pop = 0;

        if (fread(&th, sizeof th, 1, f) != 1) {
            err = CCE_ERR_IO;
            goto fail;
        }
        if (th.name_len == 0 || th.name_len >= sizeof name || th.ndim > 8) {
            err = CCE_ERR_UNSUPPORTED;
            goto fail;
        }
        rlen = (int)th.name_len;
        if (fread(name, 1, (size_t)rlen, f) != (size_t)rlen) {
            err = CCE_ERR_IO;
            goto fail;
        }
        name[rlen] = 0;
        if (th.ndim > 0 &&
            fread(shape, sizeof(int64_t), th.ndim, f) != th.ndim) {
            err = CCE_ERR_IO;
            goto fail;
        }

        s = find_site(m, name);
        if (!s || th.num_elements != (uint64_t)s->n ||
            th.num_modified > th.num_elements ||
            th.num_elements > (uint64_t)SIZE_MAX - 7u ||
            th.num_modified > (uint64_t)SIZE_MAX - 3u) {
            err = CCE_ERR_UNSUPPORTED;
            goto fail;
        }
        mask_bytes = ((size_t)th.num_elements + 7u) / 8u;
        val_bytes = ((size_t)th.num_modified + 3u) / 4u;
        mod_mask = (uint8_t *)malloc(mask_bytes ? mask_bytes : 1);
        packed = (uint8_t *)malloc(val_bytes ? val_bytes : 1);
        if (!mod_mask || !packed ||
            fread(mod_mask, 1, mask_bytes, f) != mask_bytes ||
            fread(packed, 1, val_bytes, f) != val_bytes) {
            err = CCE_ERR_IO;
            goto fail;
        }
        for (i = 0; i < (size_t)th.num_elements; ++i)
            if (mod_mask[i / 8] & (1u << (i % 8))) pop++;
        if (pop != (size_t)th.num_modified) {
            err = CCE_ERR_UNSUPPORTED;
            goto fail;
        }

        if (ensure_base_snap(s) != 0) {
            err = CCE_ERR_OOM;
            goto fail;
        }
        memcpy(s->live, s->base_snap, s->n * sizeof(float));
        s->skill_on = 1;
        m->skill_active = 1;

        for (i = 0; i < (size_t)th.num_elements; ++i) {
            int shift;
            uint8_t enc;
            float tv;
            if (!(mod_mask[i / 8] & (1u << (i % 8)))) continue;
            shift = (int)((val_idx % 4) * 2);
            enc = (packed[val_idx / 4] >> shift) & 0x03;
            tv = tmap[enc] * scale;
            if (hdr.semantics == 1)
                s->live[i] += tv; /* delta */
            else
                s->live[i] = s->base_snap[i] + tv; /* soft overwrite */
            val_idx++;
            nnz_tot++;
        }
        if (val_idx != (size_t)th.num_modified) {
            err = CCE_ERR_UNSUPPORTED;
            goto fail;
        }
        free(mod_mask);
        free(packed);
        mod_mask = NULL;
        packed = NULL;
        patched++;
        (void)shape;
    }
    if (fclose(f) != 0) {
        f = NULL;
        err = CCE_ERR_IO;
        goto fail;
    }
    f = NULL;

    m->tensors_patched = patched;
    m->nnz_applied = nnz_tot;
    m->apply_us = now_us() - t0;
    m->skill_active = patched > 0 ? 1 : 0;
    snprintf(m->skill_name, sizeof m->skill_name, "%s", path);
    if (patched > 0) fire_kv_flush(m);
    return patched > 0 ? CCE_OK : CCE_ERR_NOT_FOUND;

fail:
    free(mod_mask);
    free(packed);
    if (f) fclose(f);
    (void)cce_mtk_revert(m);
    return err;
}

cce_result cce_mtk_apply_file(cce_mtk *m, const char *path, float scale) {
    FILE *f;
    uint32_t magic = 0;
    cce_result rc;
    if (!m || !path) return CCE_ERR_INVALID_ARG;
    if (scale == 0.f) scale = 1.f;

    (void)cce_mtk_revert(m);

    f = fopen(path, "rb");
    if (!f) return CCE_ERR_IO;
    if (fread(&magic, 4, 1, f) != 1) {
        fclose(f);
        return CCE_ERR_IO;
    }
    if (magic == CCE_MTK_CMSK_MAGIC) {
        rc = apply_cmsk(m, f, scale);
        if (rc == CCE_OK) {
            snprintf(m->skill_name, sizeof m->skill_name, "%s", path);
            fire_kv_flush(m);
        }
        fclose(f);
        if (rc != CCE_OK) (void)cce_mtk_revert(m);
        return rc;
    }
    if (magic == CCE_MTK_MTSK_MAGIC || memcmp(&magic, "MTSK", 4) == 0) {
        fclose(f);
        return cce_mtk_apply_mtsk(m, path, scale);
    }
    fclose(f);
    return CCE_ERR_UNSUPPORTED;
}

int cce_mtk_skill_active(const cce_mtk *m) {
    return m && m->skill_active ? 1 : 0;
}
int cce_mtk_n_sites(const cce_mtk *m) { return m ? m->n_sites : 0; }
int cce_mtk_tensors_patched(const cce_mtk *m) {
    return m ? m->tensors_patched : 0;
}
int cce_mtk_kv_flush_count(const cce_mtk *m) {
    return m ? m->kv_flush_count : 0;
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
    for (i = 0; i < n_tensors; ++i) {
        const cce_mtk_skill_tensor *t = &tensors[i];
        size_t name_len;
        if (!t->site_name || !t->idx || !t->val || t->nnz < 1)
            return CCE_ERR_INVALID_ARG;
        name_len = strlen(t->site_name);
        if (name_len == 0 || name_len > UINT16_MAX)
            return CCE_ERR_INVALID_ARG;
    }
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

cce_result cce_mtk_write_mtsk(const char *path,
                              const cce_mtk_skill_tensor *tensors,
                              int n_tensors) {
    FILE *f;
    cce_mtsk_header hdr;
    int i;
    if (!path || !tensors || n_tensors < 1) return CCE_ERR_INVALID_ARG;
    for (i = 0; i < n_tensors; ++i) {
        const cce_mtk_skill_tensor *t = &tensors[i];
        size_t name_len;
        if (!t->site_name || !t->idx || !t->val || t->nnz < 1)
            return CCE_ERR_INVALID_ARG;
        name_len = strlen(t->site_name);
        if (name_len == 0 || name_len > UINT16_MAX)
            return CCE_ERR_INVALID_ARG;
    }
    memset(&hdr, 0, sizeof hdr);
    memcpy(hdr.magic, "MTSK", 4);
    hdr.version = 1;
    hdr.method = 1;
    hdr.encoding = 0;
    hdr.semantics = 0; /* overwrite-style soft */
    hdr.num_tensors = (uint32_t)n_tensors;
    for (i = 0; i < n_tensors; ++i) {
        hdr.total_modified += tensors[i].nnz;
        hdr.total_elements += tensors[i].n_elem;
    }
    f = fopen(path, "wb");
    if (!f) return CCE_ERR_IO;
    fwrite(&hdr, sizeof hdr, 1, f);
    for (i = 0; i < n_tensors; ++i) {
        const cce_mtk_skill_tensor *t = &tensors[i];
        cce_mtsk_tensor_header th;
        size_t mask_bytes, val_bytes, k;
        uint8_t *mask, *packed;
        int64_t shape[1];
        memset(&th, 0, sizeof th);
        th.name_len = (uint16_t)strlen(t->site_name);
        th.ndim = 1;
        th.num_elements = t->n_elem;
        th.num_modified = t->nnz;
        shape[0] = (int64_t)t->n_elem;
        fwrite(&th, sizeof th, 1, f);
        fwrite(t->site_name, 1, th.name_len, f);
        fwrite(shape, sizeof(int64_t), 1, f);

        mask_bytes = (t->n_elem + 7) / 8;
        val_bytes = (t->nnz + 3) / 4;
        mask = (uint8_t *)calloc(mask_bytes, 1);
        packed = (uint8_t *)calloc(val_bytes, 1);
        if (!mask || !packed) {
            free(mask);
            free(packed);
            fclose(f);
            return CCE_ERR_OOM;
        }
        for (k = 0; k < t->nnz; ++k) {
            uint32_t j = t->idx[k];
            int8_t tv;
            uint8_t enc;
            int shift;
            if (j >= t->n_elem) continue;
            mask[j / 8] |= (uint8_t)(1u << (j % 8));
            tv = (t->val[k] > 0.5f) ? 1 : (t->val[k] < -0.5f) ? -1 : 0;
            enc = (tv == -1) ? 0x00 : (tv == 1) ? 0x03 : 0x01;
            shift = (int)((k % 4) * 2);
            packed[k / 4] |= (uint8_t)(enc << shift);
        }
        fwrite(mask, 1, mask_bytes, f);
        fwrite(packed, 1, val_bytes, f);
        free(mask);
        free(packed);
    }
    fclose(f);
    return CCE_OK;
}

/* ---- router ------------------------------------------------------------ */

static int word_match(const char *prompt, const char *kw) {
    const char *p;
    size_t klen;
    if (!prompt || !kw || !kw[0]) return 0;
    klen = strlen(kw);
    for (p = prompt; *p; ++p) {
        if (strncasecmp(p, kw, klen) == 0) {
            char before = (p == prompt) ? ' ' : p[-1];
            char after = p[klen];
            int b_ok = !isalnum((unsigned char)before);
            int a_ok = !after || !isalnum((unsigned char)after);
            if (b_ok && a_ok) return 1;
        }
    }
    return 0;
}

cce_result cce_mtk_router_load(cce_mtk *m, const char *routes_path) {
    FILE *f;
    char line[512];
    if (!m || !routes_path) return CCE_ERR_INVALID_ARG;
    m->n_routes = 0;
    f = fopen(routes_path, "r");
    if (!f) return CCE_ERR_IO;
    while (fgets(line, sizeof line, f) && m->n_routes < CCE_MTK_MAX_ROUTES) {
        char *p = line, *kwpart, *tok;
        cce_mtk_route *r;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == 0) continue;
        /* trim newline */
        {
            size_t L = strlen(p);
            while (L > 0 && (p[L - 1] == '\n' || p[L - 1] == '\r'))
                p[--L] = 0;
        }
        kwpart = strpbrk(p, " \t");
        if (!kwpart) continue;
        *kwpart++ = 0;
        while (*kwpart == ' ' || *kwpart == '\t') kwpart++;
        r = &m->routes[m->n_routes];
        memset(r, 0, sizeof *r);
        if (strlen(p) >= sizeof r->path) {
            fclose(f);
            m->n_routes = 0;
            return CCE_ERR_INVALID_ARG;
        }
        memcpy(r->path, p, strlen(p) + 1u);
        tok = strtok(kwpart, ",");
        while (tok && r->n_kw < CCE_MTK_MAX_KW) {
            while (*tok == ' ') tok++;
            if (strlen(tok) >= CCE_MTK_KW_LEN) {
                fclose(f);
                m->n_routes = 0;
                return CCE_ERR_INVALID_ARG;
            }
            memcpy(r->keywords[r->n_kw], tok, strlen(tok) + 1u);
            r->n_kw++;
            tok = strtok(NULL, ",");
        }
        if (r->n_kw > 0) m->n_routes++;
    }
    fclose(f);
    return m->n_routes > 0 ? CCE_OK : CCE_ERR_NOT_FOUND;
}

cce_result cce_mtk_router_apply(cce_mtk *m, const char *prompt, float scale) {
    int i, k;
    if (!m || !prompt) return CCE_ERR_INVALID_ARG;
    for (i = 0; i < m->n_routes; ++i) {
        cce_mtk_route *r = &m->routes[i];
        for (k = 0; k < r->n_kw; ++k) {
            if (word_match(prompt, r->keywords[k]))
                return cce_mtk_apply_file(m, r->path, scale);
        }
    }
    return CCE_ERR_NOT_FOUND;
}

int cce_mtk_router_n_routes(const cce_mtk *m) {
    return m ? m->n_routes : 0;
}

/* ---- kernel table + block hook ----------------------------------------- */

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

cce_mtk_kernel_table *cce_mtk_global_kernels(void) {
    if (!g_kernels_ready) {
        cce_mtk_kernel_table_init(&g_kernels);
        g_kernels_ready = 1;
    }
    return &g_kernels;
}

/* Default LEGO linear: notify GEMM slot then fall through (NOT_FOUND).
 * A custom slot that sets a process flag can replace work entirely. */
static cce_result mtk_linear_hook(const cce_block *blk, const cce_tensor *in,
                                  cce_tensor *out, void *ctx) {
    cce_mtk_kernel_table *t = cce_mtk_global_kernels();
    (void)blk;
    (void)in;
    (void)out;
    (void)ctx;
    if (t->slots[CCE_MTK_K_GEMM] && t->slots[CCE_MTK_K_GEMM] != mtk_k_noop) {
        t->slots[CCE_MTK_K_GEMM]((void *)blk);
        g_hook_hits++;
        /* Custom kernels signal full handle by returning via a side channel:
           if slot is mtk_k_noop we never get here. For demo, fall through
           unless CNET_MTK_HOOK_TAKEOVER=1. */
        {
            const char *e = getenv("CNET_MTK_HOOK_TAKEOVER");
            if (e && e[0] == '1') {
                /* Zero out as proof of takeover (tests check hook_hits). */
                if (out && out->data && out->numel)
                    memset(out->data, 0, out->numel * sizeof(float));
                return CCE_OK;
            }
        }
    }
    return CCE_ERR_NOT_FOUND;
}

void cce_mtk_install_block_hook(void) {
    cce_block_set_linear_hook(mtk_linear_hook, NULL);
    g_hook_installed = 1;
}

void cce_mtk_uninstall_block_hook(void) {
    cce_block_set_linear_hook(NULL, NULL);
    g_hook_installed = 0;
}
