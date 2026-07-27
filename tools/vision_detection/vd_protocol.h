/* Pre-registered protocol identity, and a strict manifest schema.
 *
 * The failure this exists to prevent: a cache describing itself into a weaker
 * protocol. Previously the bench asked the manifest what was required of it, so
 * a cache declaring test_offset 0 was excused from the spent-holdout check
 * entirely. Identity now comes from a protocol selected by the SCORING TARGET
 * and compiled in here; the manifest is only ever checked against it, never
 * consulted to decide what the rules are.
 *
 * Roots are digests over the canonical selection derived from the official VOC
 * files: sha256 of the sorted "value\n" lines. They pin *which images* a split
 * contains and *what those images are*, independently of anything the cache says
 * about itself.
 */
#ifndef VD_PROTOCOL_H
#define VD_PROTOCOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VD_HEX 65

typedef struct {
    long manifest_version;
    char variant[32], dataset[64], cls[32], split_key[64];
    long seed;
    long hog_side, color, hog_dim, pca_dim;
    long ss_width, max_prop, min_side, nms_iou_x100, match_iou_x100;
    long test_offset, test_count, train_img, val_img, test_img;
    long train_prop, val_prop, test_prop;
    long pca_fit_images, pca_fit_rows;
    long trainval_id_overlap, trainval_content_overlap;
    long prev_test_ids_checked, prev_test_sha_checked;
    long prev_test_id_overlap, prev_test_content_overlap;
    char sha_train[VD_HEX], sha_val[VD_HEX], sha_test[VD_HEX], sha_pca[VD_HEX];
    char sha_prev[VD_HEX];
    char sha_ids_train[VD_HEX], sha_ids_val[VD_HEX], sha_ids_test[VD_HEX];
    char sha_content_train[VD_HEX], sha_content_val[VD_HEX], sha_content_test[VD_HEX];
    char id_root_train[VD_HEX], id_root_val[VD_HEX], id_root_test[VD_HEX];
    char content_root_train[VD_HEX], content_root_val[VD_HEX], content_root_test[VD_HEX];
    char artifact_root[VD_HEX];
} VdManifest;

/* Everything a scored run must be, fixed in code before the run. */
typedef struct {
    const char *name;
    long manifest_version;
    const char *dataset, *cls, *split_key;
    long seed;
    long hog_side, color, hog_dim, pca_dim;
    long ss_width, max_prop, min_side, nms_iou_x100, match_iou_x100;
    long test_offset, test_count, train_img, val_img;
    long pca_fit_images, pca_fit_rows;
    int  requires_prev;          /* NOT negotiable by the cache */
    long prev_test_count;
    const char *prev_pack_sha;   /* "" until pinned */
    const char *id_root_train, *id_root_val, *id_root_test;
    const char *content_root_train, *content_root_val, *content_root_test;
    /* Binds the exact bytes of every scored member. Canonical sidecars with
       substituted features, GT, proposals or PCA change this root. */
    const char *artifact_root;
} VdProtocol;

/* The scored members, in the ONE canonical order that goes into the artifact
   root. Order, names, sizes and digests are all bound, so a member cannot be
   substituted, reordered, renamed or resized without changing the root. */
#define VD_N_MEMBERS 10
extern const char *const VD_MEMBERS[VD_N_MEMBERS];

typedef struct {
    const char *name;
    long long size;
    char sha[VD_HEX];
} VdMember;

/* sha256 over the canonical serialisation:
     "VDCACHEROOT1\n" "schema <v>\n" "members <n>\n" then "<name> <size> <sha>\n"
   for each member in VD_MEMBERS order. */
int vd_artifact_root(const VdMember *m, size_t n, long schema, char *hex_out);

const VdProtocol *vd_protocol_get(const char *name);

/* Strict, once-only schema. Every known key must appear exactly once; unknown
   keys, duplicates, missing keys, malformed or out-of-range numbers, embedded
   or smuggled whitespace, and trailing bytes are all refused. */
int vd_manifest_parse(const char *path, VdManifest *m, char *err, size_t errn);
/* Same, from an already-open descriptor -- the snapshot the scorer holds. */
int vd_manifest_parse_fd(int fd, VdManifest *m, char *err, size_t errn);

/* Compare a parsed manifest against a pre-registered protocol. */
int vd_manifest_check(const VdManifest *m, const VdProtocol *p, char *err, size_t errn);

/* sha256 of the file's sorted "value\n" lines, and the line count. Used to
   recompute a root from a sidecar rather than trusting a declared digest. */
int vd_root_of_file(const char *path, char *hex_out, size_t *n_lines);
int vd_root_of_fd(int fd, char *hex_out, size_t *n_lines);
/* Sorted lines of a sidecar, read from the held descriptor. */
int vd_lines_of_fd(int fd, char ***out, size_t *n);
void vd_lines_free(char **v, size_t n);

#ifdef __cplusplus
}
#endif

#endif
