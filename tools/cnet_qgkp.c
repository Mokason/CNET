#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cce/cce_detect.h"
#include "../include/cce/cce_qgkp.h"

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage:\n"
            "  %s pack <source.gguf> <output.qgkp.partial> [quant-label]\n"
            "  %s materialize <source.qgkp> <output.gguf.partial>\n"
            "  %s inspect <source.qgkp>\n",
            argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(argv[0]); return 2; }
    if (strcmp(argv[1], "inspect") == 0 && argc == 3) {
        cce_qgkp_info q;
        if (cce_qgkp_inspect(argv[2], &q) != CCE_OK) {
            fprintf(stderr, "QGKP_INSPECT_FAIL path=%s\n", argv[2]);
            return 1;
        }
        printf("QGKP_INSPECT_PASS version=%u flags=%llu payload_bytes=%llu "
               "payload_hash=%016llx arch=%s quant=%s layers=%d hidden=%d ctx=%d\n",
               q.version, (unsigned long long)q.flags,
               (unsigned long long)q.payload_bytes,
               (unsigned long long)q.payload_hash, q.architecture,
               q.quantization, q.n_layer, q.hidden, q.context_length);
        return 0;
    }
    if (strcmp(argv[1], "materialize") == 0 && argc == 4) {
        cce_result rc = cce_qgkp_materialize_gguf(argv[2], argv[3]);
        if (rc != CCE_OK) {
            fprintf(stderr, "QGKP_MATERIALIZE_FAIL rc=%d source=%s output=%s\n",
                    rc, argv[2], argv[3]);
            return 1;
        }
        printf("QGKP_MATERIALIZE_PASS source=%s output=%s\n", argv[2], argv[3]);
        return 0;
    }
    if (strcmp(argv[1], "pack") == 0 && (argc == 4 || argc == 5)) {
        cce_model_info info;
        if (cce_detect_file(argv[2], &info) != CCE_OK ||
            info.format != CCE_FMT_GGUF) {
            fprintf(stderr, "QGKP_PACK_FAIL source is not a readable GGUF: %s\n", argv[2]);
            return 1;
        }
        cce_qgkp_metadata meta;
        memset(&meta, 0, sizeof meta);
        snprintf(meta.architecture, sizeof meta.architecture, "%s",
                 info.arch[0] ? info.arch : "unknown");
        snprintf(meta.quantization, sizeof meta.quantization, "%s",
                 argc == 5 ? argv[4] : (info.dtype[0] ? info.dtype : "mixed"));
        meta.n_layer = info.n_layer;
        meta.hidden = info.hidden;
        meta.context_length = info.ctx_len;
        meta.flags = CCE_QGKP_FLAG_PACKET_TRITS;
        if (strchr(meta.quantization, '+') != NULL || strstr(meta.quantization, "mixed"))
            meta.flags |= CCE_QGKP_FLAG_MIXED_QUANT;
        cce_result rc = cce_qgkp_pack_gguf(argv[2], argv[3], &meta);
        if (rc != CCE_OK) {
            fprintf(stderr, "QGKP_PACK_FAIL rc=%d source=%s output=%s\n",
                    rc, argv[2], argv[3]);
            return 1;
        }
        cce_qgkp_info q;
        if (cce_qgkp_inspect(argv[3], &q) != CCE_OK) return 1;
        printf("QGKP_PACK_PASS output=%s payload_bytes=%llu payload_hash=%016llx "
               "arch=%s quant=%s\n", argv[3],
               (unsigned long long)q.payload_bytes,
               (unsigned long long)q.payload_hash,
               q.architecture, q.quantization);
        return 0;
    }
    usage(argv[0]);
    return 2;
}
