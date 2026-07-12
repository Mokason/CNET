/* Gap lane acceptance gate — the 24/7 learning loop, hermetic.
 *
 * One lane must close the whole circle without a human in it:
 *   serve   a certified-route miss lands in the gap INBOX (the serving
 *           side's only job), and gap_lane_execute answers via the oracle
 *           while the gap is open, harvesting the exemplar;
 *   detect  the tick ingests the inbox into the persistent ledger and
 *           bridges an unhealable demotion to a HEALTH gap — without
 *           re-noting subjects whose work is already deferred;
 *   learn   the drain mines the oracle, trains a student with DYNAMIC
 *           STRUCTURE GROWTH, certifies (PROOF here — enumerable domain),
 *           seals into the base, admits, and the planner finds the plan;
 *   persist the checkpoint is atomic for BOTH base and ledger, and a
 *           fresh lane resumed from disk replans without retraining.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/gap_lane.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"

#define SYM 8

static int failures;
static int checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static Port sym_port(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = SYM;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static void onehot_row(double *row, int hot) {
    int i;
    for (i = 0; i < SYM; i++) row[i] = (i == hot) ? 1.0 : 0.0;
}

/* The "local model": a reference oracle for rot3 over an 8-symbol alphabet. */
static int oracle_calls;
static int rot3_oracle(const double *in, double *out, void *ctx) {
    int i, hot = 0;
    (void)ctx;
    oracle_calls++;
    for (i = 1; i < SYM; i++) if (in[i] > in[hot]) hot = i;
    for (i = 0; i < SYM; i++) out[i] = 0.0;
    out[(hot + 3) % SYM] = 1.0;
    return 0;
}

int main(void) {
    const char *base_path = "tmp_gap_lane.cnb";
    const char *ledger_path = "tmp_gap_lane.gaps.txt";
    const char *inbox_path = "tmp_gap_lane.inbox";
    Port in_port = sym_port("gl_sym");
    Port goal_port = sym_port("gl_rot3");
    GapLane lane;
    GapLaneTickReport tick;
    RoutePlan plan;
    double input[SYM], output[SYM], expected[SYM];
    int i, ok;

    remove(base_path);
    remove(ledger_path);
    remove(inbox_path);
    memset(&plan, 0, sizeof plan);

    printf("== gap lane: the 24/7 learning loop ==\n");

    check(gap_lane_open(&lane, base_path, ledger_path, inbox_path) == 0 &&
          lane.reg.count == 0,
          "fresh lane opens with an empty certified registry");
    check(acquire_oracle_register(&lane.oracles, "rot3_ref", in_port,
                                  goal_port, rot3_oracle, NULL) == 0,
          "local-model oracle binds to the lane");
    {
        /* the teacher's identity is the unit's provenance-to-be */
        OracleEntry *oe = &lane.oracles.entries[0];
        memset(&oe->identity, 0, sizeof oe->identity);
        oe->identity.abi_version = CNET_ORACLE_ABI_VERSION;
        oe->identity.struct_size = (uint32_t)sizeof oe->identity;
        oe->identity.artifact_digest = 0x4d4f44454cULL;      /* "MODEL" */
        oe->identity.config_digest = 0x57494e444f57ULL;      /* "WINDOW" */
        oe->identity.contract_digest = 0x434f4e5452ULL;       /* "CONTR" */
        oe->identity.retrieval_snapshot_digest = 0xc0417e37ULL;
        oe->identity.toolchain_digest = 0x544f4f4cULL;        /* "TOOL" */
        oe->behavior_digest = cnet_oracle_identity_digest(&oe->identity);
    }
    /* keep the student's structure budget visible: growth is the point */
    lane.acq.init_hidden = 4;
    lane.acq.max_hidden = 64;
    /* the fixture domain is 8 points; the default evidence floor (16) is
       sized for real corpora and would rightly defer this tiny domain */
    lane.acq.min_evidence = SYM;

    /* -- serve: the miss is answered by the oracle AND harvested --------- */
    onehot_row(input, 2);
    onehot_row(expected, (2 + 3) % SYM);
    check(gap_lane_execute(&lane, in_port, goal_port, input, SYM,
                           output, SYM) == 0 &&
          memcmp(output, expected, sizeof expected) == 0,
          "no plan yet: oracle fallback answers the live task");
    check(lane.ledger.count == 1 && lane.ledger.gaps[0].cap_count == 1,
          "the miss is a ledger gap with the exemplar harvested");

    /* -- serve: an out-of-process miss goes through the inbox ------------ */
    check(gap_inbox_note_no_plan(inbox_path, in_port, goal_port) == 0,
          "serving process appends the miss to the gap inbox");

    /* -- tick: ingest + drain closes the gap with a grown student -------- */
    check(gap_lane_tick(&lane, &tick, 0) == 0 &&
          tick.inbox_ingested == 1 && tick.inbox_malformed == 0,
          "tick ingests the inbox (coalesces onto the open gap)");
    check(tick.drain.examined >= 1 && tick.drain.closed == 1 &&
          tick.drain.deferred == 0 &&
          lane.ledger.gaps[0].status == GAP_CLOSED,
          "drain closes the gap: mined, trained, certified, sealed");
    check(tick.drain.last_verdict == CERT_PROVEN,
          "enumerable domain certifies as PROOF, not sampled");
    check(oracle_calls > 0, "the local model actually taught");
    {
        /* structure growth: the student is a real trained net, not a stub */
        const RegistryEntry *e = NULL;
        size_t k;
        for (k = 0; k < lane.reg.count; ++k)
            if (strcmp(lane.reg.entries[k].name, "acq_gl_rot3") == 0)
                e = &lane.reg.entries[k];
        check(e && e->certified && e->state == PRIM_FROZEN &&
              e->btn->hidden_count >= lane.acq.init_hidden &&
              e->btn->hidden_count <= lane.acq.max_hidden,
              "student trained under the dynamic-growth structure budget");
    }
    check(tick.checkpointed == 1, "tick checkpointed base + ledger");
    check(lane.base.oracle_count == 1 &&
          strcmp(lane.base.oracles[0].name, "rot3_ref") == 0 &&
          lane.base.oracles[0].identity.retrieval_snapshot_digest ==
              0xc0417e37ULL &&
          lane.base.oracles[0].identity.config_digest == 0x57494e444f57ULL &&
          lane.base.oracles[0].identity.toolchain_digest == 0x544f4f4cULL,
          "closing the gap persists the teacher as unit provenance");
    check(strcmp(lane.ledger.gaps[0].unit, "acq_gl_rot3") == 0 &&
          lane.ledger.gaps[0].provenance_done == 1,
          "the closed gap records its minted unit and reconciles once");
    check(cnb_unit_provenance(&lane.base, "acq_gl_rot3") != NULL &&
          strcmp(cnb_unit_provenance(&lane.base, "acq_gl_rot3"),
                 "rot3_ref") == 0,
          "the unit points at its teacher descriptor directly");
    {
        char saved_gap_name[ACQUIRE_NAME_MAX];
        char saved_oracle_name[ACQUIRE_NAME_MAX];
        snprintf(saved_gap_name, sizeof saved_gap_name, "%s",
                 lane.ledger.gaps[0].oracle);
        snprintf(saved_oracle_name, sizeof saved_oracle_name, "%s",
                 lane.oracles.entries[0].name);
        snprintf(lane.ledger.gaps[0].oracle,
                 sizeof lane.ledger.gaps[0].oracle, "bad name");
        snprintf(lane.oracles.entries[0].name,
                 sizeof lane.oracles.entries[0].name, "bad name");
        lane.ledger.gaps[0].provenance_done = 0;  /* an unreconciled record */
        lane.base.oracle_count = 0;
        lane.provenance_dirty = 1;
        check(gap_lane_drain(&lane, &tick) != 0 &&
              lane.base.oracle_count == 0 && lane.provenance_dirty == 1 &&
              lane.ledger.gaps[0].provenance_done == 0,
              "provenance descriptor refusal propagates and remains retryable");
        snprintf(lane.ledger.gaps[0].oracle,
                 sizeof lane.ledger.gaps[0].oracle, "%s", saved_gap_name);
        snprintf(lane.oracles.entries[0].name,
                 sizeof lane.oracles.entries[0].name, "%s", saved_oracle_name);
        lane.base.oracle_count = 1;
        lane.provenance_dirty = 0;
    }

    /* -- the plan now exists and executes strictly, end to end ----------- */
    check(route_plan(&lane.reg, in_port, goal_port, &plan) == 0 &&
          plan.length == 1,
          "planner finds the acquired unit unaided");
    plan.strict = 1;
    ok = 1;
    for (i = 0; i < SYM; i++) {
        onehot_row(input, i);
        onehot_row(expected, (i + 3) % SYM);
        if (route_execute(&plan, input, SYM, output, SYM) != 0 ||
            memcmp(output, expected, sizeof expected) != 0) ok = 0;
    }
    check(ok, "strict execution exact on the whole domain (8/8)");

    /* -- detect: a genuinely broken unit becomes a HEALTH gap ------------
       (a merely-demoted-but-healthy incumbent is deliberately NOT rebuilt:
       the drain defers it incumbent_healthy — churn is refused by design;
       so the scenario corrupts the weights for real) */
    {
        BinaryTransformNetwork *victim = NULL;
        size_t k, j;
        for (k = 0; k < lane.reg.count; ++k)
            if (strcmp(lane.reg.entries[k].name, "acq_gl_rot3") == 0)
                victim = lane.reg.entries[k].btn;
        if (victim)
            for (j = 0; j < victim->hidden_count * victim->output_count; ++j)
                victim->hidden_output_weights[j] =
                    -victim->hidden_output_weights[j];
        check(victim != NULL, "fixture corrupts the live unit's weights");
    }
    check(gap_lane_scan(&lane, &tick) == 0 && tick.health_noted == 1,
          "audit demotes the tampered unit; heal has no verified target; "
          "the residue bridges to a HEALTH gap");
    check(gap_lane_scan(&lane, &tick) == 0 && tick.health_noted == 0,
          "re-scan refuses to re-note pending work (no churn)");
    check(gap_lane_drain(&lane, &tick) == 0 && tick.drain.closed >= 1,
          "rebuild path resolves the HEALTH gap through the oracle");
    check(gap_lane_checkpoint(&lane) == 0, "checkpoint persists the rebuild");
    check(lane.base.oracle_count == 1,
          "provenance is idempotent: the rebuild adds no duplicate");
    {
        /* reconciled records are never rescanned: with every CLOSED record
           marked done, a dirty pass must skip even a record whose oracle
           name would refuse a descriptor write (contrast the earlier
           fixture, where the same corruption on a NOT-done record failed
           the drain) */
        char saved[ACQUIRE_NAME_MAX];
        snprintf(saved, sizeof saved, "%s", lane.ledger.gaps[0].oracle);
        snprintf(lane.ledger.gaps[0].oracle,
                 sizeof lane.ledger.gaps[0].oracle, "bad name");
        lane.provenance_dirty = 1;
        check(gap_lane_drain(&lane, &tick) == 0 &&
              lane.provenance_dirty == 0,
              "a done record is skipped by the dirty pass, not rescanned");
        snprintf(lane.ledger.gaps[0].oracle,
                 sizeof lane.ledger.gaps[0].oracle, "%s", saved);
    }

    /* -- persist: a fresh lane resumes from disk, no retraining ---------- */
    gap_lane_close(&lane);
    memset(&plan, 0, sizeof plan);
    check(gap_lane_open(&lane, base_path, ledger_path, inbox_path) == 0 &&
          lane.reg.count >= 1,
          "resume: certification replay re-admits the sealed units");
    check(lane.base.oracle_count == 1 &&
          lane.base.oracles[0].identity.retrieval_snapshot_digest ==
              0xc0417e37ULL &&
          lane.base.oracles[0].identity.toolchain_digest == 0x544f4f4cULL,
          "unit provenance survives the resume round-trip");
    check(cnb_unit_provenance(&lane.base, "acq_gl_rot3") != NULL &&
          strcmp(cnb_unit_provenance(&lane.base, "acq_gl_rot3"),
                 "rot3_ref") == 0,
          "the direct unit -> descriptor relation survives the resume");
    check(strcmp(lane.ledger.gaps[0].unit, "acq_gl_rot3") == 0 &&
          lane.ledger.gaps[0].provenance_done == 1,
          "the resumed ledger carries the minted unit AND the persisted "
          "reconcile mark (ledger v3): restart re-verifies nothing");
    check(route_plan(&lane.reg, in_port, goal_port, &plan) == 0,
          "resumed lane replans without retraining");
    {
        size_t open_gaps = 0, k;
        for (k = 0; k < lane.ledger.count; ++k)
            if (lane.ledger.gaps[k].status == GAP_OPEN) open_gaps++;
        check(open_gaps == 0, "resumed ledger carries no open gaps");
    }
    check(gap_lane_tick(&lane, &tick, 0) == 0 && tick.checkpointed == 0 &&
          tick.drain.examined == 0 && tick.health_noted == 0,
          "steady state: an idle tick is a no-op and skips the checkpoint");

    /* -- corpus-drawn id files: strict parsing ---------------------------
       (the window and teaching-context files feed the teacher's alphabet;
       a refused file must bind nothing rather than teach a wrong one) */
    {
        int ids[8];
        FILE *f = fopen("tmp_gap_lane.ids", "w");
        check(f != NULL, "id fixture file opens");
        if (f) {
            fprintf(f, "506\n\n  529\n532\r\n");
            fclose(f);
        }
        check(gap_lane_load_ids("tmp_gap_lane.ids", ids, 8) == 3 &&
              ids[0] == 506 && ids[1] == 529 && ids[2] == 532,
              "id file parses (blanks skipped, whitespace tolerated)");
        check(gap_lane_load_ids("tmp_gap_lane.ids", ids, 2) == -1,
              "over-capacity id file is refused whole");
        f = fopen("tmp_gap_lane.ids", "w");
        if (f) { fprintf(f, "506\nnot_a_token\n"); fclose(f); }
        check(gap_lane_load_ids("tmp_gap_lane.ids", ids, 8) == -1,
              "malformed id line refuses the whole file");
        check(gap_lane_load_ids("tmp_gap_lane.missing", ids, 8) == -1,
              "missing id file is refused, not defaulted");
        remove("tmp_gap_lane.ids");
    }

    /* -- artifact identity: the digest is over BYTES, not the path -------- */
    {
        unsigned long long da = 0, db = 0, dc = 0;
        FILE *f = fopen("tmp_gap_lane.art_a", "wb");
        if (f) { fputs("teacher weights v1", f); fclose(f); }
        f = fopen("tmp_gap_lane.art_b", "wb");
        if (f) { fputs("teacher weights v1", f); fclose(f); }
        f = fopen("tmp_gap_lane.art_c", "wb");
        if (f) { fputs("teacher weights v2", f); fclose(f); }
        check(gap_lane_digest_file("tmp_gap_lane.art_a", &da) == 0 &&
              gap_lane_digest_file("tmp_gap_lane.art_b", &db) == 0 &&
              da == db,
              "identical bytes digest identically under different paths");
        check(gap_lane_digest_file("tmp_gap_lane.art_c", &dc) == 0 &&
              dc != da,
              "different bytes digest differently (the artifact, not the name)");
        check(gap_lane_digest_file("tmp_gap_lane.art_missing", &da) == -1,
              "an unreadable artifact binds no identity");
        f = fopen("tmp_gap_lane.art_e", "wb");
        if (f) fclose(f);
        check(gap_lane_digest_file("tmp_gap_lane.art_e", &da) == -1,
              "an EMPTY artifact binds no identity (not the hash basis)");
        remove("tmp_gap_lane.art_a");
        remove("tmp_gap_lane.art_b");
        remove("tmp_gap_lane.art_c");
        remove("tmp_gap_lane.art_e");
    }

    /* -- identity-aware descriptor rebinding ------------------------------
       (the audited defect: a swapped model under a recurring teacher name
       must never inherit the OLD stored identity) */
    {
        OracleEntry *oe;
        int idx, idx2;
        Port goal2 = sym_port("gl_rot3v2");
        Port goal3 = sym_port("gl_rot3v3");
        check(acquire_oracle_register(&lane.oracles, "rot3_ref", in_port,
                                      goal_port, rot3_oracle, NULL) == 0,
              "teacher re-binds after the resume");
        oe = &lane.oracles.entries[0];
        memset(&oe->identity, 0, sizeof oe->identity);
        oe->identity.abi_version = CNET_ORACLE_ABI_VERSION;
        oe->identity.struct_size = (uint32_t)sizeof oe->identity;
        oe->identity.artifact_digest = 0x4d4f44454c32ULL;    /* "MODEL2" */
        oe->identity.config_digest = 0x57494e444f57ULL;
        oe->identity.contract_digest = 0x434f4e5452ULL;
        oe->identity.retrieval_snapshot_digest = 0xc0417e37ULL;
        oe->identity.toolchain_digest = 0x544f4f4cULL;
        oe->behavior_digest = cnet_oracle_identity_digest(&oe->identity);

        idx = acquire_note_no_plan(&lane.ledger, in_port, goal2);
        check(idx >= 0, "gap noted for the swapped-model teacher");
        lane.ledger.gaps[idx].status = GAP_CLOSED;
        snprintf(lane.ledger.gaps[idx].oracle,
                 sizeof lane.ledger.gaps[idx].oracle, "rot3_ref");
        lane.provenance_dirty = 1;
        check(gap_lane_drain(&lane, &tick) == 0 &&
              lane.base.oracle_count == 2 &&
              strcmp(lane.base.oracles[1].name, "rot3_ref_i2") == 0 &&
              lane.base.oracles[1].identity.artifact_digest ==
                  0x4d4f44454c32ULL &&
              lane.base.oracles[0].identity.artifact_digest ==
                  0x4d4f44454cULL &&
              lane.ledger.gaps[idx].provenance_done == 1,
              "identity change under a recurring name mints a versioned "
              "descriptor; the old identity is never silently reused");

        /* an unattested (zero-toolchain) identity is not provenance */
        oe->identity.toolchain_digest = 0;
        oe->behavior_digest = cnet_oracle_identity_digest(&oe->identity);
        idx2 = acquire_note_no_plan(&lane.ledger, in_port, goal3);
        check(idx2 >= 0, "gap noted for the unattested teacher");
        lane.ledger.gaps[idx2].status = GAP_CLOSED;
        snprintf(lane.ledger.gaps[idx2].oracle,
                 sizeof lane.ledger.gaps[idx2].oracle, "rot3_ref");
        lane.provenance_dirty = 1;
        check(gap_lane_drain(&lane, &tick) == 0 &&
              lane.base.oracle_count == 2 &&
              lane.ledger.gaps[idx2].provenance_done == 0,
              "a zero-toolchain teacher identity records nothing and the "
              "record stays pending, not failed");

        /* no live teacher + ambiguous name family: never guess a link */
        lane.oracles.count = 0;
        lane.provenance_dirty = 1;
        check(gap_lane_drain(&lane, &tick) == 0 &&
              lane.ledger.gaps[idx2].provenance_done == 1 &&
              lane.base.oracle_count == 2,
              "ambiguous descriptor family without a live teacher "
              "reconciles WITHOUT a link — lineage is never guessed");
    }

    gap_lane_close(&lane);
    remove(base_path);
    remove(ledger_path);
    remove(inbox_path);

    printf("GAP_LANE_%s checks=%d\n", failures ? "FAIL" : "PASS", checks);
    return failures ? 1 : 0;
}
