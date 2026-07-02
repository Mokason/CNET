#include "cce_model_io.h"
#include "cce_model_internal.h"
#include "../../include/cce/cce_archive.h"
#include "../../include/cce/cce_cascade.h"
#include "../../include/cce/cce_forest.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ---- growable byte buffer ---- */
typedef struct { unsigned char* p; size_t len, cap; } buf_t;
static int buf_reserve(buf_t* b, size_t extra) {
    if (b->len + extra <= b->cap) return 1;
    size_t nc = b->cap ? b->cap * 2 : 4096;
    while (nc < b->len + extra) nc *= 2;
    unsigned char* np = (unsigned char*)realloc(b->p, nc);
    if (!np) return 0;
    b->p = np; b->cap = nc; return 1;
}
static int buf_put(buf_t* b, const void* d, size_t n) {
    if (!buf_reserve(b, n)) return 0;
    memcpy(b->p + b->len, d, n); b->len += n; return 1;
}
static int put_i32(buf_t* b, int32_t v)  { return buf_put(b, &v, 4); }
static int put_f32(buf_t* b, float v)    { return buf_put(b, &v, 4); }
static int put_u64(buf_t* b, uint64_t v) { return buf_put(b, &v, 8); }
static int put_fixed(buf_t* b, const char* s, size_t n) {
    char tmp[512]; if (n > sizeof(tmp)) return 0;
    memset(tmp, 0, n); if (s) strncpy(tmp, s, n - 1);
    return buf_put(b, tmp, n);
}

cce_result cce_model_io_save(cce_model* m, const char* path) {
    if (!m || !path) return CCE_ERR_INVALID_ARG;

    remove(path);                          /* OPEN_ALWAYS appends; start fresh */
    cce_archive* arc = NULL;
    if (cce_archive_open(&arc, path) != CCE_OK) return CCE_ERR_IO;

    /* Pass 1: persist each branch cascade into the bundle, remember offsets.
       Offsets live in a flat array sized to the total branch count, so branches
       per forest are not capped by a fixed stack array (forests are capped at 16
       by the model itself, but a forest may hold many branches). Pass 2 walks the
       forests/branches in the identical order and consumes offsets via the same
       running counter. */
    size_t total_branches = 0;
    for (int fi = 0; fi < m->num_forests; ++fi)
        total_branches += (size_t)m->forests[fi]->num_branches;
    size_t* offs = total_branches ? (size_t*)malloc(total_branches * sizeof(size_t)) : NULL;
    if (total_branches && !offs) { cce_archive_close(arc); return CCE_ERR_OOM; }
    size_t k = 0;
    for (int fi = 0; fi < m->num_forests; ++fi) {
        cce_forest* f = m->forests[fi];
        for (int bi = 0; bi < f->num_branches; ++bi) {
            if (cce_forest_promote_to_hot(f, bi) != CCE_OK || !f->branches[bi].cascade) {
                free(offs); cce_archive_close(arc); return CCE_ERR_IO;
            }
            char sec[64]; snprintf(sec, sizeof(sec), "f%d_b%d", fi, bi);
            size_t off = 0;
            if (cce_cascade_save_to_archive(f->branches[bi].cascade, arc, sec, &off) != CCE_OK) {
                free(offs); cce_archive_close(arc); return CCE_ERR_IO;
            }
            offs[k++] = off;
        }
    }

    /* Pass 2: build manifest. */
    buf_t b = {0};
    buf_put(&b, "CMDL", 4);
    put_i32(&b, 1);                        /* version */
    put_fixed(&b, m->name, 64);
    put_i32(&b, (int)m->diff_mode);
    put_i32(&b, m->classify);
    put_f32(&b, m->goodness_threshold);
    put_f32(&b, m->dfa_strength);
    put_f32(&b, m->grad_clip);
    int has_sched = m->scheduler ? 1 : 0;
    put_i32(&b, has_sched);
    if (has_sched) {
        cce_scheduler* s = m->scheduler;
        put_i32(&b, (int)s->type);     put_f32(&b, s->initial_lr);
        put_i32(&b, s->warmup_epochs); put_f32(&b, s->decay_factor);
        put_i32(&b, s->step_size);     put_f32(&b, s->plateau_factor);
        put_i32(&b, s->plateau_patience);
        put_f32(&b, s->current_lr);    put_f32(&b, s->best_loss);
        put_i32(&b, s->patience_counter); put_i32(&b, s->total_steps);
    }
    put_i32(&b, m->num_forests);

    k = 0;
    for (int fi = 0; fi < m->num_forests; ++fi) {
        cce_forest* f = m->forests[fi];
        put_fixed(&b, m->forest_names[fi], 64);
        put_i32(&b, f->num_branches);
        put_i32(&b, f->centroid_dim);
        put_i32(&b, f->sealed);
        put_i32(&b, (int)f->diff_mode);
        put_i32(&b, f->default_exact_tail_length);
        for (int bi = 0; bi < f->num_branches; ++bi) {
            cce_branch* br = &f->branches[bi];
            put_fixed(&b, br->name, 64);
            buf_put(&b, br->centroid, sizeof(br->centroid));   /* float[32] */
            put_i32(&b, br->centroid_dim);
            put_i32(&b, (int)br->tier);
            put_i32(&b, (int)br->diff_mode);
            put_i32(&b, br->exact_tail_length);
            put_i32(&b, br->num_connections);
            buf_put(&b, br->conn_names, sizeof(br->conn_names)); /* char[8][64] */
            buf_put(&b, br->conn_types, sizeof(br->conn_types)); /* int[8] */
            put_u64(&b, (uint64_t)offs[k++]);
        }
    }

    size_t moff = 0;
    cce_result rc = cce_archive_append_section(arc, "MODEL_MANIFEST", b.p, b.len, &moff);
    free(b.p);
    free(offs);
    cce_archive_close(arc);                /* flushes the directory */
    return rc == CCE_OK ? CCE_OK : CCE_ERR_IO;
}

/* ---- reader over a fixed buffer ---- */
typedef struct { const unsigned char* p; size_t len, pos; } rdr_t;
static int rd(rdr_t* r, void* out, size_t n) {
    if (r->pos + n > r->len) return 0;
    memcpy(out, r->p + r->pos, n); r->pos += n; return 1;
}
static int rd_i32(rdr_t* r, int32_t* v) { return rd(r, v, 4); }
static int rd_f32(rdr_t* r, float* v)   { return rd(r, v, 4); }
static int rd_u64(rdr_t* r, uint64_t* v){ return rd(r, v, 8); }

cce_result cce_model_io_load(cce_model* m, const char* path) {
    if (!m || !path) return CCE_ERR_INVALID_ARG;

    cce_archive* arc = NULL;
    if (cce_archive_open(&arc, path) != CCE_OK) return CCE_ERR_IO;

    size_t moff = 0, msize = 0;
    if (cce_archive_find_section(arc, "MODEL_MANIFEST", &moff, &msize) != CCE_OK || msize == 0) {
        cce_archive_close(arc); return CCE_ERR_NOT_FOUND;
    }
    unsigned char* buf = (unsigned char*)malloc(msize);
    if (!buf) { cce_archive_close(arc); return CCE_ERR_OOM; }
    if (cce_archive_read_raw(arc, moff, buf, msize) != CCE_OK) {
        free(buf); cce_archive_close(arc); return CCE_ERR_IO;
    }

    rdr_t r = { buf, msize, 0 };
    char magic[4]; int32_t version = 0;
    if (!rd(&r, magic, 4) || memcmp(magic, "CMDL", 4) != 0 ||
        !rd_i32(&r, &version) || version != 1) {
        free(buf); cce_archive_close(arc); return CCE_ERR_UNSUPPORTED;
    }

    char name[64]; rd(&r, name, 64);
    strncpy(m->name, name, sizeof(m->name) - 1);
    int32_t dm = 0; rd_i32(&r, &dm); m->diff_mode = (cce_diff_mode_t)dm;
    rd_i32(&r, &m->classify);
    rd_f32(&r, &m->goodness_threshold);
    rd_f32(&r, &m->dfa_strength);
    rd_f32(&r, &m->grad_clip);

    int32_t has_sched = 0; rd_i32(&r, &has_sched);
    if (has_sched) {
        cce_scheduler* s = (cce_scheduler*)calloc(1, sizeof(cce_scheduler));
        if (!s) { free(buf); cce_archive_close(arc); return CCE_ERR_OOM; }
        int32_t t;
        rd_i32(&r, &t); s->type = (cce_sched_type_t)t; rd_f32(&r, &s->initial_lr);
        rd_i32(&r, &s->warmup_epochs); rd_f32(&r, &s->decay_factor);
        rd_i32(&r, &s->step_size);     rd_f32(&r, &s->plateau_factor);
        rd_i32(&r, &s->plateau_patience);
        rd_f32(&r, &s->current_lr);    rd_f32(&r, &s->best_loss);
        rd_i32(&r, &s->patience_counter); rd_i32(&r, &s->total_steps);
        m->scheduler = s; m->owns_scheduler = 1;
    }

    int32_t num_forests = 0; rd_i32(&r, &num_forests);
    if (num_forests < 0 || num_forests > 16) {
        free(buf); cce_archive_close(arc); return CCE_ERR_UNSUPPORTED;
    }
    m->num_forests = 0;
    /* Own everything we adopt below: if a later forest/branch fails to load, the
       caller's cce_model_destroy frees the forests already added (no leak). */
    m->owns_forests = 1;

    for (int fi = 0; fi < num_forests; ++fi) {
        char fname[64]; rd(&r, fname, 64);
        int32_t nb, cdim, sealed, fdm, etl;
        rd_i32(&r, &nb); rd_i32(&r, &cdim); rd_i32(&r, &sealed);
        rd_i32(&r, &fdm); rd_i32(&r, &etl);

        /* Bound untrusted manifest values before using them as allocation sizes,
           loop bounds, or array indices. Branch centroids are a fixed float[32],
           so centroid_dim must be in [0,32]; 65536 is a generous branch ceiling. */
        if (nb < 0 || nb > 65536 || cdim < 0 || cdim > 32) {
            free(buf); cce_archive_close(arc); return CCE_ERR_UNSUPPORTED;
        }

        int maxb = nb > 0 ? nb : 1;
        cce_forest* f = (cce_forest*)calloc(1, sizeof(cce_forest));
        if (!f) { free(buf); cce_archive_close(arc); return CCE_ERR_OOM; }
        f->archive = NULL;                 /* HOT-only, archive-less */
        f->max_branches = maxb;
        f->branches = (cce_branch*)calloc(maxb, sizeof(cce_branch));
        f->centroid_dim = cdim;
        f->centroids = (float*)calloc((size_t)maxb * (cdim > 0 ? cdim : 1), sizeof(float));
        if (!f->branches || !f->centroids) {
            free(f->branches); free(f->centroids); free(f);
            free(buf); cce_archive_close(arc); return CCE_ERR_OOM;
        }
        f->num_branches = 0;
        f->sealed = 0;
        f->diff_mode = (cce_diff_mode_t)fdm;
        f->default_exact_tail_length = etl;

        for (int bi = 0; bi < nb; ++bi) {
            cce_branch* br = &f->branches[bi];
            char bname[64]; rd(&r, bname, 64);
            rd(&r, br->centroid, sizeof(br->centroid));
            int32_t bcdim, tier, bdm, betl, nconn;
            rd_i32(&r, &bcdim); rd_i32(&r, &tier); rd_i32(&r, &bdm);
            rd_i32(&r, &betl);  rd_i32(&r, &nconn);
            rd(&r, br->conn_names, sizeof(br->conn_names));
            rd(&r, br->conn_types, sizeof(br->conn_types));
            uint64_t coff = 0; rd_u64(&r, &coff);

            br->cascade = (cce_cascade*)malloc(sizeof(cce_cascade));
            memset(br->cascade, 0, sizeof(cce_cascade));
            if (cce_cascade_load_from_archive(br->cascade, arc, (size_t)coff) != CCE_OK) {
                free(br->cascade); br->cascade = NULL;
                cce_forest_close(f); free(buf); cce_archive_close(arc);
                return CCE_ERR_IO;
            }
            strncpy(br->name, bname, sizeof(br->name) - 1);
            br->centroid_dim = bcdim;
            br->tier = CCE_TIER_HOT;
            br->is_view = 0;
            br->persisted = 0;
            br->archive_offset = 0;
            br->diff_mode = (cce_diff_mode_t)bdm;
            br->exact_tail_length = betl;
            br->num_connections = nconn;
            for (int d = 0; d < cdim && d < 32; ++d)
                f->centroids[(size_t)bi * cdim + d] = br->centroid[d];
            f->num_branches++;
        }

        cce_model_add_forest(m, f, fname);
    }

    m->owns_forests = 1;                    /* loaded model owns its forests */
    free(buf);
    cce_archive_close(arc);                 /* all data copied into HOT RAM */
    return CCE_OK;
}
