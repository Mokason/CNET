#ifndef CCE_QGKP_H
#define CCE_QGKP_H

#include "cce_defs.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CCE_QGKP_MAGIC 0x504b4751u
#define CCE_QGKP_VERSION_ENVELOPE 3u
#define CCE_QGKP_HEADER_BYTES 4096u

#define CCE_QGKP_FLAG_EMBEDDED_GGUF (1ull << 0)
#define CCE_QGKP_FLAG_PACKET_TRITS  (1ull << 1)
#define CCE_QGKP_FLAG_MIXED_QUANT   (1ull << 2)

/* QGKP v3 is a lossless CNET-owned envelope around a runnable GGUF. TQ1_0
 * tensors remain base-3 packet-trits; non-ternary tensors remain byte-exact.
 * This gives CNET an integrity/resume boundary without changing model math. */
typedef struct cce_qgkp_metadata {
    uint64_t flags;
    char architecture[64];
    char quantization[32];
    int32_t n_layer;
    int32_t hidden;
    int32_t context_length;
    int32_t reserved;
} cce_qgkp_metadata;

typedef struct cce_qgkp_info {
    uint32_t version;
    uint64_t header_bytes;
    uint64_t flags;
    uint64_t payload_bytes;
    uint64_t payload_hash;
    char architecture[64];
    char quantization[32];
    int32_t n_layer;
    int32_t hidden;
    int32_t context_length;
} cce_qgkp_info;

cce_result cce_qgkp_inspect(const char *path, cce_qgkp_info *out);

/* Both operations are resumable. Existing destination prefixes are validated
 * byte-for-byte before appending; corrupt or mismatched prefixes are refused. */
cce_result cce_qgkp_pack_gguf(const char *gguf_path, const char *qgkp_partial_path,
                              const cce_qgkp_metadata *metadata);
cce_result cce_qgkp_materialize_gguf(const char *qgkp_path,
                                     const char *gguf_partial_path);

#ifdef __cplusplus
}
#endif

#endif
