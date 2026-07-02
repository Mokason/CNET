/*
 * Unit files: weights + contract as ONE sealed binary artifact.
 *
 *  - round-trip identity: loaded btn and contract have BIT-IDENTICAL content
 *    digests (the strongest possible gate), certify passes, seal_verified=1
 *  - one file: registry_save writes <name>.cnu; no .btn / .contract pair
 *  - size: the unit is smaller than the legacy text pair (measured)
 *  - security: any flipped byte or truncation is REFUSED (seal checked
 *    before parsing); incoherent saves refused (sig mismatch, non-canonical
 *    exemplar values)
 *  - multi-port signatures and tags round-trip
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/contract/unit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, desc) do {                       \
    if (cond) { printf("  ok   %s\n", (desc)); }     \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

static Port PT(PortFamily family, size_t field_width, size_t field_count,
               const char *tag) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    if (tag != NULL) {
        port_set_tag(&p, tag);
    }
    return p;
}

static void msb2(int i, double *out) {
    out[0] = (double)((i >> 1) & 1);
    out[1] = (double)(i & 1);
}

/* decoder: ONEHOT4 "sym" -> BINARY_MSB2 "val", i -> i */
static double g_inputs[4][4];
static double g_targets[4][2];

static int make_decoder(BinaryTransformNetwork *b) {
    int i;
    memset(g_inputs, 0, sizeof g_inputs);
    if (btn_init(b, 4, 2, 1, 16, 0.8, 11u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_ONEHOT, 4, 1, "sym"),
                      PT(PORT_BINARY_MSB, 2, 1, "val")) != 0) return -1;
    for (i = 0; i < 4; ++i) {
        g_inputs[i][i] = 1.0;
        msb2(i, g_targets[i]);
    }
    return btn_train_dynamic(b, &g_inputs[0][0], &g_targets[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

/* bigger untrained fixture for the size measurement + multi-port round-trip:
   2 input ports (ONEHOT8 "a" + BINARY_MSB8 "b") -> BINARY_MSB8 "y",
   128 canonical exemplars (values are patterns, not a learned mapping —
   round-trip and size need no certification). */
#define BIG_IN 16
#define BIG_OUT 8
#define BIG_HID 24
#define BIG_EX 128
static double big_in[BIG_EX][BIG_IN];
static double big_out[BIG_EX][BIG_OUT];

static int make_big(BinaryTransformNetwork *b, Contract *c) {
    Port ins[2];
    Port out;
    int i, j;
    if (btn_init(b, BIG_IN, BIG_OUT, BIG_HID, 32, 0.5, 7u) != 0) return -1;
    ins[0] = PT(PORT_ONEHOT, 8, 1, "a");
    ins[1] = PT(PORT_BINARY_MSB, 8, 1, "b");
    out = PT(PORT_BINARY_MSB, 8, 1, "y");
    if (btn_set_io_ports(b, ins, 2, &out, 1) != 0) return -1;
    memset(big_in, 0, sizeof big_in);
    for (i = 0; i < BIG_EX; ++i) {
        big_in[i][i % 8] = 1.0;                       /* onehot slice */
        for (j = 0; j < 8; ++j) {
            big_in[i][8 + j] = (double)((i >> (j % 7)) & 1);
            big_out[i][j] = (double)(((i * 31) >> j) & 1);
        }
    }
    return contract_init_borrowed(c, "big_unit", b,
                                  &big_in[0][0], &big_out[0][0], BIG_EX);
}

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    if (f == NULL) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
}

static int flip_byte(const char *path, long offset) {
    FILE *f = fopen(path, "rb+");
    int ch;
    if (f == NULL) return -1;
    if (fseek(f, offset, SEEK_SET) != 0) { fclose(f); return -1; }
    ch = fgetc(f);
    if (ch == EOF) { fclose(f); return -1; }
    fseek(f, offset, SEEK_SET);
    fputc(ch ^ 0x40, f);
    fclose(f);
    return 0;
}

int main(void) {
    BinaryTransformNetwork decoder;
    Contract c;

    printf("unit files (weights + contract, one sealed artifact):\n");

    if (make_decoder(&decoder) != 0) {
        printf("  FAIL fixture training\n");
        return 1;
    }
    if (contract_init_borrowed(&c, "decoder", &decoder,
                               &g_inputs[0][0], &g_targets[0][0], 4) != 0) {
        printf("  FAIL contract init\n");
        return 1;
    }

    /* ---- round-trip identity ---- */
    CHECK(unit_save(&decoder, &c, "unit_test.cnu") == 0, "unit saves");
    {
        BinaryTransformNetwork lb;
        Contract lc;
        CHECK(unit_load(&lb, &lc, "unit_test.cnu") == 0, "unit loads");
        CHECK(contract_btn_digest(&lb) == contract_btn_digest(&decoder),
              "loaded weights are BIT-IDENTICAL (btn digest equal)");
        CHECK(contract_content_digest(&lc) == contract_content_digest(&c),
              "loaded contract is BIT-IDENTICAL (content digest equal)");
        CHECK(lc.seal_verified == 1, "loaded contract is marked sealed");
        CHECK(btn_certify(&lb, &lc, NULL) == 0,
              "the loaded unit certifies (weights satisfy their own spec)");
        contract_free(&lc);
        btn_free(&lb);
    }

    /* ---- security: seal refuses any modification ---- */
    {
        BinaryTransformNetwork lb;
        Contract lc;
        long sz = file_size("unit_test.cnu");
        CHECK(sz > 0, "unit file present");
        CHECK(flip_byte("unit_test.cnu", sz / 2) == 0, "byte flipped mid-file");
        CHECK(unit_load(&lb, &lc, "unit_test.cnu") != 0,
              "tampered unit REFUSED (seal checked before parsing)");
        CHECK(unit_save(&decoder, &c, "unit_test.cnu") == 0, "re-save clean");
        /* truncation */
        {
            FILE *f = fopen("unit_test.cnu", "rb");
            unsigned char buf[65536];
            size_t n = fread(buf, 1, sizeof buf, f);
            fclose(f);
            f = fopen("unit_test.cnu", "wb");
            fwrite(buf, 1, n - 9, f);
            fclose(f);
            CHECK(unit_load(&lb, &lc, "unit_test.cnu") != 0, "truncated unit REFUSED");
        }
        CHECK(unit_save(&decoder, &c, "unit_test.cnu") == 0, "re-save clean again");
        CHECK(flip_byte("unit_test.cnu", 0) == 0, "magic corrupted");
        CHECK(unit_load(&lb, &lc, "unit_test.cnu") != 0, "bad magic REFUSED");
    }
    remove("unit_test.cnu");

    /* ---- incoherent saves refused ---- */
    {
        Contract wrong = c;
        wrong.input_ports[0].tag[0] = 'X';  /* signature mismatch vs btn */
        CHECK(unit_save(&decoder, &wrong, "unit_bad.cnu") != 0,
              "signature mismatch between btn and contract refused");
    }
    {
        double saved = g_targets[0][0];
        g_targets[0][0] = 0.5;              /* non-canonical exemplar */
        CHECK(unit_save(&decoder, &c, "unit_bad.cnu") != 0,
              "non-canonical exemplar value refused (bit-packing stays sound)");
        g_targets[0][0] = saved;
    }
    remove("unit_bad.cnu");

    /* ---- size vs the legacy two-file text pair + multi-port round-trip ---- */
    {
        BinaryTransformNetwork big, lb;
        Contract bc, lc;
        long unit_sz, pair_sz;
        CHECK(make_big(&big, &bc) == 0, "big multi-port fixture builds");
        CHECK(unit_save(&big, &bc, "unit_big.cnu") == 0, "big unit saves");
        CHECK(btn_save(&big, "unit_big.btn") == 0 &&
              contract_save(&bc, "unit_big.contract") == 0, "legacy pair saves");
        unit_sz = file_size("unit_big.cnu");
        pair_sz = file_size("unit_big.btn") + file_size("unit_big.contract");
        printf("  info unit %ld bytes vs legacy pair %ld bytes (%.1fx smaller)\n",
               unit_sz, pair_sz, (double)pair_sz / (double)unit_sz);
        CHECK(unit_sz > 0 && pair_sz > 0 && unit_sz < pair_sz,
              "one unit file is smaller than the two text files");

        CHECK(unit_load(&lb, &lc, "unit_big.cnu") == 0, "big unit loads");
        CHECK(contract_btn_digest(&lb) == contract_btn_digest(&big) &&
              contract_content_digest(&lc) == contract_content_digest(&bc),
              "multi-port unit round-trips bit-identically");
        CHECK(lb.input_port_count == 2 &&
              strcmp(lb.input_ports[0].tag, "a") == 0 &&
              strcmp(lb.input_ports[1].tag, "b") == 0 &&
              strcmp(lb.output_ports[0].tag, "y") == 0,
              "port tags round-trip");
        contract_free(&lc);
        btn_free(&lb);
        btn_free(&big);
        remove("unit_big.cnu");
        remove("unit_big.btn");
        remove("unit_big.contract");
    }

    /* ---- registry_save now writes ONE file per primitive ---- */
    {
        PrimitiveRegistry reg;
        BinaryTransformNetwork lb;
        Contract lc;
        registry_init(&reg);
        CHECK(registry_add(&reg, &decoder, "unitprim") == 0, "registry add");
        CHECK(registry_save(&reg, "") == 0, "registry_save ok");
        CHECK(file_size("unitprim.cnu") > 0, "one .cnu unit written");
        CHECK(file_size("unitprim.btn") < 0 && file_size("unitprim.contract") < 0,
              "no legacy .btn/.contract pair anymore");
        CHECK(file_size("unitprim.stats") > 0,
              ".stats sidecar kept (evidence has its own lifecycle)");
        CHECK(unit_load(&lb, &lc, "unitprim.cnu") == 0, "saved unit loads");
        CHECK(contract_btn_digest(&lb) == contract_btn_digest(&decoder),
              "registry-saved unit carries the exact weights");
        CHECK(btn_certify(&lb, &lc, NULL) == 0,
              "registry-saved unit certifies against its embedded contract");
        contract_free(&lc);
        btn_free(&lb);
        registry_free(&reg);
        remove("unitprim.cnu");
        remove("unitprim.stats");
    }

    printf(failures == 0 ? "\nAll unit-file tests passed.\n"
                         : "\n%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
