/* SoulHost reopen / remount execution tracer — the durable-identity acid test.
 *
 * Functions cannot be serialized; identity and provenance can. This test seals
 * a base with native BTNs and an evidence-carrying Oracle DESCRIPTOR, closes
 * EVERY original runtime object, reopens through the host, and remounts the
 * Oracle through a caller resolver:
 *   - the resolver's asserted identity is checked against the sealed digest
 *     (mismatch is refused, never silently trusted);
 *   - the adapter is certified against the provenance-linked native unit's OWN
 *     sealed contract (the oracle TAUGHT that unit, so it must reproduce it);
 *   - admission goes through the one specialist door (specialist_admit), so the
 *     live registry entry carries SpecialistKind == ORACLE.
 * It then plans+executes a typed chain whose stages are a native BTN and the
 * remounted Oracle, proves the kind survives in the live registry, and repeats
 * the whole close/reopen/remount cycle. Failure cases (unresolved descriptor,
 * wrong asserted identity, wrong callback behavior, missing provenance) are
 * explicit refusal counts — not silent runtime trust.
 *
 * Exact marker: SPECIALIST_REOPEN_PASS.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/soul_host.h"
#include "../include/base.h"
#include "../include/acquire.h"
#include "../include/specialist.h"
#include "../include/contract/contract.h"
#include "../include/nn.h"

static int failures;

static void check(int ok, const char *name) {
    printf("  %-64s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static Port port(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = 2;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

/* The real reference implementations. swap is the oracle's behaviour; the
   identity variant is a WRONG callback used to prove certification refuses it. */
static int swap_oracle(const double *in, double *out, void *ctx) {
    (void)ctx;
    out[0] = in[1];
    out[1] = in[0];
    return 0;
}
static int identity_oracle(const double *in, double *out, void *ctx) {
    (void)ctx;
    out[0] = in[0];
    out[1] = in[1];
    return 0;
}

/* Seal one trained native BTN unit (in -> out over the 2 canonical exemplars)
   into the base. Returns 0, or -1 if training/certification/seal failed. */
static int seal_native(CnetBase *base, const char *name, Port in, Port out,
                       const double *X, const double *Y) {
    BinaryTransformNetwork b;
    Contract c;
    int reused = 0, rc;
    memset(&b, 0, sizeof b);
    memset(&c, 0, sizeof c);
    if (btn_init(&b, in.field_width, out.field_width, 8, 64, 0.5, 42) != 0)
        return -1;
    if (btn_set_ports(&b, in, out) != 0) { btn_free(&b); return -1; }
    btn_train_dynamic(&b, X, Y, 2, 20000, 200, 1e-4, 1e-6);
    if (contract_init_borrowed(&c, name, &b, X, Y, 2) != 0) {
        btn_free(&b);
        return -1;
    }
    if (btn_certify(&b, &c, NULL) != 0) { contract_free(&c); btn_free(&b); return -1; }
    rc = cnb_add_unit(base, &b, &c, &reused);
    contract_free(&c);
    btn_free(&b);
    return rc;
}

static CnetOracleIdentity make_identity(uint64_t artifact) {
    CnetOracleIdentity id;
    memset(&id, 0, sizeof id);
    id.abi_version = CNET_ORACLE_ABI_VERSION;
    id.struct_size = (uint32_t)sizeof id;
    id.artifact_digest = artifact;
    id.contract_digest = 0x2222u;
    id.config_digest = 0x3333u;
    id.retrieval_snapshot_digest = 0x4444u;
    id.toolchain_digest = 0x5555u;
    return id;
}

enum { MODE_GOOD, MODE_DECLINE, MODE_WRONG_FN, MODE_WRONG_ID };

typedef struct {
    int mode;
    CnetOracleIdentity id_oracle;
    CnetOracleIdentity id_orphan;
} ResolverCtx;

/* One resolver, switched by mode. GOOD binds the true swap callback with the
   matching sealed identity; DECLINE leaves everything unbound; WRONG_FN binds
   the identity callback (behaviourally wrong) with a matching identity;
   WRONG_ID binds the true callback but asserts a corrupted identity. */
static int reopen_resolver(const char *name, const char *kind,
                           SoulOracleBinding *out, void *rctx) {
    ResolverCtx *rc = (ResolverCtx *)rctx;
    (void)kind;
    if (rc->mode == MODE_DECLINE) return 0;      /* out->fn stays NULL -> unbound */
    if (strcmp(name, "reopen_oracle") == 0) {
        out->fn = (rc->mode == MODE_WRONG_FN) ? identity_oracle : swap_oracle;
        out->ctx = NULL;
        out->identity = rc->id_oracle;
        if (rc->mode == MODE_WRONG_ID) out->identity.artifact_digest ^= 0xABCDEFu;
        out->has_identity = 1;
        return 0;
    }
    if (strcmp(name, "reopen_orphan") == 0) {
        out->fn = swap_oracle;
        out->ctx = NULL;
        out->identity = rc->id_orphan;           /* correct identity, but no provenance */
        out->has_identity = 1;
        return 0;
    }
    return 0;
}

static const double IN0[2] = {1.0, 0.0};
static const double IN1[2] = {0.0, 1.0};

/* Run the remounted oracle directly enough times to earn reliability above a
   fresh unit's 0.5, and verify it computes swap each time. */
static int exercise_oracle(SoulHost *h, int times) {
    int t, ok = 1;
    for (t = 0; t < times; ++t) {
        double o0[2] = {0, 0}, o1[2] = {0, 0};
        if (soul_run(h, "reopen_oracle", IN0, o0, 2) != 2 ||
            o0[0] != 0.0 || o0[1] != 1.0) ok = 0;
        if (soul_run(h, "reopen_oracle", IN1, o1, 2) != 2 ||
            o1[0] != 1.0 || o1[1] != 0.0) ok = 0;
    }
    return ok;
}

/* Plan+execute the typed chain reopen_in -> reopen_out. The only certified
   producer of reopen_mid is the native head; the planner prefers the
   evidence-rich remounted oracle over the never-run native teacher for
   reopen_mid -> reopen_out, so a correct swap output proves BOTH a native BTN
   and the remounted Oracle executed in one chain. */
static int run_chain(SoulHost *h) {
    double o0[2] = {0, 0}, o1[2] = {0, 0};
    int r0 = soul_request(h, PORT_ONEHOT, 2, 1, "reopen_in",
                          PORT_ONEHOT, 2, 1, "reopen_out", IN0, 2, o0, 2);
    int r1 = soul_request(h, PORT_ONEHOT, 2, 1, "reopen_in",
                          PORT_ONEHOT, 2, 1, "reopen_out", IN1, 2, o1, 2);
    return r0 == 2 && r1 == 2 &&
           o0[0] == 0.0 && o0[1] == 1.0 &&   /* swap(identity([1,0])) = [0,1] */
           o1[0] == 1.0 && o1[1] == 0.0;     /* swap(identity([0,1])) = [1,0] */
}

/* One full durable cycle: open -> mount (GOOD) -> prove kind + chain -> close. */
static void durable_cycle(const char *base_path, ResolverCtx *good, int cycle) {
    SoulHost *h = NULL;
    SoulMountReport rep;
    int k = -1, trust = -1, role = -1;
    char label[96];

    snprintf(label, sizeof label, "cycle %d: reopens the sealed base", cycle);
    check(soul_open(base_path, NULL, &h) == 0 && h != NULL, label);
    if (!h) return;
    check(soul_unit_count(h) == 2 && soul_oracle_count(h) == 2,
          "cycle: 2 native units replay, 2 oracle descriptors project");
    check(soul_unit_kind(h, "reopen_oracle", &k) < 0,
          "cycle: descriptor is NOT a live unit before remount");

    memset(&rep, 0, sizeof rep);
    /* Model a fresh reopen PROCESS: a cold certification cache, so the remount
       genuinely replays the recovered contract instead of hitting a memoized
       verdict from an earlier phase in this one test process. */
    contract_cache_reset();
    check(soul_mount_oracles(h, reopen_resolver, good, &rep) == 1,
          "cycle: exactly one oracle remounts through the resolver");
    check(rep.mounted == 1 && rep.missing_provenance == 1 &&
          rep.unbound == 0 && rep.identity_mismatch == 0 && rep.cert_failed == 0,
          "cycle: mount report — 1 mounted, orphan lacks provenance");
    check(soul_mounted_oracle_count(h) == 1, "cycle: mounted-oracle count is 1");

    k = -1;
    check(soul_unit_kind(h, "reopen_oracle", &k) == 0 && k == SPECIALIST_KIND_ORACLE,
          "cycle: remounted oracle reports kind=ORACLE in the live registry");
    check(soul_unit_kind(h, "reopen_head", &k) == 0 && k == SPECIALIST_KIND_BTN,
          "cycle: native head reports kind=BTN after reopen");
    check(soul_unit_axes(h, "reopen_oracle", &trust, &role) == 0 &&
          trust == SPECIALIST_TRUST_CERTIFIED && role == SPECIALIST_ROLE_ACTIVE,
          "cycle: remounted oracle reads certified/active on the shared axes");

    check(exercise_oracle(h, 12),
          "cycle: remounted oracle executes swap live through the adapter");
    check(soul_unit_reliability_milli(h, "reopen_oracle") > 500 &&
          soul_unit_reliability_milli(h, "reopen_tail") == 500,
          "cycle: the oracle earned evidence; the native teacher stayed idle");

    check(run_chain(h),
          "cycle: typed chain reopen_in->reopen_out executes (native + oracle)");
    check(soul_unit_reliability_milli(h, "reopen_tail") == 500 &&
          soul_unit_reliability_milli(h, "reopen_head") > 500,
          "cycle: chain routed through the native head + remounted oracle, not the teacher");

    soul_close(h);
}

/* A mount pass that must admit nothing, with an explicit refusal tally. */
static void refusal_case(const char *base_path, ResolverCtx *ctx,
                         int want_unbound, int want_identity_mismatch,
                         int want_cert_failed, const char *what) {
    SoulHost *h = NULL;
    SoulMountReport rep;
    int k = -1;
    char label[128];
    if (soul_open(base_path, NULL, &h) != 0 || !h) {
        check(0, "refusal: base reopens");
        return;
    }
    memset(&rep, 0, sizeof rep);
    contract_cache_reset();   /* cold cache: a wrong callback must genuinely fail replay */
    snprintf(label, sizeof label, "refusal(%s): mounts nothing", what);
    check(soul_mount_oracles(h, reopen_resolver, ctx, &rep) == 0 &&
          rep.mounted == 0, label);
    snprintf(label, sizeof label, "refusal(%s): explicit skip tally", what);
    check(rep.unbound == want_unbound &&
          rep.identity_mismatch == want_identity_mismatch &&
          rep.cert_failed == want_cert_failed, label);
    snprintf(label, sizeof label, "refusal(%s): descriptor never gains live trust", what);
    check(soul_unit_kind(h, "reopen_oracle", &k) < 0 &&
          soul_mounted_oracle_count(h) == 0, label);
    soul_close(h);
}

int main(void) {
    const char *base_path = "tmp_soul_reopen.cnb";
    Port p_in = port("reopen_in");
    Port p_mid = port("reopen_mid");
    Port p_out = port("reopen_out");
    /* identity: in==out; swap: out reversed */
    const double id_X[4] = {1, 0, 0, 1};
    const double id_Y[4] = {1, 0, 0, 1};
    const double sw_Y[4] = {0, 1, 1, 0};
    CnetBase base;
    ResolverCtx good, decline, wrong_fn, wrong_id;

    remove(base_path);
    remove("tmp_soul_reopen.cnb.tmp");

    printf("== soul_host reopen/remount execution tracer ==\n");

    /* ---- seal the base, then destroy every original runtime object -------- */
    cnb_init(&base);
    check(seal_native(&base, "reopen_head", p_in, p_mid, id_X, id_Y) == 0,
          "native head (identity, reopen_in->reopen_mid) seals");
    check(seal_native(&base, "reopen_tail", p_mid, p_out, id_X, sw_Y) == 0,
          "native teacher (swap, reopen_mid->reopen_out) seals");
    {
        CnetOracleIdentity id_oracle = make_identity(0x1111u);
        CnetOracleIdentity id_orphan = make_identity(0x9999u);
        check(cnb_add_oracle_desc_v2(&base, "reopen_oracle", "builtin",
                                     p_mid, p_out, &id_oracle) == 0,
              "evidence-carrying oracle descriptor enters the base");
        check(cnb_add_oracle_desc_v2(&base, "reopen_orphan", "builtin",
                                     p_mid, p_out, &id_orphan) == 0,
              "second (un-provenanced) oracle descriptor enters the base");
        check(cnb_set_unit_provenance(&base, "reopen_tail", "reopen_oracle") == 0,
              "the teacher unit records reopen_oracle as its provenance");
        check(cnb_save(&base, base_path) == 0, "sealed base saves atomically");
        cnb_free(&base);   /* every original runtime object is now gone */

        memset(&good, 0, sizeof good);
        good.mode = MODE_GOOD; good.id_oracle = id_oracle; good.id_orphan = id_orphan;
        decline = good;   decline.mode = MODE_DECLINE;
        wrong_fn = good;   wrong_fn.mode = MODE_WRONG_FN;
        wrong_id = good;   wrong_id.mode = MODE_WRONG_ID;
    }

    /* ---- two full close/reopen/remount cycles (durability) ---------------- */
    durable_cycle(base_path, &good, 1);
    durable_cycle(base_path, &good, 2);

    /* ---- explicit refusals, never silent runtime trust ------------------- */
    /* DECLINE: resolver binds nothing -> both descriptors unbound. */
    refusal_case(base_path, &decline, /*unbound*/2, /*idmis*/0, /*cert*/0,
                 "unresolved");
    /* WRONG_ID: reopen_oracle identity mismatch; orphan bound but no provenance. */
    refusal_case(base_path, &wrong_id, /*unbound*/0, /*idmis*/1, /*cert*/0,
                 "identity_mismatch");
    /* WRONG_FN: reopen_oracle certifies-and-fails; orphan bound but no provenance. */
    refusal_case(base_path, &wrong_fn, /*unbound*/0, /*idmis*/0, /*cert*/1,
                 "wrong_callback");

    /* the mounted-nothing null-resolver contract */
    {
        SoulHost *h = NULL;
        if (soul_open(base_path, NULL, &h) == 0 && h) {
            check(soul_mount_oracles(h, NULL, NULL, NULL) < 0,
                  "a NULL resolver is an explicit error (descriptor-only default)");
            soul_close(h);
        } else {
            check(0, "null-resolver fixture reopens");
        }
    }

    if (getenv("CNET_KEEP_TEST_BASE") == NULL) {
        remove(base_path);
        remove("tmp_soul_reopen.cnb.tmp");
    }

    printf("SPECIALIST_REOPEN_%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
