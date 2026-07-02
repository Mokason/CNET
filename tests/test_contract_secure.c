/*
 * Contract logic: efficiency + security gates.
 *
 *  - content digests: deterministic; behavior-only (evidence counters and
 *    learning_rate excluded; any weight change included)
 *  - certification cache: repeat certify of unchanged content is a hit with
 *    the identical verdict + report; any weight/exemplar change misses
 *  - sealed contract files: v2 round-trip verifies; a tampered exemplar or
 *    seal is REFUSED even when every value stays canonical; v1 still loads
 *  - certification binding: a certified entry whose weights mutate after
 *    certification is demoted by registry_audit_certified
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* decoder: ONEHOT4 "sym" -> BINARY_MSB2 "val", i -> i (from test_certify). */
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

/* Replace a token in a text file (first occurrence). Returns 0 on success. */
static int file_patch(const char *path, const char *from, const char *to) {
    FILE *f = fopen(path, "rb");
    char buf[65536];
    size_t n;
    char *hit;
    if (f == NULL) return -1;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    hit = strstr(buf, from);
    if (hit == NULL || strlen(from) != strlen(to)) return -1;
    memcpy(hit, to, strlen(to));
    f = fopen(path, "wb");
    if (f == NULL) return -1;
    fwrite(buf, 1, n, f);
    fclose(f);
    return 0;
}

int main(void) {
    BinaryTransformNetwork decoder;
    Contract c;
    CertifyReport rep_a, rep_b;
    size_t hits, misses;
    unsigned long long d1, d2;

    printf("contract security + efficiency:\n");

    if (make_decoder(&decoder) != 0) {
        printf("  FAIL fixture training\n");
        return 1;
    }
    if (contract_init_borrowed(&c, "decoder", &decoder,
                               &g_inputs[0][0], &g_targets[0][0], 4) != 0) {
        printf("  FAIL contract init\n");
        return 1;
    }

    /* ---- digests ---- */
    d1 = contract_btn_digest(&decoder);
    d2 = contract_btn_digest(&decoder);
    CHECK(d1 != 0 && d1 == d2, "btn digest is deterministic");

    decoder.output_successes += 7;
    decoder.output_failures += 3;
    CHECK(contract_btn_digest(&decoder) == d1,
          "evidence counters do NOT change the digest (accrual keeps certs valid)");
    decoder.learning_rate *= 2.0;
    CHECK(contract_btn_digest(&decoder) == d1,
          "learning_rate does NOT change the digest (training metadata)");
    {
        double saved = decoder.hidden_bias[0];
        decoder.hidden_bias[0] += 1e-9;
        CHECK(contract_btn_digest(&decoder) != d1,
              "any weight change DOES change the digest");
        decoder.hidden_bias[0] = saved;  /* bit-exact restore */
        CHECK(contract_btn_digest(&decoder) == d1, "restoring the weight restores the digest");
    }

    d2 = contract_content_digest(&c);
    CHECK(d2 != 0 && d2 == contract_content_digest(&c), "contract digest deterministic");

    /* ---- certification cache ---- */
    contract_cache_reset();
    CHECK(btn_certify(&decoder, &c, &rep_a) == 0, "decoder certifies");
    contract_cache_stats(&hits, &misses);
    CHECK(hits == 0 && misses == 1, "first certify is a miss");

    CHECK(btn_certify(&decoder, &c, &rep_b) == 0, "second certify agrees");
    contract_cache_stats(&hits, &misses);
    CHECK(hits == 1 && misses == 1, "second certify is a cache hit (no replay)");
    CHECK(rep_a.exemplars == rep_b.exemplars && rep_a.passed == rep_b.passed &&
          rep_a.failed == rep_b.failed && rep_a.min_margin == rep_b.min_margin,
          "cached report is identical to the replayed report");

    {
        double saved = decoder.hidden_bias[0];
        decoder.hidden_bias[0] += 0.5;   /* mutate: must force a fresh replay */
        (void)btn_certify(&decoder, &c, NULL);
        contract_cache_stats(&hits, &misses);
        CHECK(misses == 2, "weight mutation invalidates the cache (fresh replay)");
        decoder.hidden_bias[0] = saved;  /* bit-exact restore */
    }

    CHECK(btn_certify(&decoder, &c, NULL) == 0, "restored weights certify again");
    contract_cache_stats(&hits, &misses);
    CHECK(hits == 2, "restored content hits the original cache entry");

    /* efficiency: informational timing over repeated certifies */
    {
        clock_t t0 = clock();
        int k;
        for (k = 0; k < 2000; ++k) (void)btn_certify(&decoder, &c, NULL);
        double ms = (clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
        contract_cache_stats(&hits, &misses);
        printf("  info 2000 cached certifies: %.1f ms (hits=%lu misses=%lu)\n",
               ms, (unsigned long)hits, (unsigned long)misses);
        CHECK(misses == 2, "repeat certifies never replay");
    }

    /* ---- sealed files ---- */
    CHECK(contract_save(&c, "secure_test.contract") == 0, "v2 save ok");
    {
        Contract lc;
        CHECK(contract_load(&lc, "secure_test.contract") == 0, "v2 load ok");
        CHECK(lc.seal_verified == 1, "seal verified on clean round-trip");
        CHECK(contract_content_digest(&lc) == contract_content_digest(&c),
              "loaded contract has the identical content digest");
        contract_free(&lc);
    }
    {
        /* tamper with an exemplar: first target row "0 0" -> "0 1" stays
           canonical per-value, but the seal must catch it */
        Contract lc;
        CHECK(file_patch("secure_test.contract", "1 0 0 0 0 0", "1 0 0 0 0 1") == 0,
              "tamper patch applied");
        CHECK(contract_load(&lc, "secure_test.contract") != 0,
              "tampered exemplar REFUSED by the seal");
    }
    CHECK(contract_save(&c, "secure_test.contract") == 0, "re-save clean");
    {
        /* corrupt the seal itself */
        Contract lc;
        char from[32], to[32];
        unsigned long long dg = contract_content_digest(&c);
        snprintf(from, sizeof from, "%016llx", dg);
        snprintf(to, sizeof to, "%016llx", dg ^ 0xf);
        CHECK(file_patch("secure_test.contract", from, to) == 0, "seal corrupted");
        CHECK(contract_load(&lc, "secure_test.contract") != 0, "bad seal REFUSED");
    }
    {
        /* legacy v1 (no seal) still loads, unsealed */
        Contract lc;
        CHECK(contract_save(&c, "secure_test.contract") == 0, "re-save clean");
        CHECK(file_patch("secure_test.contract", "CNET_CONTRACT 2", "CNET_CONTRACT 1") == 0,
              "downgraded to v1 header");
        CHECK(contract_load(&lc, "secure_test.contract") == 0,
              "legacy v1 file still loads (compat)");
        CHECK(lc.seal_verified == 0, "v1 load is marked unsealed");
        contract_free(&lc);
    }
    remove("secure_test.contract");

    /* ---- certification binding + audit ---- */
    {
        PrimitiveRegistry reg;
        registry_init(&reg);
        CHECK(registry_add_certified(&reg, &decoder, "decoder", &c) == 0,
              "registry_add_certified grants");
        CHECK(reg.entries[0].certified == 1 &&
              reg.entries[0].cert_btn_digest == contract_btn_digest(&decoder),
              "certificate bound to the weight digest");
        CHECK(registry_audit_certified(&reg) == 0, "clean audit demotes nothing");

        decoder.hidden_bias[0] += 0.25;  /* mutate AFTER certification */
        CHECK(registry_audit_certified(&reg) == 1,
              "mutated-after-certify entry is demoted");
        CHECK(reg.entries[0].certified == 0 && reg.entries[0].state == PRIM_RESET,
              "demotion: certified cleared, state RESET (heal is the only way back)");
        decoder.hidden_bias[0] -= 0.25;
        CHECK(registry_audit_certified(&reg) == 0,
              "audit is idempotent (already-demoted entries stay demoted)");

        /* recovery: a fresh certification grant re-binds the digest */
        CHECK(registry_add_certified(&reg, &decoder, "decoder2", &c) == 0 &&
              reg.entries[1].certified == 1 &&
              reg.entries[1].cert_btn_digest == contract_btn_digest(&decoder),
              "a new passing certification re-binds cleanly");
        registry_free(&reg);
    }

    printf(failures == 0 ? "\nAll contract security tests passed.\n"
                         : "\n%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
