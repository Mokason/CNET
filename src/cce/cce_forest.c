#include "../../include/cce/cce_forest.h"
#include "../../include/cce/cce_block_patch.h"
#include "../../include/cce/cce_router.h"
#include "../../include/cnet_lfru.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

static float distance(const float* a, const float* b, int dim) {
    float sum = 0.0f;
    for (int i = 0; i < dim; ++i) {
        float d = a[i] - b[i];
        sum += d * d;
    }
    return sum;
}

static void mark_cascade_moved(cce_cascade* cas) {
    if (cas) memset(cas, 0, sizeof(*cas));
}

cce_result cce_forest_open(cce_forest** forest_out, const char* archive_path, int max_branches) {
    if (!forest_out || !archive_path || max_branches <= 0) return CCE_ERR_INVALID_ARG;

    cce_forest* f = (cce_forest*)calloc(1, sizeof(cce_forest));
    if (!f) return CCE_ERR_OOM;

    if (cce_archive_open(&f->archive, archive_path) != CCE_OK) {
        free(f);
        return CCE_ERR_IO;
    }

    f->branches = (cce_branch*)calloc(max_branches, sizeof(cce_branch));
    f->max_branches = max_branches;
    f->centroid_dim = 8; /* small for demo */
    f->centroids = (float*)calloc(max_branches * f->centroid_dim, sizeof(float));
    f->diff_mode = CCE_DIFF_LOCAL; /* default to core local mode */
    f->default_exact_tail_length = 2;

    if (!f->branches || !f->centroids) {
        cce_archive_close(f->archive);
        free(f->branches);
        free(f->centroids);
        free(f);
        return CCE_ERR_OOM;
    }

    *forest_out = f;
    return CCE_OK;
}

void cce_forest_close(cce_forest* f) {
    if (!f) return;

    for (int i = 0; i < f->num_branches; ++i) {
        /* Free any resident cascade, owned (HOT) or view (WARM): cce_cascade_free
           skips view tensors (owns_memory=0) and frees only owned ones. */
        if (f->branches[i].cascade) {
            cce_cascade_free(f->branches[i].cascade);
            free(f->branches[i].cascade);
        }
    }

    cce_archive_close(f->archive);
    free(f->branches);
    free(f->centroids);
    free(f);
}

/* ---- residency (tiered runtime) ---- */

void cce_forest_set_residency(cce_forest* f, int hot_cap,
                              cce_cascade* (*provider)(void*, const char*), void* ctx) {
    if (!f) return;
    if (provider && hot_cap < 8) hot_cap = 8; /* transformer layer working set */
    f->hot_cap = provider ? hot_cap : 0;
    f->residency_provider = provider;
    f->residency_ctx = ctx;
    {
        const char *e = getenv("CNET_FOREST_LFRU");
        f->lfru = (e && e[0] == '1') ? 1 : 0;
    }
    if (!provider) {
        for (int i = 0; i < f->num_branches; i++) f->branches[i].evictable = 0;
    }
}

void cce_forest_set_lfru(cce_forest* f, int enabled) {
    if (f) f->lfru = enabled ? 1 : 0;
}

int cce_forest_lfru(const cce_forest* f) {
    return f ? f->lfru : 0;
}

void cce_forest_heat_decay(cce_forest* f) {
    int i;
    if (!f) return;
    for (i = 0; i < f->num_branches; i++) f->branches[i].heat >>= 1;
}

int cce_forest_resident_count(const cce_forest* f) {
    if (!f) return 0;
    int n = 0;
    for (int i = 0; i < f->num_branches; i++)
        if (f->branches[i].evictable && f->branches[i].cascade) n++;
    return n;
}

int cce_forest_resident_high_water(const cce_forest* f) {
    return f ? f->resident_high_water : 0;
}

cce_result cce_forest_evict_branch(cce_forest* f, int idx) {
    if (!f || idx < 0 || idx >= f->num_branches) return CCE_ERR_INVALID_ARG;
    cce_branch* b = &f->branches[idx];
    if (!b->evictable || !b->cascade || b->is_view) return CCE_ERR_INVALID_ARG;
    cce_cascade_free(b->cascade);
    free(b->cascade);
    b->cascade = NULL;
    b->tier = CCE_TIER_COLD;
    return CCE_OK;
}

/* evict LRU (or LFRU when f->lfru) until fewer than `target` are resident */
static void forest_make_room(cce_forest* f, int target, int protect_idx) {
    while (cce_forest_resident_count(f) >= target) {
        int victim = -1;
        if (f->lfru) {
            int res[256], nres = 0;
            for (int i = 0; i < f->num_branches && nres < 256; i++) {
                cce_branch* b = &f->branches[i];
                if (i == protect_idx || !b->evictable || !b->cascade || b->is_view)
                    continue;
                res[nres++] = i;
            }
            if (nres < 1) return;
            {
                int cold_slot = 0;
                uint64_t cs = cnet_lfru_score(f->branches[res[0]].heat,
                                              (uint32_t)f->branches[res[0]].last_use,
                                              (uint32_t)f->use_tick);
                for (int z = 1; z < nres; z++) {
                    uint64_t sc = cnet_lfru_score(
                        f->branches[res[z]].heat,
                        (uint32_t)f->branches[res[z]].last_use,
                        (uint32_t)f->use_tick);
                    if (sc < cs) {
                        cs = sc;
                        cold_slot = z;
                    }
                }
                victim = res[cold_slot];
            }
        } else {
            int oldest = 0x7fffffff;
            for (int i = 0; i < f->num_branches; i++) {
                cce_branch* b = &f->branches[i];
                if (i == protect_idx || !b->evictable || !b->cascade || b->is_view)
                    continue;
                if (b->last_use < oldest) {
                    oldest = b->last_use;
                    victim = i;
                }
            }
        }
        if (victim < 0) return; /* nothing safely evictable */
        cce_forest_evict_branch(f, victim);
    }
}

cce_cascade* cce_forest_get_resident(cce_forest* f, const char* branch_name) {
    if (!f || !branch_name) return NULL;
    int idx = -1;
    for (int i = 0; i < f->num_branches; i++) {
        if (strcmp(f->branches[i].name, branch_name) == 0) { idx = i; break; }
    }
    if (idx < 0) return NULL;
    cce_branch* b = &f->branches[idx];
    b->last_use = ++f->use_tick;
    if (b->heat < 0xffffff00u) b->heat++;
    if (b->cascade) return b->cascade;

    if (!f->residency_provider || !b->evictable) return NULL;
    /* make room FIRST: residency never exceeds the cap, even transiently */
    if (f->hot_cap > 0) forest_make_room(f, f->hot_cap, idx);
    cce_cascade* cas = f->residency_provider(f->residency_ctx, branch_name);
    if (!cas) return NULL;
    b->cascade = cas; /* adopt the heap cascade (freed like any owned branch) */
    b->tier = CCE_TIER_HOT;
    int rn = cce_forest_resident_count(f);
    if (rn > f->resident_high_water) f->resident_high_water = rn;
    return b->cascade;
}

cce_result cce_forest_add_branch(cce_forest* f, cce_cascade* cas, const char* name) {
    if (!f || !cas || f->num_branches >= f->max_branches) return CCE_ERR_INVALID_ARG;
    if (!cas->blocks || cas->num_blocks <= 0 || cas->num_blocks > cas->max_blocks) return CCE_ERR_INVALID_ARG;
    if (f->sealed) return CCE_ERR_INVALID_ARG;  /* appending would remap the mmap -> invalidate views */

    int idx = f->num_branches;
    cce_branch* br = &f->branches[idx];

    br->cascade = (cce_cascade*)malloc(sizeof(cce_cascade));
    if (!br->cascade) return CCE_ERR_OOM;

    /* Persist first for archive-backed durability (deeper forests).
       CNET_FOREST_NO_PERSIST=1 skips the archive write: parity/oracle loads
       keep every branch HOT for the process lifetime and never restore from
       the archive — persisting a 9B FP forest would write ~37 GB of scratch
       for nothing. Same HOT-only posture the int8 oracle path already has
       (its payload-less blocks make this save refuse anyway). */
    size_t off = 0;
    char sec_name[128];
    const char* nps = getenv("CNET_FOREST_NO_PERSIST");
    snprintf(sec_name, sizeof(sec_name), "branch_%s", name ? name : "unnamed");
    if (!(nps && nps[0] == '1') &&
        cce_cascade_save_to_archive(cas, f->archive, sec_name, &off) == CCE_OK) {
        br->archive_offset = off;
        br->archive_size = 0; /* size known via dir if needed */
        br->persisted = 1;    /* offset is valid even when 0 (first section) */
    }

    *br->cascade = *cas;  /* copy for HOT (trainable RAM) */
    if (br->cascade->exact_tail_length < 0) br->cascade->exact_tail_length = f->default_exact_tail_length;
    if (br->cascade->diff_mode < 0) br->cascade->diff_mode = f->diff_mode;
    br->tier = CCE_TIER_HOT;
    br->is_view = 0;
    strncpy(br->name, name ? name : "branch", sizeof(br->name)-1);
    br->name[sizeof(br->name)-1] = '\0';

    br->num_connections = 0;
    memset(br->conn_names, 0, sizeof(br->conn_names));
    memset(br->conn_types, 0, sizeof(br->conn_types));

    /* Build centroid from input-dim of first block or zero (real tasks will refine via loop) */
    memset(br->centroid, 0, sizeof(br->centroid));
    br->centroid_dim = f->centroid_dim;

    /* inherit defaults for new branch */
    br->diff_mode = -1;
    br->exact_tail_length = -1;  /* will use f->default... when adapting */
    br->num_connections = 0;
    memset(br->conn_names, 0, sizeof(br->conn_names));
    memset(br->conn_types, 0, sizeof(br->conn_types));

    /* store in index */
    for (int d = 0; d < f->centroid_dim; ++d) {
        f->centroids[idx * f->centroid_dim + d] = br->centroid[d];
    }

    f->num_branches++;
    mark_cascade_moved(cas);
    return CCE_OK;
}

cce_result cce_forest_add_cascade_branch(cce_forest* f, cce_cascade* cas, const char* name, int* out_branch_idx) {
    if (!f || !cas || cas->num_blocks <= 0) return CCE_ERR_INVALID_ARG;

    int idx = f->num_branches;
    cce_result rc = cce_forest_add_branch(f, cas, name ? name : "cascade_branch");
    if (rc != CCE_OK) return rc;

    if (out_branch_idx) *out_branch_idx = idx;
    return CCE_OK;
}

cce_result cce_forest_add_linear_branch(cce_forest* f, const char* name,
                                        int input_dim, int hidden_dim, int output_dim,
                                        float init_scale, int* out_branch_idx) {
    if (!f || input_dim <= 0 || hidden_dim <= 0 || output_dim <= 0)
        return CCE_ERR_INVALID_ARG;

    cce_cascade cas;
    if (cce_cascade_init(&cas, 2) != CCE_OK) return CCE_ERR_OOM;

    cce_block hidden;
    memset(&hidden, 0, sizeof(hidden));
    cce_result brc = cce_block_init_linear(&hidden, input_dim, hidden_dim, init_scale);
    if (brc != CCE_OK) {
        cce_cascade_free(&cas);
        return brc;
    }
    if (cce_cascade_append(&cas, &hidden) != CCE_OK) {
        cce_block_free(&hidden);
        cce_cascade_free(&cas);
        return CCE_ERR_INVALID_ARG;
    }

    cce_block head;
    memset(&head, 0, sizeof(head));
    brc = cce_block_init_linear(&head, hidden_dim, output_dim, init_scale);
    if (brc != CCE_OK) {
        cce_cascade_free(&cas);
        return brc;
    }
    head.type = CCE_BLOCK_LINEAR_HEAD;
    if (cce_cascade_append(&cas, &head) != CCE_OK) {
        cce_block_free(&head);
        cce_cascade_free(&cas);
        return CCE_ERR_INVALID_ARG;
    }

    int idx = f->num_branches;
    cce_result rc = cce_forest_add_branch(f, &cas, name ? name : "linear_branch");
    if (rc != CCE_OK) {
        cce_cascade_free(&cas);
        return rc;
    }
    if (out_branch_idx) *out_branch_idx = idx;
    return CCE_OK;
}

cce_result cce_forest_add_patch_branch(cce_forest* f, const char* name,
                                       int patch_size, int stride, int channels,
                                       int hidden_dim, int output_dim,
                                       float init_scale, int* out_branch_idx) {
    if (!f || patch_size <= 0 || stride <= 0 || channels <= 0 || hidden_dim <= 0 || output_dim <= 0)
        return CCE_ERR_INVALID_ARG;

    const size_t max_patch_dim = 2147483647u;
    if ((size_t)patch_size > max_patch_dim / (size_t)patch_size)
        return CCE_ERR_INVALID_ARG;
    size_t patch_area = (size_t)patch_size * (size_t)patch_size;
    if ((size_t)channels > max_patch_dim / patch_area)
        return CCE_ERR_INVALID_ARG;
    size_t patch_dim_sz = patch_area * (size_t)channels;
    if (patch_dim_sz == 0)
        return CCE_ERR_INVALID_ARG;
    int patch_dim = (int)patch_dim_sz;

    cce_cascade cas;
    if (cce_cascade_init(&cas, 3) != CCE_OK) return CCE_ERR_OOM;

    cce_block patch;
    memset(&patch, 0, sizeof(patch));
    cce_result pr = cce_block_patch_init(&patch, patch_size, stride, channels);
    if (pr != CCE_OK) {
        cce_cascade_free(&cas);
        return pr;
    }
    if (cce_cascade_append(&cas, &patch) != CCE_OK) {
        cce_block_free(&patch);
        cce_cascade_free(&cas);
        return CCE_ERR_INVALID_ARG;
    }

    cce_block hidden;
    memset(&hidden, 0, sizeof(hidden));
    cce_result brc = cce_block_init_linear(&hidden, patch_dim, hidden_dim, init_scale);
    if (brc != CCE_OK) {
        cce_cascade_free(&cas);
        return brc;
    }
    if (cce_cascade_append(&cas, &hidden) != CCE_OK) {
        cce_block_free(&hidden);
        cce_cascade_free(&cas);
        return CCE_ERR_INVALID_ARG;
    }

    cce_block head;
    memset(&head, 0, sizeof(head));
    brc = cce_block_init_linear(&head, hidden_dim, output_dim, init_scale);
    if (brc != CCE_OK) {
        cce_cascade_free(&cas);
        return brc;
    }
    head.type = CCE_BLOCK_LINEAR_HEAD;
    if (cce_cascade_append(&cas, &head) != CCE_OK) {
        cce_block_free(&head);
        cce_cascade_free(&cas);
        return CCE_ERR_INVALID_ARG;
    }

    int idx = f->num_branches;
    cce_result rc = cce_forest_add_branch(f, &cas, name ? name : "patch_branch");
    if (rc != CCE_OK) {
        cce_cascade_free(&cas);
        return rc;
    }
    if (out_branch_idx) *out_branch_idx = idx;
    return CCE_OK;
}

cce_result cce_forest_recall(cce_forest* f, const float* input, int dim, int* branch_idx) {
    if (!f || !input || !branch_idx || f->num_branches == 0) return CCE_ERR_INVALID_ARG;

    /* Wire SSMax retrieval: compute scores (centroid sim + goodness) then sparse softmax */
    float* scores = (float*)malloc(f->num_branches * sizeof(float));
    if (!scores) {
        /* fallback to old centroid */
        int best = 0;
        float best_d = 1e30f;
        for (int i = 0; i < f->num_branches; ++i) {
            float d = distance(input, f->centroids + i * f->centroid_dim, f->centroid_dim);
            if (d < best_d) { best_d = d; best = i; }
        }
        *branch_idx = best;
        return CCE_OK;
    }

    for (int i = 0; i < f->num_branches; ++i) {
        cce_branch* br = &f->branches[i];
        float sim = 0.0f;
        int cdim = (br->centroid_dim < dim) ? br->centroid_dim : dim;
        for (int d = 0; d < cdim; ++d) {
            float diff = input[d] - br->centroid[d];
            sim -= diff * diff;
        }
        float goodness = (br->cascade && br->cascade->goodness > 0.0f) ? br->cascade->goodness : 0.5f;
        scores[i] = sim * 0.7f + goodness * 0.3f;
    }

    float* probs = (float*)calloc(f->num_branches, sizeof(float));
    /* Use router SSMax for retrieval scoring */
    cce_ssmax(scores, f->num_branches, 1.0f, probs, 1);  /* top-1 */

    int best = 0;
    float best_p = -1.0f;
    for (int i = 0; i < f->num_branches; ++i) {
        if (probs[i] > best_p) {
            best_p = probs[i];
            best = i;
        }
    }

    *branch_idx = best;

    free(scores);
    free(probs);
    return CCE_OK;
}

/* Materialize branch `idx`'s cascade on demand and reuse the resident pointer.
   want_writable=1 forces an OWNED (trainable, HOT) copy; want_writable=0 prefers a
   ZERO-COPY view when the forest is sealed (else an owned copy, safe even if the
   archive later grows). A read-only view is upgraded to an owned copy on demand
   (copy-on-write) when a writable cascade is requested. */
static cce_result forest_ensure_resident(cce_forest* f, int idx, int want_writable) {
    cce_branch* br = &f->branches[idx];

    if (br->cascade) {
        if (want_writable && br->is_view) {
            cce_cascade_free(br->cascade);   /* view tensors are no-ops; frees the blocks array */
            free(br->cascade);
            br->cascade = NULL;
            br->is_view = 0;
        } else {
            return CCE_OK;                   /* already resident at an adequate level (pointer reuse) */
        }
    }

    if (!br->persisted) return CCE_ERR_INVALID_ARG;  /* nothing persisted to load */

    br->cascade = (cce_cascade*)malloc(sizeof(cce_cascade));
    if (!br->cascade) return CCE_ERR_OOM;

    if (!want_writable && f->sealed) {
        /* zero-copy WARM view -- the mmap is stable because a sealed forest takes
           no more appends, so view pointers stay valid */
        if (cce_cascade_view_from_archive(br->cascade, f->archive, br->archive_offset) != CCE_OK) {
            free(br->cascade); br->cascade = NULL;
            return CCE_ERR_IO;
        }
        br->is_view = 1;
        br->tier = CCE_TIER_WARM;
    } else {
        /* owned copy: writable (HOT), or an unsealed read where views aren't safe yet */
        if (cce_cascade_load_from_archive(br->cascade, f->archive, br->archive_offset) != CCE_OK) {
            free(br->cascade); br->cascade = NULL;
            return CCE_ERR_IO;
        }
        br->is_view = 0;
        br->tier = CCE_TIER_HOT;
    }
    return CCE_OK;
}

cce_result cce_forest_get_branch_blocks(cce_forest* f, int branch_idx,
                                        int types[], int input_dims[], int output_dims[],
                                        int* num, int max_blocks) {
    if (!f || !types || !input_dims || !output_dims || !num ||
        branch_idx < 0 || branch_idx >= f->num_branches || max_blocks <= 0)
        return CCE_ERR_INVALID_ARG;

    cce_result r = forest_ensure_resident(f, branch_idx, 0 /*read-only*/);
    if (r != CCE_OK) return r;

    cce_cascade* cas = f->branches[branch_idx].cascade;
    if (!cas || cas->num_blocks < 0) return CCE_ERR_INVALID_ARG;

    int n = cas->num_blocks < max_blocks ? cas->num_blocks : max_blocks;
    for (int i = 0; i < n; i++) {
        cce_block* blk = &cas->blocks[i];
        types[i] = (int)blk->type;
        input_dims[i] = blk->weights.ndim >= 2 ? blk->weights.shape[0] : 0;
        output_dims[i] = blk->weights.ndim >= 2 ? blk->weights.shape[1] : 0;
    }
    *num = n;
    return CCE_OK;
}

cce_result cce_forest_promote_to_hot(cce_forest* f, int branch_idx) {
    if (!f || branch_idx < 0 || branch_idx >= f->num_branches) return CCE_ERR_INVALID_ARG;
    return forest_ensure_resident(f, branch_idx, 1 /*want_writable: owned HOT copy*/);
}

cce_result cce_forest_set_branch_specialist_type(cce_forest* f,
                                                 int branch_idx,
                                                 cce_specialist_type_t specialist_type) {
    if (!f || branch_idx < 0 || branch_idx >= f->num_branches) return CCE_ERR_INVALID_ARG;
    if (specialist_type != CCE_SPECIALIST_GENERAL &&
        specialist_type != CCE_SPECIALIST_NARRATIVE) {
        return CCE_ERR_INVALID_ARG;
    }
    f->branches[branch_idx].specialist_type = specialist_type;
    return CCE_OK;
}

cce_specialist_type_t cce_forest_get_branch_specialist_type(const cce_forest* f,
                                                            int branch_idx) {
    if (!f || branch_idx < 0 || branch_idx >= f->num_branches) {
        return CCE_SPECIALIST_GENERAL;
    }
    return f->branches[branch_idx].specialist_type;
}

cce_result cce_forest_seal(cce_forest* f) {
    if (!f) return CCE_ERR_INVALID_ARG;
    f->sealed = 1;   /* no more appends -> mmap stable -> zero-copy views are safe */
    return CCE_OK;
}

cce_result cce_forest_forward(cce_forest* f, int branch_idx,
                              const cce_tensor* input, cce_tensor* output) {
    if (!f || !input || !output || branch_idx < 0 || branch_idx >= f->num_branches)
        return CCE_ERR_INVALID_ARG;
    cce_result r = forest_ensure_resident(f, branch_idx, 0 /*read-only -> view if sealed*/);
    if (r != CCE_OK) return r;
    cce_branch* br = &f->branches[branch_idx];
    if (!br->cascade || br->cascade->num_blocks == 0) return CCE_ERR_INVALID_ARG;
    return cce_cascade_forward(br->cascade, input, output);
}

cce_result cce_forest_contract_branch(cce_forest* f, int branch_idx) {
    if (!f || branch_idx < 0 || branch_idx >= f->num_branches) return CCE_ERR_INVALID_ARG;

    cce_branch* br = &f->branches[branch_idx];
    if (!br->cascade) return CCE_ERR_INVALID_ARG;

    cce_cascade_freeze(br->cascade);

    /* Ensure persisted (if not already on add). Skip when already persisted so we
       never re-append (an append remaps the mmap and would invalidate live views). */
    if (!br->persisted) {
        char sec_name[128];
        snprintf(sec_name, sizeof(sec_name), "branch_%s", br->name);
        if (cce_cascade_save_to_archive(br->cascade, f->archive, sec_name, &br->archive_offset) == CCE_OK)
            br->persisted = 1;
    }

    br->tier = CCE_TIER_WARM;

    /* Free the in-RAM cascade (warm uses archive + reload/view on demand) */
    if (br->cascade) {
        cce_cascade_free(br->cascade);
        free(br->cascade);
        br->cascade = NULL;
    }
    br->is_view = 0;

    return CCE_OK;
}

cce_result cce_forest_infer(cce_forest* f, const float* input, int dim,
                            int* out_class, float* confidence) {
    if (!f || !input || !out_class) return CCE_ERR_INVALID_ARG;

    int idx = 0;
    cce_result r = cce_forest_recall(f, input, dim, &idx);
    if (r != CCE_OK) return r;

    cce_branch* br = &f->branches[idx];

    /* Materialize for read: a zero-copy view if the forest is sealed, else an
       owned copy. Reuses the resident pointer when already loaded. */
    if (br->cascade == NULL && br->persisted) {
        forest_ensure_resident(f, idx, 0 /*read-only*/);
    }

    if (br->cascade && br->cascade->num_blocks > 0) {
        /* Real forward through the (possibly loaded) cascade if we have a 1d input tensor */
        /* For now keep demo behavior but mark that persistence + load path is live */
        *out_class = idx;
        if (confidence) *confidence = br->cascade->goodness > 0 ? br->cascade->goodness : 0.8f;
    } else {
        /* fallback demo */
        *out_class = idx % 10;
        if (confidence) *confidence = 0.85f;
    }

    return CCE_OK;
}

/* === Merging & Connections impl === */

cce_result cce_forest_connect(cce_forest* f, int from_idx, int to_idx, int conn_type) {
    if (!f || from_idx < 0 || from_idx >= f->num_branches ||
        to_idx < 0 || to_idx >= f->num_branches || f->num_branches == 0)
        return CCE_ERR_INVALID_ARG;

    cce_branch* br = &f->branches[from_idx];
    if (br->num_connections >= 8) return CCE_ERR_INVALID_ARG;

    /* store by name for merge safety */
    memcpy(br->conn_names[br->num_connections], f->branches[to_idx].name,
           sizeof(br->conn_names[br->num_connections]));
    br->conn_names[br->num_connections][63] = '\0';
    br->conn_types[br->num_connections] = conn_type;
    br->num_connections++;
    return CCE_OK;
}

cce_result cce_forest_get_connections(cce_forest* f, int branch_idx,
                                      char names[][64], int types[], int* num, int max_conn) {
    if (!f || branch_idx < 0 || branch_idx >= f->num_branches || !num) return CCE_ERR_INVALID_ARG;
    cce_branch* br = &f->branches[branch_idx];
    int n = br->num_connections < max_conn ? br->num_connections : max_conn;
    for (int i=0; i<n; i++) {
        if (names) {
            strncpy(names[i], br->conn_names[i], 63);
            names[i][63] = '\0';
        }
        if (types) types[i] = br->conn_types[i];
    }
    *num = n;
    return CCE_OK;
}

static cce_result clone_block_owned(cce_block* dst, const cce_block* src) {
    if (!dst || !src || src->weights.ndim != 2 || src->weights.shape[0] <= 0 || src->weights.shape[1] <= 0 ||
        !src->weights.data)
        return CCE_ERR_INVALID_ARG;

    int in_dim = src->weights.shape[0];
    int out_dim = src->weights.shape[1];
    memset(dst, 0, sizeof(*dst));

    if (src->type == CCE_BLOCK_LINEAR || src->type == CCE_BLOCK_LINEAR_HEAD) {
        if (src->bias.ndim != 1 || src->bias.shape[0] != out_dim || !src->bias.data)
            return CCE_ERR_INVALID_ARG;
        cce_result rc = cce_block_init_linear(dst, in_dim, out_dim, 0.0f);
        if (rc != CCE_OK) return rc;
        dst->type = src->type;
        if (dst->weights.numel != src->weights.numel || dst->bias.numel != src->bias.numel) {
            cce_block_free(dst);
            return CCE_ERR_INVALID_ARG;
        }
        if (src->weights.data)
            memcpy(dst->weights.data, src->weights.data, dst->weights.numel * sizeof(float));
        if (src->bias.data)
            memcpy(dst->bias.data, src->bias.data, dst->bias.numel * sizeof(float));
        if (src->momentum_weights.data && dst->momentum_weights.numel == src->momentum_weights.numel)
            memcpy(dst->momentum_weights.data, src->momentum_weights.data, dst->momentum_weights.numel * sizeof(float));
        if (src->momentum_bias.data && dst->momentum_bias.numel == src->momentum_bias.numel)
            memcpy(dst->momentum_bias.data, src->momentum_bias.data, dst->momentum_bias.numel * sizeof(float));
        if (src->second_moment_w.data && dst->second_moment_w.numel == src->second_moment_w.numel)
            memcpy(dst->second_moment_w.data, src->second_moment_w.data, dst->second_moment_w.numel * sizeof(float));
        if (src->second_moment_b.data && dst->second_moment_b.numel == src->second_moment_b.numel)
            memcpy(dst->second_moment_b.data, src->second_moment_b.data, dst->second_moment_b.numel * sizeof(float));
    } else if (src->type == CCE_BLOCK_PATCH) {
        if (in_dim != out_dim) return CCE_ERR_INVALID_ARG;
        int wshape[2] = { in_dim, out_dim };
        cce_result rc = cce_tensor_alloc(&dst->weights, wshape, 2);
        if (rc != CCE_OK) return rc;
        rc = cce_tensor_alloc(&dst->momentum_weights, wshape, 2);
        if (rc != CCE_OK) {
            cce_tensor_free(&dst->weights);
            return rc;
        }
        rc = cce_tensor_alloc(&dst->second_moment_w, wshape, 2);
        if (rc != CCE_OK) {
            cce_tensor_free(&dst->momentum_weights);
            cce_tensor_free(&dst->weights);
            return rc;
        }
        if (dst->weights.numel != src->weights.numel) {
            cce_block_free(dst);
            return CCE_ERR_INVALID_ARG;
        }
        memcpy(dst->weights.data, src->weights.data, dst->weights.numel * sizeof(float));
        cce_tensor_zero(&dst->momentum_weights);
        cce_tensor_zero(&dst->second_moment_w);
        if (src->momentum_weights.data && dst->momentum_weights.numel == src->momentum_weights.numel)
            memcpy(dst->momentum_weights.data, src->momentum_weights.data, dst->momentum_weights.numel * sizeof(float));
        if (src->second_moment_w.data && dst->second_moment_w.numel == src->second_moment_w.numel)
            memcpy(dst->second_moment_w.data, src->second_moment_w.data, dst->second_moment_w.numel * sizeof(float));
        dst->type = CCE_BLOCK_PATCH;
        dst->flags = CCE_FLAG_PATCH;
    } else {
        return CCE_ERR_UNSUPPORTED;
    }

    dst->flags |= src->flags;
    if (dst->type == CCE_BLOCK_PATCH) dst->flags |= CCE_FLAG_PATCH;
    dst->goodness = src->goodness;
    dst->freeze_countdown = src->freeze_countdown;
    dst->timestep = src->timestep;
    return CCE_OK;
}

static cce_result clone_cascade_owned(cce_cascade* dst, const cce_cascade* src) {
    if (!dst || !src || src->num_blocks <= 0) return CCE_ERR_INVALID_ARG;
    cce_result rc = cce_cascade_init(dst, src->num_blocks);
    if (rc != CCE_OK) return rc;

    dst->flags = src->flags;
    dst->goodness = src->goodness;
    dst->freeze_countdown = src->freeze_countdown;
    dst->exact_tail_length = src->exact_tail_length;
    dst->diff_mode = src->diff_mode;
    strncpy(dst->name, src->name, sizeof(dst->name) - 1);
    dst->name[sizeof(dst->name) - 1] = '\0';

    for (int i = 0; i < src->num_blocks; i++) {
        cce_block clone;
        rc = clone_block_owned(&clone, &src->blocks[i]);
        if (rc != CCE_OK) {
            cce_cascade_free(dst);
            return rc;
        }
        rc = cce_cascade_append(dst, &clone);
        if (rc != CCE_OK) {
            cce_block_free(&clone);
            cce_cascade_free(dst);
            return rc;
        }
    }
    return CCE_OK;
}

cce_result cce_forest_merge(cce_forest* dst, cce_forest* src, const char* name_prefix) {
    if (!dst || !src) return CCE_ERR_INVALID_ARG;
    int merged = 0;
    char pfx[32] = "";
    if (name_prefix) {
        strncpy(pfx, name_prefix, 31);
        pfx[31] = '\0';
    }

    for (int i=0; i < src->num_branches; i++) {
        cce_branch* sbr = &src->branches[i];
        if (!sbr->cascade) continue;

        /* create a copy cascade to add */
        cce_cascade* copy = (cce_cascade*)malloc(sizeof(cce_cascade));
        if (!copy) continue;
        if (clone_cascade_owned(copy, sbr->cascade) != CCE_OK) {
            free(copy);
            continue;
        }

        char newname[64];
        snprintf(newname, sizeof(newname), "%s%s", pfx, sbr->name);

        cce_result add_rc = cce_forest_add_branch(dst, copy, newname);
        if (add_rc == CCE_OK) {
            /* copy connections by name (will resolve by name on use) */
            int new_idx = dst->num_branches - 1;
            cce_branch* dbr = &dst->branches[new_idx];
            dbr->num_connections = sbr->num_connections;
            for (int c=0; c < sbr->num_connections; c++) {
                char conn[64];
                snprintf(conn, sizeof(conn), "%s%s", pfx, sbr->conn_names[c]);
                memcpy(dbr->conn_names[c], conn,
                       sizeof(dbr->conn_names[c]));
                dbr->conn_names[c][63] = '\0';
                dbr->conn_types[c] = sbr->conn_types[c];
            }
            merged++;
        }
        if (add_rc != CCE_OK) {
            cce_cascade_free(copy);
        }
        free(copy);
    }
    return merged;
}

cce_result cce_forest_set_diff_mode(cce_forest* f, cce_diff_mode_t mode) {
    if (!f) return CCE_ERR_INVALID_ARG;
    f->diff_mode = mode;
    return CCE_OK;
}

void cce_forest_set_default_exact_tail_length(cce_forest* f, int length) {
    if (f) f->default_exact_tail_length = length;
}

cce_result cce_forest_set_branch_diff_mode(cce_forest* f, int branch_idx, cce_diff_mode_t mode) {
    if (!f || branch_idx < 0 || branch_idx >= f->num_branches) return CCE_ERR_INVALID_ARG;
    f->branches[branch_idx].diff_mode = mode;
    return CCE_OK;
}

cce_result cce_forest_set_branch_exact_tail_length(cce_forest* f, int branch_idx, int length) {
    if (!f || branch_idx < 0 || branch_idx >= f->num_branches) return CCE_ERR_INVALID_ARG;
    f->branches[branch_idx].exact_tail_length = length;
    return CCE_OK;
}

int cce_forest_branch_count(const cce_forest* f) {
    return f ? f->num_branches : 0;
}

cce_result cce_forest_branch_name(const cce_forest* f, int idx, char* buf, int len) {
    if (!f || !buf || len <= 0 || idx < 0 || idx >= f->num_branches) return CCE_ERR_INVALID_ARG;
    strncpy(buf, f->branches[idx].name, (size_t)len - 1);
    buf[len - 1] = '\0';
    return CCE_OK;
}

/* Basic weight import for linear branches created via add_linear_branch.
   Sets the final head block (common case for importing a pre-trained classification/regression head).
   weights expected as [out_dim * in_dim] row-major, bias [out_dim]. */
cce_result cce_forest_set_branch_linear_weights(cce_forest* f, int branch_idx,
                                                 const float* weights, int w_in_dim, int w_out_dim,
                                                 const float* bias, int b_dim) {
    if (!f || branch_idx < 0 || branch_idx >= f->num_branches || !weights || !bias)
        return CCE_ERR_INVALID_ARG;

    cce_cascade* cas = f->branches[branch_idx].cascade;
    if (!cas || cas->num_blocks == 0) return CCE_ERR_INVALID_ARG;

    // Target the last block (the head in a typical linear_branch)
    cce_block* blk = &cas->blocks[cas->num_blocks - 1];
    if (blk->weights.numel == 0) return CCE_ERR_INVALID_ARG;

    size_t expected_w = blk->weights.numel;
    if ((size_t)w_out_dim * (size_t)w_in_dim != expected_w || (size_t)b_dim != blk->bias.numel)
        return CCE_ERR_INVALID_ARG;

    memcpy(blk->weights.data, weights, expected_w * sizeof(float));
    memcpy(blk->bias.data, bias, (size_t)b_dim * sizeof(float));

    // Clear momentum since these are imported (fresh)
    if (blk->momentum_weights.data) cce_tensor_zero(&blk->momentum_weights);
    if (blk->momentum_bias.data) cce_tensor_zero(&blk->momentum_bias);

    return CCE_OK;
}
