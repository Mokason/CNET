/* RED marker: CORE_BUS_TENSOR_REFUSAL_RED
 * Explicit host tensor names must resolve exactly or through the documented
 * same-block Q/K-to-QKV mapping. A typo must never select an unrelated tensor.
 */
#include "cnet_core_bus.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int checks;
static int failures;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void write_u32(FILE *f, uint32_t value) {
    (void)fwrite(&value, sizeof value, 1, f);
}

static void write_u64(FILE *f, uint64_t value) {
    (void)fwrite(&value, sizeof value, 1, f);
}

static void write_string(FILE *f, const char *value) {
    write_u64(f, (uint64_t)strlen(value));
    (void)fwrite(value, 1, strlen(value), f);
}

static int write_host_fixture(const char *path) {
    FILE *f = fopen(path, "wb");
    float weights[128];
    long pos;
    int pad;
    size_t i;
    if (!f) return -1;
    for (i = 0; i < 128; ++i) weights[i] = (i & 1u) ? 1.0f : -1.0f;

    (void)fwrite("GGUF", 1, 4, f);
    write_u32(f, 3);
    write_u64(f, 1); /* tensor count */
    write_u64(f, 1); /* metadata count */
    write_string(f, "general.alignment");
    write_u32(f, 4); /* GGUF_TYPE_UINT32 */
    write_u32(f, 32);
    write_string(f, "blk.0.attn_qkv.weight");
    write_u32(f, 1);
    write_u64(f, 128);
    write_u32(f, 0); /* GGML_TYPE_F32 */
    write_u64(f, 0);
    pos = ftell(f);
    if (pos < 0) {
        fclose(f);
        return -1;
    }
    pad = (int)((32 - (unsigned)(pos % 32)) % 32);
    while (pad-- > 0) (void)fputc(0, f);
    if (fwrite(weights, sizeof weights[0], 128, f) != 128) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

int main(void) {
    const char *host = "/tmp/cnet_core_bus_tensor_host.gguf";
    const char *out = "/tmp/cnet_core_bus_tensor_out.gguf";
    int rc;

    unlink(host);
    unlink(out);
    check(write_host_fixture(host) == 0, "synthetic host GGUF created");

    rc = cnet_core_bus_write_q1_domain_from_host_ex(
        host, "blk.0.attn_q.weight", out, 0);
    check(rc == 0 && access(out, F_OK) == 0,
          "documented Q-to-QKV mapping remains supported");

    unlink(out);
    rc = cnet_core_bus_write_q1_domain_from_host_ex(
        host, "blk.0.ffn_dwon.weight", out, 0);
    check(rc != 0, "misspelled explicit tensor is refused");
    check(access(out, F_OK) != 0, "refusal leaves no output artifact");

    unlink(host);
    unlink(out);
    if (failures) {
        printf("CORE_BUS_TENSOR_REFUSAL_RED checks=%d fails=%d\n", checks,
               failures);
        return 1;
    }
    printf("CORE_BUS_TENSOR_REFUSAL_PASS checks=%d fails=0 "
           "explicit_miss_fallback=0 broader_claims=WITHHELD\n",
           checks);
    return 0;
}
