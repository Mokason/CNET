#include "vd_protocol.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vd_io.h"
#include "vd_sha256.h"
#include "vd_roots.h"

/* ------------------------------------------------------------------------
   Pre-registered protocols. These constants ARE the identity of a scored run.
   The roots below were computed once from the official VOC2007 archives
   (MD5 c52e279531787c972589f7e41ab4ae64 / b6e924de25625d8de591ea690078ad9f)
   under the frozen selection rule in
   plans/cnet_vision_object_detection_v2_20260727.md, and pinned here. A cache
   that does not reproduce them is not this protocol's cache.
   ------------------------------------------------------------------------ */
const char *const VD_MEMBERS[VD_N_MEMBERS] = {
    "train.pack", "val.pack", "test.pack", "pca.bin",
    "ids_train.txt", "ids_val.txt", "ids_test.txt",
    "content_train.txt", "content_val.txt", "content_test.txt"
};

int vd_artifact_root(const VdMember *m, size_t n, long schema, char *hex_out) {
    VdSha256 c;
    char line[512];
    size_t i;
    if (!m || !hex_out || n != VD_N_MEMBERS) return -1;
    vd_sha256_init(&c);
    vd_sha256_update(&c, "VDCACHEROOT1\n", 13);
    snprintf(line, sizeof line, "schema %ld\n", schema);
    vd_sha256_update(&c, line, strlen(line));
    snprintf(line, sizeof line, "members %zu\n", n);
    vd_sha256_update(&c, line, strlen(line));
    for (i = 0; i < n; i++) {
        /* names come from VD_MEMBERS, so ordering is fixed by the protocol and
           not by whatever the cache happens to contain */
        if (!m[i].name || strcmp(m[i].name, VD_MEMBERS[i]) != 0) return -1;
        if (m[i].size < 0 || strlen(m[i].sha) != 64) return -1;
        snprintf(line, sizeof line, "%s %lld %s\n", m[i].name, m[i].size, m[i].sha);
        vd_sha256_update(&c, line, strlen(line));
    }
    vd_sha256_hex(&c, hex_out);
    return 0;
}

static const VdProtocol PROTOCOLS[] = {
    {
        "v2",
        /* manifest_version */ 1,
        /* dataset */ "PASCAL_VOC_2007", /* cls */ "car",
        /* split_key */ "sha256_content_hash_trainval",
        /* seed */ 20260727,
        /* hog_side */ 64, /* color */ 1, /* hog_dim */ 1764, /* pca_dim */ 256,
        /* ss_width */ 300, /* max_prop */ 300, /* min_side */ 16,
        /* nms_iou_x100 */ 30, /* match_iou_x100 */ 50,
        /* test_offset */ 1000, /* test_count */ 1000,
        /* train_img */ 4042, /* val_img */ 969,
        /* pca_fit_images */ 600, /* pca_fit_rows */ 162983,
        /* requires_prev */ 1, /* prev_test_count */ 1000,
        /* prev_pack_sha */ "cdfa0b82c5f24498e16f56362b63336680e622cfc1dd5c15277ca7a6ca5eafc6",
        /* id_root_train */ VD_ROOT_ID_TRAIN,
        /* id_root_val */ VD_ROOT_ID_VAL,
        /* id_root_test */ VD_ROOT_ID_TEST,
        /* content_root_train */ VD_ROOT_CONTENT_TRAIN,
        /* content_root_val */ VD_ROOT_CONTENT_VAL,
        /* content_root_test */ VD_ROOT_CONTENT_TEST,
        /* artifact_root */ VD_ARTIFACT_ROOT_V2
    },
    /* Test-only protocol for the evidence-gate negative controls. It is pinned
       to the synthetic fixtures produced by vd_mkcache and declares
       dataset SYNTHETIC_TEST, so it is mutually exclusive with v2 by
       construction: it can never accept a real VOC cache, and v2 can never
       accept a synthetic one. The scoring target always passes --protocol v2. */
    {
        "synthetic-test",
        /* manifest_version */ 1,
        /* dataset */ "SYNTHETIC_TEST", /* cls */ "car",
        /* split_key */ "sha256_content_hash_trainval",
        /* seed */ 20260727,
        /* hog_side */ 64, /* color */ 1, /* hog_dim */ 1764, /* pca_dim */ 8,
        /* ss_width */ 300, /* max_prop */ 300, /* min_side */ 16,
        /* nms_iou_x100 */ 30, /* match_iou_x100 */ 50,
        /* test_offset */ 1000, /* test_count */ 40,
        /* train_img */ 400, /* val_img */ 40,
        /* pca_fit_images */ 2, /* pca_fit_rows */ 8,
        /* requires_prev */ 1, /* prev_test_count */ 40,
        /* prev_pack_sha */ "3444b1b3c026a8b47e0a07b242306eb946a2e0dc717522f933e14eedd6b02ce3",
        /* id_root_train */ "b564f7695637a5213bdbb5451ab4379e7ae7e4f84af5c70d1d8dc1efbe3d1f7b",
        /* id_root_val */ "af1e12cec6eb9ae0dc55ae12861dc27292c195976f94e4fda2f8c8b44338be36",
        /* id_root_test */ "13b3104aef253ed23bd4214de08a0971d28116d1bebd2bbcc175c486c8f98c70",
        /* content_root_train */ "c4a4f3282236f6c821878d63107bf6715582e334719d0d8039293cdd1ab5ddfe",
        /* content_root_val */ "a860d640fade02e0fa739c588ee7218db7d8b55960d6d55918b9185f3f6b20dc",
        /* content_root_test */ "1c69eea3f257843ffa3a417be0c5b17cc09c19f36dd86332162d19f0d7304436",
        /* artifact_root */ "ca9bb6112e7170becc292c5aff9118ab3403ea1f700ddd6ba190bb3408111441"
    }
};

const VdProtocol *vd_protocol_get(const char *name) {
    size_t i;
    if (!name) return NULL;
    for (i = 0; i < sizeof PROTOCOLS / sizeof PROTOCOLS[0]; i++)
        if (!strcmp(PROTOCOLS[i].name, name)) return &PROTOCOLS[i];
    return NULL;
}

/* ---------------- strict schema ------------------------------------------ */
typedef enum { T_LONG, T_STR, T_HEX } Ty;

typedef struct {
    const char *key;
    Ty ty;
    size_t off;
    size_t cap;
} Field;

#define F_L(k, m) { k, T_LONG, offsetof(VdManifest, m), 0 }
#define F_S(k, m) { k, T_STR,  offsetof(VdManifest, m), sizeof ((VdManifest *)0)->m }
#define F_H(k, m) { k, T_HEX,  offsetof(VdManifest, m), sizeof ((VdManifest *)0)->m }

static const Field FIELDS[] = {
    F_L("manifest_version", manifest_version),
    F_S("variant", variant), F_S("dataset", dataset), F_S("class", cls),
    F_S("split_key", split_key),
    F_L("seed", seed),
    F_L("hog_side", hog_side), F_L("color", color),
    F_L("hog_dim", hog_dim), F_L("pca_dim", pca_dim),
    F_L("ss_width", ss_width), F_L("max_prop", max_prop), F_L("min_side", min_side),
    F_L("nms_iou_x100", nms_iou_x100), F_L("match_iou_x100", match_iou_x100),
    F_L("test_offset", test_offset), F_L("test_count", test_count),
    F_L("train_img", train_img), F_L("val_img", val_img), F_L("test_img", test_img),
    F_L("train_prop", train_prop), F_L("val_prop", val_prop), F_L("test_prop", test_prop),
    F_L("pca_fit_images", pca_fit_images), F_L("pca_fit_rows", pca_fit_rows),
    F_L("trainval_id_overlap", trainval_id_overlap),
    F_L("trainval_content_overlap", trainval_content_overlap),
    F_L("prev_test_ids_checked", prev_test_ids_checked),
    F_L("prev_test_sha_checked", prev_test_sha_checked),
    F_L("prev_test_id_overlap", prev_test_id_overlap),
    F_L("prev_test_content_overlap", prev_test_content_overlap),
    F_H("sha256_train_pack", sha_train), F_H("sha256_val_pack", sha_val),
    F_H("sha256_test_pack", sha_test), F_H("sha256_pca_bin", sha_pca),
    F_H("sha256_prev_test_pack", sha_prev),
    F_H("sha256_ids_train", sha_ids_train), F_H("sha256_ids_val", sha_ids_val),
    F_H("sha256_ids_test", sha_ids_test),
    F_H("sha256_content_train", sha_content_train),
    F_H("sha256_content_val", sha_content_val),
    F_H("sha256_content_test", sha_content_test),
    F_H("id_root_train", id_root_train), F_H("id_root_val", id_root_val),
    F_H("id_root_test", id_root_test),
    F_H("content_root_train", content_root_train),
    F_H("content_root_val", content_root_val),
    F_H("content_root_test", content_root_test),
    F_H("artifact_root", artifact_root)
};
#define NFIELDS (sizeof FIELDS / sizeof FIELDS[0])

static int is_hex64(const char *s) {
    int i;
    if (!strcmp(s, "none")) return 1;   /* explicit absence, only where allowed */
    for (i = 0; i < 64; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return s[64] == 0;
}

static int strict_long(const char *t, long *out) {
    char *end = NULL;
    long v;
    if (!*t) return -1;
    if (t[0] == '+' ) return -1;
    if (t[0] == '0' && t[1] != 0) return -1;             /* no leading zeros */
    if (t[0] == '-' && (t[1] == '0' || t[1] == 0)) return -1;
    v = strtol(t, &end, 10);
    if (!end || *end) return -1;                          /* full consumption */
    if (v == LONG_MAX || v == LONG_MIN) return -1;
    *out = v;
    return 0;
}

static int manifest_parse_stream(FILE *f, VdManifest *m, char *err, size_t errn) {
    char *buf;
    long sz;
    size_t got, pos = 0, i;
    int seen[NFIELDS];

    memset(m, 0, sizeof *m);
    memset(seen, 0, sizeof seen);
    if (err && errn) err[0] = 0;
    if (!f) { if (err) snprintf(err, errn, "manifest_unreadable"); return -1; }
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 65536) {
        if (err) snprintf(err, errn, "manifest_size_out_of_range");
        return -1;
    }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { if (err) snprintf(err, errn, "manifest_alloc"); return -1; }
    got = fread(buf, 1, (size_t)sz, f);
    if (got != (size_t)sz) { free(buf); if (err) snprintf(err, errn, "manifest_short_read"); return -1; }
    buf[sz] = 0;
    if (memchr(buf, 0, (size_t)sz)) { free(buf); if (err) snprintf(err, errn, "manifest_nul_byte"); return -1; }
    if (buf[sz - 1] != '\n') { free(buf); if (err) snprintf(err, errn, "manifest_no_final_newline"); return -1; }

    while (pos < (size_t)sz) {
        char *nl = (char *)memchr(buf + pos, '\n', (size_t)sz - pos);
        char *line, *sp;
        size_t li;
        int found = -1;
        if (!nl) { free(buf); if (err) snprintf(err, errn, "manifest_unterminated_line"); return -1; }
        *nl = 0;
        line = buf + pos;
        pos = (size_t)(nl - buf) + 1;

        if (!*line) { free(buf); if (err) snprintf(err, errn, "manifest_blank_line"); return -1; }
        /* exactly one single space, no tabs/CR, no leading or trailing space */
        if (line[0] == ' ' || strchr(line, '\t') || strchr(line, '\r')) {
            free(buf); if (err) snprintf(err, errn, "manifest_whitespace_smuggling"); return -1;
        }
        sp = strchr(line, ' ');
        if (!sp || sp == line || !sp[1] || strchr(sp + 1, ' ')) {
            free(buf); if (err) snprintf(err, errn, "manifest_malformed_line"); return -1;
        }
        *sp = 0;
        for (li = 0; li < NFIELDS; li++)
            if (!strcmp(FIELDS[li].key, line)) { found = (int)li; break; }
        if (found < 0) {
            if (err) snprintf(err, errn, "manifest_unknown_key:%s", line);
            free(buf);   /* err is filled first: line points into buf */
            return -1;
        }
        if (seen[found]) {
            if (err) snprintf(err, errn, "manifest_duplicate_key:%s", line);
            free(buf);
            return -1;
        }
        seen[found] = 1;
        {
            const Field *fd = &FIELDS[found];
            const char *val = sp + 1;
            void *dst = (char *)m + fd->off;
            if (fd->ty == T_LONG) {
                if (strict_long(val, (long *)dst) != 0) {
                    free(buf);
                    if (err) snprintf(err, errn, "manifest_bad_number:%s", fd->key);
                    return -1;
                }
            } else if (fd->ty == T_HEX) {
                if (!is_hex64(val)) {
                    free(buf);
                    if (err) snprintf(err, errn, "manifest_bad_digest:%s", fd->key);
                    return -1;
                }
                snprintf((char *)dst, fd->cap, "%s", val);
            } else {
                if (strlen(val) >= fd->cap) {
                    free(buf);
                    if (err) snprintf(err, errn, "manifest_value_too_long:%s", fd->key);
                    return -1;
                }
                snprintf((char *)dst, fd->cap, "%s", val);
            }
        }
    }
    free(buf);
    for (i = 0; i < NFIELDS; i++)
        if (!seen[i]) {
            if (err) snprintf(err, errn, "manifest_missing_key:%s", FIELDS[i].key);
            return -1;
        }
    return 0;
}

int vd_manifest_parse(const char *path, VdManifest *m, char *err, size_t errn) {
    FILE *f = fopen(path, "rb");
    int rc;
    if (!f) { memset(m, 0, sizeof *m); if (err) snprintf(err, errn, "manifest_unreadable"); return -1; }
    rc = manifest_parse_stream(f, m, err, errn);
    fclose(f);
    return rc;
}

int vd_manifest_parse_fd(int fd, VdManifest *m, char *err, size_t errn) {
    FILE *f = (FILE *)vd_fdopen_ro(fd);
    int rc;
    if (!f) { memset(m, 0, sizeof *m); if (err) snprintf(err, errn, "manifest_unreadable"); return -1; }
    rc = manifest_parse_stream(f, m, err, errn);
    fclose(f);
    return rc;
}

#define WANT_L(field, expect) \
    do { if ((field) != (expect)) { \
        if (err) snprintf(err, errn, "protocol_mismatch:%s got=%ld want=%ld", \
                          #field, (long)(field), (long)(expect)); \
        return -1; } } while (0)
#define WANT_S(field, expect) \
    do { if (strcmp((field), (expect)) != 0) { \
        if (err) snprintf(err, errn, "protocol_mismatch:%s got=%s want=%s", \
                          #field, (field), (expect)); \
        return -1; } } while (0)

int vd_manifest_check(const VdManifest *m, const VdProtocol *p, char *err, size_t errn) {
    if (!m || !p) return -1;
    if (err && errn) err[0] = 0;
    WANT_L(m->manifest_version, p->manifest_version);
    WANT_S(m->variant, p->name);
    WANT_S(m->dataset, p->dataset);
    WANT_S(m->cls, p->cls);
    WANT_S(m->split_key, p->split_key);
    WANT_L(m->seed, p->seed);
    WANT_L(m->hog_side, p->hog_side);
    WANT_L(m->color, p->color);
    WANT_L(m->hog_dim, p->hog_dim);
    WANT_L(m->pca_dim, p->pca_dim);
    WANT_L(m->ss_width, p->ss_width);
    WANT_L(m->max_prop, p->max_prop);
    WANT_L(m->min_side, p->min_side);
    WANT_L(m->nms_iou_x100, p->nms_iou_x100);
    WANT_L(m->match_iou_x100, p->match_iou_x100);
    WANT_L(m->test_offset, p->test_offset);
    WANT_L(m->test_count, p->test_count);
    WANT_L(m->test_img, p->test_count);
    WANT_L(m->train_img, p->train_img);
    WANT_L(m->val_img, p->val_img);
    WANT_L(m->pca_fit_images, p->pca_fit_images);
    WANT_L(m->pca_fit_rows, p->pca_fit_rows);
    /* leakage counters are only ever allowed to be zero */
    WANT_L(m->trainval_id_overlap, 0);
    WANT_L(m->trainval_content_overlap, 0);
    WANT_L(m->prev_test_id_overlap, 0);
    WANT_L(m->prev_test_content_overlap, 0);
    if (p->requires_prev) {
        WANT_L(m->prev_test_ids_checked, p->prev_test_count);
        WANT_L(m->prev_test_sha_checked, p->prev_test_count);
        if (*p->prev_pack_sha && strcmp(m->sha_prev, p->prev_pack_sha) != 0) {
            if (err) snprintf(err, errn, "protocol_mismatch:sha256_prev_test_pack");
            return -1;
        }
    }
    /* Source identity: pinned roots, not self-description. */
    if (*p->id_root_test && strcmp(m->id_root_test, p->id_root_test) != 0) {
        if (err) snprintf(err, errn, "protocol_mismatch:id_root_test");
        return -1;
    }
    if (*p->content_root_test && strcmp(m->content_root_test, p->content_root_test) != 0) {
        if (err) snprintf(err, errn, "protocol_mismatch:content_root_test");
        return -1;
    }
    if (*p->id_root_train && strcmp(m->id_root_train, p->id_root_train) != 0) {
        if (err) snprintf(err, errn, "protocol_mismatch:id_root_train");
        return -1;
    }
    if (*p->id_root_val && strcmp(m->id_root_val, p->id_root_val) != 0) {
        if (err) snprintf(err, errn, "protocol_mismatch:id_root_val");
        return -1;
    }
    if (*p->content_root_train && strcmp(m->content_root_train, p->content_root_train) != 0) {
        if (err) snprintf(err, errn, "protocol_mismatch:content_root_train");
        return -1;
    }
    if (*p->content_root_val && strcmp(m->content_root_val, p->content_root_val) != 0) {
        if (err) snprintf(err, errn, "protocol_mismatch:content_root_val");
        return -1;
    }
    /* The artifact root is deliberately NOT compared here. Authority for it is
       the value the scorer RECOMPUTES from the bytes it holds open, which is
       then required to equal both this manifest's claim and the pinned
       protocol constant. Checking the declared value here would short-circuit
       that and leave the byte-derived check unreachable. */
    return 0;
}

/* ---------------- root recomputation ------------------------------------- */
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int lines_of_stream(FILE *f, char ***out, size_t *n_out) {
    char line[256];
    char **v = NULL;
    size_t n = 0, cap = 0, i;
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        if (l == 0 || line[l - 1] != '\n') goto fail;
        line[--l] = 0;
        if (l == 0 || l > 200) goto fail;
        if (n == cap) {
            char **nv;
            cap = cap ? cap * 2 : 1024;
            nv = (char **)realloc(v, cap * sizeof *v);
            if (!nv) goto fail;
            v = nv;
        }
        v[n] = (char *)malloc(l + 1);
        if (!v[n]) goto fail;
        memcpy(v[n], line, l + 1);
        n++;
    }
    if (n == 0) goto fail;
    qsort(v, n, sizeof *v, cmp_str);
    *out = v;
    *n_out = n;
    return 0;
fail:
    for (i = 0; i < n; i++) free(v[i]);
    free(v);
    return -1;
}

void vd_lines_free(char **v, size_t n) {
    size_t i;
    if (!v) return;
    for (i = 0; i < n; i++) free(v[i]);
    free(v);
}

static int root_of_stream(FILE *f, char *hex_out, size_t *n_lines) {
    char **v = NULL;
    size_t n = 0, i;
    VdSha256 c;
    if (lines_of_stream(f, &v, &n) != 0) return -1;
    vd_sha256_init(&c);
    for (i = 0; i < n; i++) {
        vd_sha256_update(&c, v[i], strlen(v[i]));
        vd_sha256_update(&c, "\n", 1);
    }
    vd_sha256_hex(&c, hex_out);
    vd_lines_free(v, n);
    if (n_lines) *n_lines = n;
    return 0;
}

int vd_root_of_file(const char *path, char *hex_out, size_t *n_lines) {
    FILE *f = fopen(path, "rb");
    int rc;
    if (!f) return -1;
    rc = root_of_stream(f, hex_out, n_lines);
    fclose(f);
    return rc;
}

int vd_root_of_fd(int fd, char *hex_out, size_t *n_lines) {
    FILE *f = (FILE *)vd_fdopen_ro(fd);
    int rc;
    if (!f) return -1;
    rc = root_of_stream(f, hex_out, n_lines);
    fclose(f);
    return rc;
}

int vd_lines_of_fd(int fd, char ***out, size_t *n) {
    FILE *f = (FILE *)vd_fdopen_ro(fd);
    int rc;
    if (!f) return -1;
    rc = lines_of_stream(f, out, n);
    fclose(f);
    return rc;
}
