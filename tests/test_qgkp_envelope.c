#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/cce/cce_qgkp.h"

static int checks;
static int failures;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { failures++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)

static int same_file(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");
    int equal = fa != NULL && fb != NULL;
    while (equal) {
        unsigned char aa[31], bb[31];
        size_t na = fread(aa, 1, sizeof aa, fa);
        size_t nb = fread(bb, 1, sizeof bb, fb);
        if (na != nb || memcmp(aa, bb, na) != 0) equal = 0;
        if (na < sizeof aa) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return equal;
}

int main(void) {
    const char *src = "qgkp_fixture.gguf";
    const char *pack = "qgkp_fixture.qgkp.partial";
    const char *out = "qgkp_fixture.restored.partial";
    FILE *f = fopen(src, "wb");
    const unsigned char payload[] = {
        'G','G','U','F',3,0,0,0,0,1,2,3,4,5,6,7,8,9,
        0,242,1,80,65,67,75,69,84,45,84,82,73,84
    };
    fwrite(payload, 1, sizeof payload, f);
    fclose(f);

    cce_qgkp_metadata meta;
    memset(&meta, 0, sizeof meta);
    snprintf(meta.architecture, sizeof meta.architecture, "qwen35");
    snprintf(meta.quantization, sizeof meta.quantization, "TQ1_0+Q4_K");
    meta.n_layer = 40;
    meta.hidden = 4096;
    meta.context_length = 262144;
    meta.flags = CCE_QGKP_FLAG_PACKET_TRITS | CCE_QGKP_FLAG_MIXED_QUANT;

    CHECK(cce_qgkp_pack_gguf(src, pack, &meta) == CCE_OK,
          "GGUF packs into a QGKP v3 envelope");

    cce_qgkp_info info;
    CHECK(cce_qgkp_inspect(pack, &info) == CCE_OK,
          "QGKP v3 header inspects");
    CHECK(info.version == CCE_QGKP_VERSION_ENVELOPE &&
          info.payload_bytes == sizeof payload &&
          strcmp(info.architecture, "qwen35") == 0 &&
          strcmp(info.quantization, "TQ1_0+Q4_K") == 0,
          "QGKP preserves model identity and payload size");
    CHECK((info.flags & CCE_QGKP_FLAG_PACKET_TRITS) != 0 &&
          (info.flags & CCE_QGKP_FLAG_EMBEDDED_GGUF) != 0,
          "QGKP marks packet-trit GGUF payload explicitly");

    CHECK(truncate(pack, (off_t)(info.header_bytes + 7)) == 0,
          "test simulates interrupted pack");
    CHECK(cce_qgkp_pack_gguf(src, pack, &meta) == CCE_OK,
          "packing resumes from a validated payload prefix");
    CHECK(cce_qgkp_materialize_gguf(pack, out) == CCE_OK && same_file(src, out),
          "materialized GGUF is byte-identical");

    CHECK(truncate(out, 9) == 0, "test simulates interrupted materialization");
    CHECK(cce_qgkp_materialize_gguf(pack, out) == CCE_OK && same_file(src, out),
          "materialization resumes from a validated prefix");

    f = fopen(pack, "r+b");
    fseek(f, (long)info.header_bytes + 4, SEEK_SET);
    int ch = fgetc(f);
    fseek(f, -1, SEEK_CUR);
    fputc(ch ^ 0x5a, f);
    fclose(f);
    CHECK(cce_qgkp_materialize_gguf(pack, out) == CCE_ERR_IO,
          "corrupt packet payload is rejected");

    remove(src); remove(pack); remove(out);
    if (failures) {
        fprintf(stderr, "QGKP_ENVELOPE_FAIL checks=%d failures=%d\n", checks, failures);
        return 1;
    }
    printf("QGKP_ENVELOPE_PASS checks=%d\n", checks);
    return 0;
}
