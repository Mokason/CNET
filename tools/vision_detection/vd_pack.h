/* Feature-pack reader with protocol-derived caps.
 *
 * A pack is benchmark evidence produced by vd_prep and consumed by vd_bench. It
 * is parsed defensively: every count is bounded before it is used to allocate
 * or index, all arithmetic that could overflow is done in uint64, the declared
 * aggregate proposal count must match what was actually read, and the file must
 * end exactly where the structure says it ends. Anything else is refused with a
 * nonzero reason -- a benchmark that scores a malformed pack is worse than one
 * that refuses to run.
 */
#ifndef VD_PACK_H
#define VD_PACK_H

#include <stddef.h>

#include "vd_eval.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Caps follow the frozen protocol, with headroom; they are not tuning knobs. */
#define VD_MAX_IMAGES          20000    /* VOC2007 trainval+test is 9963 */
#define VD_MAX_PROPS_PER_IMAGE 300      /* MAX_PROP in vd_prep */
#define VD_MAX_GT_PER_IMAGE    256
#define VD_MAX_TOTAL_PROPS     8000000
#define VD_MAX_DIM             256      /* PCA256 is the largest variant */
#define VD_MAX_ID_LEN          30
#define VD_MAX_COORD           1000000  /* image coordinates, generously bounded */

typedef struct {
    char id[VD_MAX_ID_LEN + 2];
    VdBox *gts;  int *gt_dif;  size_t n_gt;
    VdBox *pb;   int *plabel;  float *pf;  size_t n_prop;
} VdPackImg;

typedef struct {
    VdPackImg *imgs;
    size_t n;
    int dim;
    size_t total_prop;
} VdPack;

/* Reason codes; all nonzero values mean "refused". */
enum {
    VD_PACK_OK = 0,
    VD_PACK_E_OPEN = -1,
    VD_PACK_E_MAGIC = -2,
    VD_PACK_E_HEADER = -3,
    VD_PACK_E_COUNT = -4,
    VD_PACK_E_TRUNC = -5,
    VD_PACK_E_AGGREGATE = -6,
    VD_PACK_E_TRAILING = -7,
    VD_PACK_E_FIELD = -8,
    VD_PACK_E_ALLOC = -9
};

const char *vd_pack_strerror(int code);

/* Load a full pack. On any nonzero return, *p is left zeroed and every partial
   allocation is released (transactional). */
int vd_pack_load(const char *path, VdPack *p);
/* Load from a descriptor the caller already holds -- the scored path, so the
   bytes parsed are the bytes that were hashed. */
int vd_pack_load_fd(int fd, VdPack *p);
void vd_pack_free(VdPack *p);

/* Structural walk that returns only the image IDs. Applies the identical
   validation as vd_pack_load, so it cannot accept a file the loader rejects.
   Caller frees *ids (each entry and the array). */
int vd_pack_read_ids(const char *path, char ***ids, size_t *n);
void vd_pack_free_ids(char **ids, size_t n);

#ifdef __cplusplus
}
#endif

#endif
