#include "vd_pack.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *vd_pack_strerror(int code) {
    switch (code) {
        case VD_PACK_OK:          return "ok";
        case VD_PACK_E_OPEN:      return "cannot open";
        case VD_PACK_E_MAGIC:     return "bad magic";
        case VD_PACK_E_HEADER:    return "bad header";
        case VD_PACK_E_COUNT:     return "count out of range";
        case VD_PACK_E_TRUNC:     return "truncated";
        case VD_PACK_E_AGGREGATE: return "aggregate mismatch";
        case VD_PACK_E_TRAILING:  return "trailing data";
        case VD_PACK_E_FIELD:     return "invalid field";
        case VD_PACK_E_ALLOC:     return "allocation failed";
        default:                  return "unknown";
    }
}

void vd_pack_free(VdPack *p) {
    size_t i;
    if (!p) return;
    if (p->imgs) {
        for (i = 0; i < p->n; i++) {
            free(p->imgs[i].gts); free(p->imgs[i].gt_dif);
            free(p->imgs[i].pb); free(p->imgs[i].plabel); free(p->imgs[i].pf);
        }
        free(p->imgs);
    }
    memset(p, 0, sizeof *p);
}

static int rd(FILE *f, void *dst, size_t n) {
    return fread(dst, 1, n, f) == n ? 0 : -1;
}

/* A box stored in a pack is an image-space rectangle: strictly positive extent,
   and corners inside a generous coordinate bound so no downstream arithmetic
   has to reason about extremes. */
static int box_ok(const int32_t v[4]) {
    int64_t x = v[0], y = v[1], w = v[2], h = v[3];
    if (w <= 0 || h <= 0) return -1;
    if (w > VD_MAX_COORD || h > VD_MAX_COORD) return -1;
    if (x < -VD_MAX_COORD || x > VD_MAX_COORD) return -1;
    if (y < -VD_MAX_COORD || y > VD_MAX_COORD) return -1;
    if (x + w > (int64_t)2 * VD_MAX_COORD) return -1;
    if (y + h > (int64_t)2 * VD_MAX_COORD) return -1;
    return 0;
}

int vd_pack_load(const char *path, VdPack *p) {
    FILE *f;
    char magic[8];
    int32_t dim, n_img;
    int64_t declared_prop;
    uint64_t total = 0;
    size_t i;
    int rc = VD_PACK_OK;
    unsigned char extra;

    if (!p) return VD_PACK_E_OPEN;
    memset(p, 0, sizeof *p);
    if (!path) return VD_PACK_E_OPEN;
    f = fopen(path, "rb");
    if (!f) return VD_PACK_E_OPEN;

    if (rd(f, magic, 8) != 0 || memcmp(magic, "VDPACK1", 7) != 0) {
        fclose(f); return VD_PACK_E_MAGIC;
    }
    if (rd(f, &dim, 4) != 0 || rd(f, &n_img, 4) != 0 || rd(f, &declared_prop, 8) != 0) {
        fclose(f); return VD_PACK_E_HEADER;
    }
    /* Bound every count BEFORE it is used to allocate or index. */
    if (dim <= 0 || dim > VD_MAX_DIM) { fclose(f); return VD_PACK_E_COUNT; }
    if (n_img < 0 || n_img > VD_MAX_IMAGES) { fclose(f); return VD_PACK_E_COUNT; }
    if (declared_prop < 0 || declared_prop > (int64_t)VD_MAX_TOTAL_PROPS) {
        fclose(f); return VD_PACK_E_COUNT;
    }

    p->dim = dim;
    p->n = (size_t)n_img;
    if (p->n) {
        p->imgs = (VdPackImg *)calloc(p->n, sizeof(VdPackImg));
        if (!p->imgs) {
            /* p->n is already set; leaving it would hand the caller a pack that
               claims images it does not have. */
            memset(p, 0, sizeof *p);
            fclose(f);
            return VD_PACK_E_ALLOC;
        }
    }

    for (i = 0; i < p->n; i++) {
        VdPackImg *im = &p->imgs[i];
        int32_t idlen, np, ng, j;

        if (rd(f, &idlen, 4) != 0) { rc = VD_PACK_E_TRUNC; goto fail; }
        if (idlen <= 0 || idlen > VD_MAX_ID_LEN) { rc = VD_PACK_E_FIELD; goto fail; }
        if (rd(f, im->id, (size_t)idlen) != 0) { rc = VD_PACK_E_TRUNC; goto fail; }
        im->id[idlen] = 0;
        if (strlen(im->id) != (size_t)idlen) { rc = VD_PACK_E_FIELD; goto fail; }

        if (rd(f, &np, 4) != 0 || rd(f, &ng, 4) != 0) { rc = VD_PACK_E_TRUNC; goto fail; }
        if (np < 0 || np > VD_MAX_PROPS_PER_IMAGE) { rc = VD_PACK_E_COUNT; goto fail; }
        if (ng < 0 || ng > VD_MAX_GT_PER_IMAGE) { rc = VD_PACK_E_COUNT; goto fail; }

        total += (uint64_t)np;
        if (total > (uint64_t)VD_MAX_TOTAL_PROPS) { rc = VD_PACK_E_COUNT; goto fail; }

        im->n_gt = (size_t)ng;
        im->n_prop = (size_t)np;
        if (ng) {
            im->gts = (VdBox *)calloc((size_t)ng, sizeof(VdBox));
            im->gt_dif = (int *)calloc((size_t)ng, sizeof(int));
            if (!im->gts || !im->gt_dif) { rc = VD_PACK_E_ALLOC; goto fail; }
        }
        for (j = 0; j < ng; j++) {
            int32_t v[5];
            if (rd(f, v, sizeof v) != 0) { rc = VD_PACK_E_TRUNC; goto fail; }
            if (box_ok(v) != 0) { rc = VD_PACK_E_FIELD; goto fail; }
            if (v[4] != 0 && v[4] != 1) { rc = VD_PACK_E_FIELD; goto fail; }
            im->gts[j].x = v[0]; im->gts[j].y = v[1];
            im->gts[j].w = v[2]; im->gts[j].h = v[3];
            im->gt_dif[j] = v[4];
        }
        if (np) {
            im->pb = (VdBox *)calloc((size_t)np, sizeof(VdBox));
            im->plabel = (int *)calloc((size_t)np, sizeof(int));
            /* np <= 300 and dim <= 256, so this product cannot overflow. */
            im->pf = (float *)calloc((size_t)np * (size_t)dim, sizeof(float));
            if (!im->pb || !im->plabel || !im->pf) { rc = VD_PACK_E_ALLOC; goto fail; }
        }
        for (j = 0; j < np; j++) {
            int32_t v[5], k;
            float *row = im->pf + (size_t)j * (size_t)dim;
            if (rd(f, v, sizeof v) != 0) { rc = VD_PACK_E_TRUNC; goto fail; }
            if (box_ok(v) != 0) { rc = VD_PACK_E_FIELD; goto fail; }
            if (v[4] != -1 && v[4] != 0 && v[4] != 1) { rc = VD_PACK_E_FIELD; goto fail; }
            im->pb[j].x = v[0]; im->pb[j].y = v[1];
            im->pb[j].w = v[2]; im->pb[j].h = v[3];
            im->plabel[j] = v[4];
            if (rd(f, row, sizeof(float) * (size_t)dim) != 0) { rc = VD_PACK_E_TRUNC; goto fail; }
            /* A NaN feature poisons every score it touches, silently. */
            for (k = 0; k < dim; k++)
                if (!isfinite(row[k])) { rc = VD_PACK_E_FIELD; goto fail; }
        }
    }

    /* The header's aggregate must describe what was actually read... */
    if ((uint64_t)declared_prop != total) { rc = VD_PACK_E_AGGREGATE; goto fail; }
    /* ...and the file must end exactly where the structure says it does. */
    if (fread(&extra, 1, 1, f) != 0) { rc = VD_PACK_E_TRAILING; goto fail; }
    if (ferror(f)) { rc = VD_PACK_E_TRUNC; goto fail; }

    p->total_prop = (size_t)total;
    fclose(f);
    return VD_PACK_OK;

fail:
    fclose(f);
    vd_pack_free(p);   /* transactional: no partial pack ever escapes */
    return rc;
}

void vd_pack_free_ids(char **ids, size_t n) {
    size_t i;
    if (!ids) return;
    for (i = 0; i < n; i++) free(ids[i]);
    free(ids);
}

int vd_pack_read_ids(const char *path, char ***ids_out, size_t *n_out) {
    VdPack p;
    char **ids;
    size_t i;
    int rc;
    if (!ids_out || !n_out) return VD_PACK_E_OPEN;
    *ids_out = NULL; *n_out = 0;
    /* Deliberately the same full-structure validation as the loader: an ID list
       must never be obtainable from a file the loader would refuse. */
    rc = vd_pack_load(path, &p);
    if (rc != VD_PACK_OK) return rc;
    ids = (char **)calloc(p.n ? p.n : 1, sizeof(char *));
    if (!ids) { vd_pack_free(&p); return VD_PACK_E_ALLOC; }
    for (i = 0; i < p.n; i++) {
        size_t l = strlen(p.imgs[i].id);
        ids[i] = (char *)malloc(l + 1);
        if (!ids[i]) { vd_pack_free_ids(ids, i); vd_pack_free(&p); return VD_PACK_E_ALLOC; }
        memcpy(ids[i], p.imgs[i].id, l + 1);
    }
    *ids_out = ids;
    *n_out = p.n;
    vd_pack_free(&p);
    return VD_PACK_OK;
}
