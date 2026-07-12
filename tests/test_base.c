/* Unified base (CNB1) — vertical slice gate.
   One sealed container: units + tags + stats + oracle descriptors.
   Fixture: the hex_value / increment pair (same as test_acquire). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/base.h"
#include "../include/contract/unit.h"

static int checks_run = 0;

static void check(int cond, const char *what) {
    ++checks_run;
    if (!cond) {
        printf("FAIL: %s\n", what);
        exit(1);
    }
    printf("  ok: %s\n", what);
}

static Port make_port(PortFamily fam, size_t w, size_t c, const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = fam;
    p.field_width = w;
    p.field_count = c;
    if (port_set_tag(&p, tag) != 0) { printf("FAIL: bad tag %s\n", tag); exit(1); }
    return p;
}

static void nibble_bits(unsigned v, double *out) {
    out[0] = (v >> 3) & 1u; out[1] = (v >> 2) & 1u;
    out[2] = (v >> 1) & 1u; out[3] = v & 1u;
}

static unsigned bits_nibble(const double *in) {
    return (unsigned)(((in[0] > 0.5) << 3) | ((in[1] > 0.5) << 2) |
                      ((in[2] > 0.5) << 1) | (in[3] > 0.5));
}

/* Train hex_value: ONEHOT16 "hex_sym" -> BINARY_MSB4 "nibble".
   Caller owns the returned heap BTN; *c_out borrows the static tables. */
static double hexv_inputs[16 * 16];
static double hexv_targets[16 * 4];

static BinaryTransformNetwork *make_hex_value(Contract *c_out) {
    BinaryTransformNetwork *btn = calloc(1, sizeof *btn);
    Port hex = make_port(PORT_ONEHOT, 16, 1, "hex_sym");
    Port nib = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
    unsigned v;
    memset(hexv_inputs, 0, sizeof hexv_inputs);
    for (v = 0; v < 16; ++v) {
        hexv_inputs[v * 16 + v] = 1.0;
        nibble_bits(v, hexv_targets + v * 4);
    }
    if (!btn || btn_init(btn, 16, 4, 8, 64, 0.5, 7) != 0 ||
        btn_set_ports(btn, hex, nib) != 0) { free(btn); return NULL; }
    btn_train_dynamic(btn, hexv_inputs, hexv_targets, 16, 4000, 200, 1e-4, 1e-6);
    if (contract_init_borrowed(c_out, "hex_value", btn,
                               hexv_inputs, hexv_targets, 16) != 0) {
        btn_free(btn); free(btn); return NULL;
    }
    return btn;
}

/* Train increment: BINARY_MSB4 "nibble" -> BINARY_MSB4 "nibble_next". */
static double inc_inputs[16 * 4];
static double inc_targets[16 * 4];

static BinaryTransformNetwork *make_increment(Contract *c_out) {
    BinaryTransformNetwork *btn = calloc(1, sizeof *btn);
    Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
    Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
    unsigned v;
    for (v = 0; v < 16; ++v) {
        nibble_bits(v, inc_inputs + v * 4);
        nibble_bits((v + 1u) & 0xFu, inc_targets + v * 4);
    }
    if (!btn || btn_init(btn, 4, 4, 8, 64, 0.5, 7) != 0 ||
        btn_set_ports(btn, nib, nibn) != 0) { free(btn); return NULL; }
    btn_train_dynamic(btn, inc_inputs, inc_targets, 16, 4000, 200, 1e-4, 1e-6);
    if (contract_init_borrowed(c_out, "increment", btn,
                               inc_inputs, inc_targets, 16) != 0) {
        btn_free(btn); free(btn); return NULL;
    }
    return btn;
}

/* Reference implementation of increment, for oracle binding + acquisition. */
static int oracle_increment(const double *in, double *out, void *ctx) {
    (void)ctx;
    nibble_bits((bits_nibble(in) + 1u) & 0xFu, out);
    return 0;
}

/* Resolver: binds builtin descriptors only; anything else stays unbound. */
static CnetOracleFn resolve_builtin(const char *name, const char *kind,
                                    void *rctx) {
    (void)rctx;
    if (strcmp(kind, "builtin") == 0 && strcmp(name, "increment_ref") == 0)
        return oracle_increment;
    return NULL;
}

/* Read a whole file (for byte-identity and tamper tests). */
static unsigned char *slurp(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *len_out = (size_t)n;
    return buf;
}

int main(void) {
    BinaryTransformNetwork *hexv, *inc;
    Contract hexv_c, inc_c;

    printf("== base: unified CNB1 container ==\n");

    hexv = make_hex_value(&hexv_c);
    inc = make_increment(&inc_c);
    if (!hexv || !inc) { printf("FAIL: fixture training\n"); return 1; }

    printf("[1] container round-trip + dedup/refusal\n");
    {
        CnetBase b, b2;
        BinaryTransformNetwork rbtn;
        Contract rc;
        int reused = -1;

        cnb_init(&b);
        check(cnb_add_unit(&b, hexv, &hexv_c, &reused) == 0 && reused == 0,
              "first add ingests");
        check(cnb_add_unit(&b, inc, &inc_c, &reused) == 0 && reused == 0,
              "second unit ingests");
        check(b.unit_count == 2 && b.blob_count == 2, "two units, two blobs");

        check(cnb_add_unit(&b, hexv, &hexv_c, &reused) == 0 && reused == 1 &&
              b.unit_count == 2 && b.blob_count == 2,
              "identical re-add is idempotent (reused=1)");

        /* same name, different content: refused */
        {
            double w0 = inc->hidden_output_weights[0];
            inc->hidden_output_weights[0] = -w0;
            check(cnb_add_unit(&b, inc, &inc_c, &reused) == -1,
                  "same name + different weights refused");
            inc->hidden_output_weights[0] = w0;
        }

        check(cnb_save(&b, "test_base.cnb") == 0, "base saves");
        cnb_init(&b2);
        check(cnb_load(&b2, "test_base.cnb") == 0, "base loads");
        check(b2.unit_count == 2 && b2.blob_count == 2 && b2.tag_count == 3,
              "counts survive (2 units, 3 tags)");

        check(cnb_get_unit(&b2, "increment", &rbtn, &rc) == 0 &&
              rc.seal_verified == 1,
              "unit extracts with its CNU1 seal verified");
        check(contract_btn_digest(&rbtn) == contract_btn_digest(inc),
              "extracted behavior digest matches the original");
        check(btn_certify(&rbtn, &rc, NULL) == 0,
              "extracted unit certifies its own contract");
        btn_free(&rbtn);
        contract_free(&rc);

        cnb_free(&b);
        cnb_free(&b2);
    }

    printf("[2] tamper refusal\n");
    {
        CnetBase b;
        unsigned char *bytes;
        size_t len;
        FILE *f;

        cnb_init(&b);
        check(cnb_load(&b, "test_base.cnb") == 0, "pristine file loads");

        bytes = slurp("test_base.cnb", &len);
        check(bytes != NULL, "file readable");
        bytes[len / 2] ^= 0x01;   /* flip one payload bit */
        f = fopen("test_base_tampered.cnb", "wb");
        fwrite(bytes, 1, len, f);
        fclose(f);
        free(bytes);

        check(cnb_load(&b, "test_base_tampered.cnb") == -1,
              "tampered file refused");
        check(b.unit_count == 2, "base untouched by refused load");

        cnb_free(&b);
        remove("test_base_tampered.cnb");
    }

    printf("[3] determinism: save -> load -> save byte-identical\n");
    {
        CnetBase b;
        unsigned char *a, *c;
        size_t alen, clen;

        cnb_init(&b);
        check(cnb_load(&b, "test_base.cnb") == 0, "loads");
        check(cnb_save(&b, "test_base_resave.cnb") == 0, "resaves");
        a = slurp("test_base.cnb", &alen);
        c = slurp("test_base_resave.cnb", &clen);
        check(a && c && alen == clen && memcmp(a, c, alen) == 0,
              "byte-identical resave");
        free(a); free(c);
        cnb_free(&b);
        remove("test_base_resave.cnb");
    }

    printf("[4] tag governance\n");
    {
        CnetBase b;
        char existing[PORT_TAG_MAX];

        cnb_init(&b);
        check(cnb_tag_mint(&b, "nibble", "hand") == 0, "tag mints");
        check(cnb_tag_mint(&b, "nibble", "other") == 1,
              "exact re-mint is idempotent (returns 1)");
        check(b.tag_count == 1, "no duplicate entry");

        check(cnb_tag_mint(&b, "Nibble", "x") == -1, "case near-miss refused");
        check(cnb_tag_mint(&b, "nibbel", "x") == -1, "edit-1 near-miss refused");
        check(cnb_tag_mint(&b, "nibble_", "x") == -1,
              "underscore-stripped near-miss refused");
        check(cnb_tag_near_miss(&b, "nibbel", existing, sizeof existing) == 1 &&
              strcmp(existing, "nibble") == 0,
              "near_miss names the colliding tag");

        check(cnb_tag_mint(&b, "nibble_next", "hand") == 0,
              "genuinely different tag (distance 5) mints fine");
        check(cnb_tag_mint(&b, "bad tag!", "x") == -1, "non-atom refused");

        /* unit add auto-mints with owner = unit name */
        check(cnb_add_unit(&b, hexv, &hexv_c, NULL) == 0,
              "unit add auto-mints its tags");
        check(cnb_tag_lookup(&b, "hex_sym") >= 0, "hex_sym minted by add");
        check(strcmp(b.tags[cnb_tag_lookup(&b, "hex_sym")].owner,
                     "hex_value") == 0,
              "provenance records the minting unit");

        /* a unit whose tag near-misses an existing one is REFUSED whole */
        {
            BinaryTransformNetwork bad;
            Contract bad_c;
            Port in = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
            Port out = make_port(PORT_BINARY_MSB, 4, 1, "nibblenext"); /* != nibble_next after fold */
            size_t tags_before = b.tag_count;
            memset(&bad, 0, sizeof bad);
            check(btn_init(&bad, 4, 4, 8, 64, 0.5, 3) == 0 &&
                  btn_set_ports(&bad, in, out) == 0, "drifted unit inits");
            btn_train_dynamic(&bad, inc_inputs, inc_targets, 16, 4000, 200,
                              1e-4, 1e-6);
            {
                Contract tmpc;
                check(contract_init_borrowed(&tmpc, "drifted", &bad,
                                             inc_inputs, inc_targets, 16) == 0,
                      "drifted contract builds");
                bad_c = tmpc;
            }
            check(cnb_add_unit(&b, &bad, &bad_c, NULL) == -1,
                  "unit with near-miss tag refused (nibblenext vs nibble_next)");
            check(b.tag_count == tags_before && b.unit_count == 1,
                  "refusal is all-or-nothing: no tags minted, no unit added");
            btn_free(&bad);
        }

        cnb_tag_audit(&b, stdout);
        cnb_free(&b);
    }

    printf("[5] digest-bound stats\n");
    {
        CnetBase b;
        cnb_init(&b);
        check(cnb_add_unit(&b, inc, &inc_c, NULL) == 0, "unit in base");

        inc->output_successes = 42;
        inc->output_failures = 3;
        check(cnb_put_stats(&b, "increment", inc) == 0, "stats recorded");

        inc->output_successes = 0;
        inc->output_failures = 0;
        check(cnb_apply_stats(&b, "increment", inc) == 0 &&
              inc->output_successes == 42 && inc->output_failures == 3,
              "stats restored on digest match");

        /* 'retrain': change a weight -> digest changes -> evidence is stale */
        {
            double w0 = inc->hidden_output_weights[0];
            inc->hidden_output_weights[0] = -w0;
            inc->output_successes = 0;
            inc->output_failures = 0;
            check(cnb_apply_stats(&b, "increment", inc) == -1 &&
                  inc->output_successes == 0 && inc->output_failures == 0,
                  "stale evidence refused, counters untouched");
            inc->hidden_output_weights[0] = w0;
        }
        check(cnb_apply_stats(&b, "no_such_unit", inc) == -1,
              "unknown unit refused");
        inc->output_successes = 0;
        inc->output_failures = 0;
        cnb_free(&b);
    }

    printf("[6] registry bridge (certify-on-load + composition)\n");
    {
        CnetBase b;
        PrimitiveRegistry reg;
        RoutePlan plan;
        size_t skipped = 99;
        Port hex  = make_port(PORT_ONEHOT, 16, 1, "hex_sym");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        double in[16], out[4];
        unsigned v;

        cnb_init(&b);
        check(cnb_add_unit(&b, hexv, &hexv_c, NULL) == 0 &&
              cnb_add_unit(&b, inc, &inc_c, NULL) == 0,
              "both fixture units in the base");

        /* exercise the read-only cross-unit overlap analyzer (mining-prefetch style) */
        cnb_analyze_cross_unit_overlap(&b, NULL);

        registry_init(&reg);
        check(cnb_load_registry(&b, &reg, &skipped) == 0 && skipped == 0,
              "registry loads with zero skips");
        check(reg.count == 2 && reg.entries[0].certified == 1 &&
              reg.entries[1].certified == 1 &&
              reg.entries[0].state == PRIM_FROZEN &&
              reg.entries[1].state == PRIM_FROZEN,
              "both units re-certified FROZEN (trust replayed, not stored)");

        reg.require_certified = 1;
        check(route_plan(&reg, hex, nibn, &plan) == 0 && plan.length == 2,
              "router composes the 2-hop certified chain from the base");
        plan.strict = 1;
        for (v = 0; v < 16; ++v) {
            memset(in, 0, sizeof in);
            in[v] = 1.0;
            check(route_execute(&plan, in, 16, out, 4) == 0 &&
                  bits_nibble(out) == ((v + 1u) & 0xFu),
                  "strict execution correct");
        }

        registry_free(&reg);
        cnb_free(&b);   /* frees the loaded BTNs AFTER the registry is done */
    }

    printf("[7] oracle descriptors\n");
    {
        CnetBase b, b2;
        OracleRegistry orc;
        size_t unbound = 99;
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        CnetOracleIdentity identity;

        cnb_init(&b);
        memset(&identity, 0, sizeof identity);
        identity.abi_version = CNET_ORACLE_ABI_VERSION;
        identity.struct_size = (uint32_t)sizeof identity;
        identity.artifact_digest = 0x11112222u;
        identity.contract_digest = 0x33334444u;
        identity.config_digest = 0x55556666u;
        identity.retrieval_snapshot_digest = 0x77778888u;
        identity.toolchain_digest = 0x9999aaaau;
        {
            int z;
            for (z = 0; z < 32; ++z) identity.artifact_sha256[z] = (unsigned char)(z + 1);
        }
        check(cnb_add_oracle_desc_v2(&b, "increment_ref", "builtin", nib, nibn,
                                     &identity) == 0,
              "evidence-carrying descriptor added");
        check(cnb_add_oracle_desc(&b, "future_llm", "cce_model", nib, nibn) == 0,
              "second descriptor added");
        check(cnb_add_oracle_desc(&b, "increment_ref", "builtin", nib, nibn) == -1,
              "duplicate descriptor name refused");

        check(cnb_save(&b, "test_base_orc.cnb") == 0, "saves with oracles");
        cnb_init(&b2);
        check(cnb_load(&b2, "test_base_orc.cnb") == 0 && b2.oracle_count == 2,
              "descriptors survive the round-trip");
        check(strcmp(b2.oracles[1].kind, "cce_model") == 0 &&
               strcmp(b2.oracles[0].input_port.tag, "nibble") == 0,
              "kind + port tags survive");
        check(b2.oracles[0].identity.artifact_digest == identity.artifact_digest &&
              b2.oracles[0].identity.retrieval_snapshot_digest ==
                  identity.retrieval_snapshot_digest &&
              b2.oracles[0].behavior_digest == cnet_oracle_identity_digest(&identity),
              "CNB2 preserves complete Oracle identity");
        check(memcmp(b2.oracles[0].identity.artifact_sha256,
                     identity.artifact_sha256, 32) == 0,
              "CNB4 preserves the full 256-bit artifact hash");
        /* the full hash is NOT folded into the behavior digest, so an old base
           (sha256 all-zero) and a new one with the same 64-bit fields share a
           behavior digest — the digest stays a stable index across the bump */
        {
            CnetOracleIdentity old_id = identity;
            memset(old_id.artifact_sha256, 0, 32);
            old_id.struct_size = (uint32_t)offsetof(CnetOracleIdentity,
                                                    artifact_sha256);
            check(cnet_oracle_identity_digest(&old_id) ==
                  cnet_oracle_identity_digest(&identity),
                  "full hash is not in the behavior digest (v-agnostic index)");
        }

        memset(&orc, 0, sizeof orc);
        check(cnb_bind_oracles(&b2, &orc, resolve_builtin, NULL, &unbound) == 0,
              "bind runs");
        check(orc.count == 1 && unbound == 1 &&
              strcmp(orc.entries[0].name, "increment_ref") == 0 &&
              orc.entries[0].behavior_digest == b2.oracles[0].behavior_digest,
              "builtin bound with persisted identity; cce_model remains unbound");

        cnb_free(&b);
        cnb_free(&b2);
        remove("test_base_orc.cnb");
    }

    printf("[7b] direct unit -> descriptor provenance relation\n");
    {
        CnetBase b, b2;
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        CnetOracleIdentity identity;

        cnb_init(&b);
        memset(&identity, 0, sizeof identity);
        identity.abi_version = CNET_ORACLE_ABI_VERSION;
        identity.struct_size = (uint32_t)sizeof identity;
        identity.artifact_digest = 0x11112222u;
        identity.contract_digest = 0x33334444u;
        check(cnb_add_unit(&b, inc, &inc_c, NULL) == 0 &&
              cnb_add_oracle_desc_v2(&b, "increment_ref", "builtin",
                                     nib, nibn, &identity) == 0,
              "unit + teacher descriptor in one base");
        check(cnb_unit_provenance(&b, "increment") != NULL &&
              cnb_unit_provenance(&b, "increment")[0] == '\0',
              "a fresh unit has no recorded teacher");
        check(cnb_set_unit_provenance(&b, "increment", "increment_ref") == 0 &&
              strcmp(cnb_unit_provenance(&b, "increment"),
                     "increment_ref") == 0,
              "unit points at its descriptor directly");
        check(cnb_set_unit_provenance(&b, "increment", "increment_ref") == 0,
              "same relation re-set is idempotent");
        check(cnb_set_unit_provenance(&b, "increment", "other_ref") == -1 &&
              cnb_set_unit_provenance(&b, "no_such_unit",
                                      "increment_ref") == -1,
              "re-pointing and unknown endpoints are refused");
        check(cnb_save(&b, "test_base_prov.cnb") == 0, "saves the relation");
        cnb_init(&b2);
        check(cnb_load(&b2, "test_base_prov.cnb") == 0 &&
              cnb_unit_provenance(&b2, "increment") != NULL &&
              strcmp(cnb_unit_provenance(&b2, "increment"),
                     "increment_ref") == 0,
              "relation survives the round-trip");
        cnb_free(&b2);
        /* a relation naming a descriptor absent from the same container is
           corruption: refused whole at load */
        snprintf(b.units[0].provenance, CNB_NAME_MAX, "vanished_ref");
        check(cnb_save(&b, "test_base_prov.cnb") == 0, "corrupt fixture saves");
        cnb_init(&b2);
        check(cnb_load(&b2, "test_base_prov.cnb") == -1,
              "dangling unit -> descriptor relation refused at load");
        cnb_free(&b);
        cnb_free(&b2);
        remove("test_base_prov.cnb");
    }

    printf("[8] migration from loose .cnu files\n");
    {
        CnetBase b;
        BinaryTransformNetwork rbtn;
        Contract rc;
        int reused = -1;

        check(unit_save(hexv, &hexv_c, "mig_hex_value.cnu") == 0 &&
              unit_save(inc, &inc_c, "mig_increment.cnu") == 0,
              "loose .cnu files written (the old sprawl)");

        cnb_init(&b);
        check(cnb_ingest_cnu_file(&b, "mig_hex_value.cnu", &reused) == 0 &&
              reused == 0 &&
              cnb_ingest_cnu_file(&b, "mig_increment.cnu", &reused) == 0 &&
              reused == 0,
              "both files ingested");
        check(cnb_ingest_cnu_file(&b, "mig_hex_value.cnu", &reused) == 0 &&
              reused == 1,
              "re-ingest is idempotent");
        remove("mig_hex_value.cnu");
        remove("mig_increment.cnu");

        /* registry-from-base is behavior-identical to the original units */
        check(cnb_get_unit(&b, "hex_value", &rbtn, &rc) == 0 &&
              contract_btn_digest(&rbtn) == contract_btn_digest(hexv),
              "migrated hex_value behavior-digest-identical");
        btn_free(&rbtn);
        contract_free(&rc);
        check(cnb_get_unit(&b, "increment", &rbtn, &rc) == 0 &&
              contract_btn_digest(&rbtn) == contract_btn_digest(inc),
              "migrated increment behavior-digest-identical");
        btn_free(&rbtn);
        contract_free(&rc);

        cnb_free(&b);
    }

    printf("[9] acquisition seals into the base\n");
    {
        CnetBase b;
        PrimitiveRegistry reg;
        AcquireLedger led;
        OracleRegistry orc;
        AcquireConfig cfg;
        AcquireReport rep;
        RoutePlan plan;
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        FILE *probe;

        /* success path: drain seals into the base, no loose files */
        cnb_init(&b);
        registry_init(&reg);
        acquire_ledger_init(&led);
        memset(&orc, 0, sizeof orc);
        acquire_config_defaults(&cfg);
        cfg.unit_dir = ".";     /* would sprawl — base must override it */
        cfg.base = &b;
        acquire_oracle_register(&orc, "increment_ref", nib, nibn,
                                oracle_increment, NULL);
        acquire_note_no_plan(&led, nib, nibn);
        memset(&rep, 0, sizeof rep);
        check(acquire_drain(&reg, &led, &orc, &cfg, &rep) == 0 &&
              rep.closed == 1,
              "drain closes the gap with a base target");
        probe = fopen("./acq_nibble_next.cnu", "r");
        check(probe == NULL, "no loose .cnu written (sprawl killed)");
        if (probe) fclose(probe);
        check(cnb_has_unit(&b, "acq_nibble_next") == 1,
              "acquired unit lives in the base");
        check(cnb_tag_lookup(&b, "nibble_next") >= 0 &&
              strcmp(b.tags[cnb_tag_lookup(&b, "nibble_next")].owner,
                     "acq_nibble_next") == 0,
              "tags minted with acquisition provenance");
        check(route_plan(&reg, nib, nibn, &plan) == 0,
              "router plans through the base-sealed unit");
        acquire_ledger_free(&led);
        registry_free(&reg);
        cnb_free(&b);

        /* collision path: a near-miss tag in the base DEFERs the acquisition
           and leaves base + registry untouched */
        cnb_init(&b);
        registry_init(&reg);
        acquire_ledger_init(&led);
        check(cnb_tag_mint(&b, "nibble_nxt", "hand") == 0,
              "pre-existing drifted tag minted by hand");
        cfg.base = &b;
        acquire_note_no_plan(&led, nib, nibn);   /* goal tag nibble_next */
        memset(&rep, 0, sizeof rep);
        check(acquire_drain(&reg, &led, &orc, &cfg, &rep) == 0 &&
              rep.deferred == 1 &&
              strcmp(rep.last_defer_reason, "tag_collision") == 0,
              "near-miss tag DEFERs with reason tag_collision");
        check(b.unit_count == 0 && b.tag_count == 1 && reg.count == 0,
              "base and registry untouched by the refused acquisition");
        acquire_ledger_free(&led);
        registry_free(&reg);
        cnb_free(&b);
    }

    remove("test_base.cnb");
    btn_free(hexv); free(hexv);
    btn_free(inc); free(inc);

    printf("checks run: %d\n", checks_run);
    printf("ALL BASE TESTS PASSED\n");
    return 0;
}
