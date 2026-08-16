#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "../../include/cce/cce_qgkp.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../../include/cnet_platform.h"  /* cnet_fsync */

typedef struct qgkp_v3_disk_header {
    uint32_t magic;
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
    int32_t reserved;
} qgkp_v3_disk_header;

typedef char qgkp_header_must_fit[
    sizeof(qgkp_v3_disk_header) <= CCE_QGKP_HEADER_BYTES ? 1 : -1];

static uint64_t fnv1a_update(uint64_t h, const unsigned char *buf, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        h ^= (uint64_t)buf[i];
        h *= 1099511628211ull;
    }
    return h;
}

static cce_result hash_stream(FILE *f, uint64_t offset, uint64_t bytes,
                              uint64_t *hash_out) {
    unsigned char buf[1024 * 1024];
    uint64_t left = bytes;
    uint64_t h = 1469598103934665603ull;
    if (fseeko(f, (off_t)offset, SEEK_SET) != 0) return CCE_ERR_IO;
    while (left > 0) {
        size_t want = left < sizeof buf ? (size_t)left : sizeof buf;
        size_t got = fread(buf, 1, want, f);
        if (got != want) return CCE_ERR_IO;
        h = fnv1a_update(h, buf, got);
        left -= got;
    }
    *hash_out = h;
    return CCE_OK;
}

static cce_result stream_size(FILE *f, uint64_t *size_out) {
    off_t here = ftello(f);
    if (here < 0 || fseeko(f, 0, SEEK_END) != 0) return CCE_ERR_IO;
    off_t end = ftello(f);
    if (end < 0 || fseeko(f, here, SEEK_SET) != 0) return CCE_ERR_IO;
    *size_out = (uint64_t)end;
    return CCE_OK;
}

static cce_result read_header(FILE *f, qgkp_v3_disk_header *h) {
    if (fseeko(f, 0, SEEK_SET) != 0 || fread(h, sizeof *h, 1, f) != 1)
        return CCE_ERR_IO;
    if (h->magic != CCE_QGKP_MAGIC || h->version != CCE_QGKP_VERSION_ENVELOPE ||
        h->header_bytes != CCE_QGKP_HEADER_BYTES ||
        !(h->flags & CCE_QGKP_FLAG_EMBEDDED_GGUF) || h->payload_bytes == 0)
        return CCE_ERR_UNSUPPORTED;
    h->architecture[sizeof h->architecture - 1] = 0;
    h->quantization[sizeof h->quantization - 1] = 0;
    return CCE_OK;
}

cce_result cce_qgkp_inspect(const char *path, cce_qgkp_info *out) {
    if (!path || !out) return CCE_ERR_INVALID_ARG;
    FILE *f = fopen(path, "rb");
    if (!f) return CCE_ERR_IO;
    qgkp_v3_disk_header h;
    cce_result rc = read_header(f, &h);
    fclose(f);
    if (rc != CCE_OK) return rc;
    memset(out, 0, sizeof *out);
    out->version = h.version;
    out->header_bytes = h.header_bytes;
    out->flags = h.flags;
    out->payload_bytes = h.payload_bytes;
    out->payload_hash = h.payload_hash;
    memcpy(out->architecture, h.architecture, sizeof out->architecture);
    memcpy(out->quantization, h.quantization, sizeof out->quantization);
    out->architecture[sizeof out->architecture - 1] = 0;
    out->quantization[sizeof out->quantization - 1] = 0;
    out->n_layer = h.n_layer;
    out->hidden = h.hidden;
    out->context_length = h.context_length;
    return CCE_OK;
}

static cce_result compare_prefix(FILE *a, uint64_t a_offset, FILE *b,
                                 uint64_t b_offset, uint64_t bytes) {
    unsigned char aa[1024 * 1024], bb[1024 * 1024];
    if (fseeko(a, (off_t)a_offset, SEEK_SET) != 0 ||
        fseeko(b, (off_t)b_offset, SEEK_SET) != 0) return CCE_ERR_IO;
    while (bytes > 0) {
        size_t n = bytes < sizeof aa ? (size_t)bytes : sizeof aa;
        if (fread(aa, 1, n, a) != n || fread(bb, 1, n, b) != n ||
            memcmp(aa, bb, n) != 0) return CCE_ERR_IO;
        bytes -= n;
    }
    return CCE_OK;
}

static cce_result append_range(FILE *src, uint64_t src_offset, FILE *dst,
                               uint64_t bytes) {
    unsigned char buf[1024 * 1024];
    if (fseeko(src, (off_t)src_offset, SEEK_SET) != 0 ||
        fseeko(dst, 0, SEEK_END) != 0) return CCE_ERR_IO;
    while (bytes > 0) {
        size_t n = bytes < sizeof buf ? (size_t)bytes : sizeof buf;
        if (fread(buf, 1, n, src) != n || fwrite(buf, 1, n, dst) != n)
            return CCE_ERR_IO;
        bytes -= n;
    }
    if (fflush(dst) != 0 || cnet_fsync(fileno(dst)) != 0) return CCE_ERR_IO;
    return CCE_OK;
}

cce_result cce_qgkp_pack_gguf(const char *gguf_path, const char *qgkp_path,
                              const cce_qgkp_metadata *meta) {
    if (!gguf_path || !qgkp_path || !meta) return CCE_ERR_INVALID_ARG;
    FILE *src = fopen(gguf_path, "rb");
    if (!src) return CCE_ERR_IO;
    unsigned char magic[4];
    uint64_t source_size = 0, source_hash = 0;
    if (fread(magic, 1, 4, src) != 4 || memcmp(magic, "GGUF", 4) != 0 ||
        stream_size(src, &source_size) != CCE_OK || source_size < 8 ||
        hash_stream(src, 0, source_size, &source_hash) != CCE_OK) {
        fclose(src); return CCE_ERR_UNSUPPORTED;
    }

    FILE *dst = fopen(qgkp_path, "r+b");
    uint64_t have = 0;
    qgkp_v3_disk_header h;
    if (dst) {
        uint64_t dst_size = 0;
        if (read_header(dst, &h) != CCE_OK || stream_size(dst, &dst_size) != CCE_OK ||
            h.payload_bytes != source_size || h.payload_hash != source_hash ||
            dst_size < h.header_bytes || dst_size > h.header_bytes + source_size ||
            strcmp(h.architecture, meta->architecture) != 0 ||
            strcmp(h.quantization, meta->quantization) != 0) {
            fclose(src); fclose(dst); return CCE_ERR_IO;
        }
        have = dst_size - h.header_bytes;
        if (compare_prefix(src, 0, dst, h.header_bytes, have) != CCE_OK) {
            fclose(src); fclose(dst); return CCE_ERR_IO;
        }
    } else {
        dst = fopen(qgkp_path, "w+b");
        if (!dst) { fclose(src); return CCE_ERR_IO; }
        memset(&h, 0, sizeof h);
        h.magic = CCE_QGKP_MAGIC;
        h.version = CCE_QGKP_VERSION_ENVELOPE;
        h.header_bytes = CCE_QGKP_HEADER_BYTES;
        h.flags = meta->flags | CCE_QGKP_FLAG_EMBEDDED_GGUF;
        h.payload_bytes = source_size;
        h.payload_hash = source_hash;
        snprintf(h.architecture, sizeof h.architecture, "%s", meta->architecture);
        snprintf(h.quantization, sizeof h.quantization, "%s", meta->quantization);
        h.n_layer = meta->n_layer;
        h.hidden = meta->hidden;
        h.context_length = meta->context_length;
        unsigned char header[CCE_QGKP_HEADER_BYTES];
        memset(header, 0, sizeof header);
        memcpy(header, &h, sizeof h);
        if (fwrite(header, 1, sizeof header, dst) != sizeof header) {
            fclose(src); fclose(dst); return CCE_ERR_IO;
        }
    }
    cce_result rc = append_range(src, have, dst, source_size - have);
    fclose(src); fclose(dst);
    return rc;
}

cce_result cce_qgkp_materialize_gguf(const char *qgkp_path,
                                     const char *gguf_path) {
    if (!qgkp_path || !gguf_path) return CCE_ERR_INVALID_ARG;
    FILE *src = fopen(qgkp_path, "rb");
    if (!src) return CCE_ERR_IO;
    qgkp_v3_disk_header h;
    uint64_t src_size = 0, actual_hash = 0;
    if (read_header(src, &h) != CCE_OK || stream_size(src, &src_size) != CCE_OK ||
        src_size != h.header_bytes + h.payload_bytes ||
        hash_stream(src, h.header_bytes, h.payload_bytes, &actual_hash) != CCE_OK ||
        actual_hash != h.payload_hash) {
        fclose(src); return CCE_ERR_IO;
    }

    FILE *dst = fopen(gguf_path, "r+b");
    uint64_t have = 0;
    if (dst) {
        if (stream_size(dst, &have) != CCE_OK || have > h.payload_bytes ||
            compare_prefix(src, h.header_bytes, dst, 0, have) != CCE_OK) {
            fclose(src); fclose(dst); return CCE_ERR_IO;
        }
    } else {
        dst = fopen(gguf_path, "w+b");
        if (!dst) { fclose(src); return CCE_ERR_IO; }
    }
    cce_result rc = append_range(src, h.header_bytes + have, dst,
                                 h.payload_bytes - have);
    fclose(src); fclose(dst);
    return rc;
}
