/*
 * belowbeam_chars_common.h -- shared static helpers for the below-beam
 * failure-mode CHARACTERIZATION study (spec: docs/superpowers/specs/
 * 2026-06-19-belowbeam-failure-mode-characterization-design.md).
 *
 * The v5.1 probe (proposal_sidecar_below_beam_probe) DETECTS a correct producer
 * ranked below the planner's beam. This study characterizes the SHAPE of that
 * blind spot under the two causes that push a correct producer below the cutoff:
 *   - cold-start (under-ranked-correct): delta's OWN reliability is low;
 *   - rank-poisoning (over-ranked-wrong): wrong-for-this-task competitors are
 *     injected high enough to consume the beam.
 *
 * The fixture device is reused VERBATIM from tests/test_below_beam_recovery.c:
 * the correct producer is the mod-k residue delta (b=4,k=7) from
 * residue_common.h, trained + certified, with its residue input slot retagged
 * "r_state" and its output retagged "r_next" so the goal ONEHOT 7 "r_next" is
 * produced ONLY by delta (the start residue source cannot satisfy it directly).
 * Competitors are synthetic untrained BTNs consuming "r_state" whose output is
 * INCOMPATIBLE with the goal (wrong tag or wrong width), so they rank by
 * reliability and spend the beam but can never satisfy the goal.
 *
 * Reliability is set by writing the lifetime Laplace counters DIRECTLY before
 * planning (output_successes/output_failures); the ranking reads them on demand
 * (recon-confirmed). btn_reliability(btn) = (s+1)/(s+f+2), no decay term.
 *
 * Margin/rank reconstruction here REPLICATES rank_by_reliability
 * (src/router.c:302): a stable sort of btn_reliability descending, ties keeping
 * registry order. The reliability MARGIN is delta's reliability minus the
 * reliability of the entry at index beam_limit (the (k)th, just past the
 * cutoff). If delta is itself within the top-k, the margin is non-negative and
 * there is no blind spot.
 *
 * Static functions, included by both the anchor (test_belowbeam_chars.c) and the
 * study (belowbeam_chars_study.c); compile with -Wno-unused-function (like
 * residue_common.h).
 */
#ifndef BELOWBEAM_CHARS_COMMON_H
#define BELOWBEAM_CHARS_COMMON_H

#include "residue_common.h"
#include "../include/proposal_sidecar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The task fixture parameters (the v5.1 device): residue delta b=4,k=7,
   start residue onehot(0), digit onehot(3) -> expected next residue
   (0*4 + 3) % 7 = 3. */
#define BBC_B 4
#define BBC_K 7
#define BBC_START_RESIDUE 0
#define BBC_DIGIT 3
#define BBC_EXPECTED ((BBC_START_RESIDUE * BBC_B + BBC_DIGIT) % BBC_K)

/* Up to this many competitors in any fixture (cold-start uses 8, rank-poisoning
   sweeps up to 8). The registry then holds delta + competitors. */
#define BBC_MAX_COMPETITORS 8
#define BBC_MAX_ENTRIES (BBC_MAX_COMPETITORS + 1)

/* A fully-built below-beam fixture: the registry plus all the storage the
   sources and BTNs borrow. Owned by the caller; release with bbc_fixture_free.
   delta is index `delta_index` in the registry; competitors fill the rest. */
typedef struct {
    BinaryTransformNetwork delta;
    BinaryTransformNetwork competitors[BBC_MAX_COMPETITORS];
    char competitor_names[BBC_MAX_COMPETITORS][32];  /* stable: registry borrows */
    size_t competitor_count;
    PrimitiveRegistry reg;

    /* Task ports + sources. */
    Port goal;
    Port src_residue;
    Port src_digit;
    double residue_vec[RES_MAX_K];
    double digit_vec[RES_MAX_B];
    DagSource sources[2];

    int built;             /* 1 once the registry is populated */
    int delta_certified;   /* 1 if delta certified on its table */
} BbcFixture;

/* Build + certify the correct producer (delta) into *f, retagged to the task
   tags. Returns 0 on success, -1 if delta does not certify (fixture invalid). */
static int bbc_build_delta(BbcFixture *f) {
    static double inputs[RES_MAX_K * RES_MAX_B * (RES_MAX_K + RES_MAX_B)];
    static double targets[RES_MAX_K * RES_MAX_B * RES_MAX_K];
    CertifyReport rep;
    size_t samples;

    memset(&f->delta, 0, sizeof f->delta);
    samples = build_residue_step_data(BBC_B, BBC_K, inputs, targets);
    train_residue_step(&f->delta, BBC_B, BBC_K, inputs, targets, samples, 12345u);
    if (certify_residue_step(&f->delta, BBC_K, inputs, targets, samples,
                             0.0, &rep) != 0) {
        f->delta_certified = 0;
        return -1;
    }
    f->delta_certified = 1;
    /* Retag delta's residue input slot + output to the distinct task tags so
       the start source cannot satisfy the goal directly and only delta produces
       it. Pure tag relabel of representationally-identical ONEHOT 7 ports. */
    port_set_tag(&f->delta.input_ports[0], "r_state");
    port_set_tag(&f->delta.output_ports[0], "r_next");
    return 0;
}

/* Build one synthetic competitor BTN: input ONEHOT k "r_state", output
   INCOMPATIBLE with the goal so it can never satisfy it. Even-indexed
   competitors get a wrong WIDTH (ONEHOT 3); odd-indexed get a wrong TAG
   (ONEHOT k "r_other"). Untrained. Returns 0 on success, -1 on failure. */
static int bbc_build_competitor(BinaryTransformNetwork *b, size_t idx) {
    Port in = {PORT_ONEHOT, (size_t)BBC_K, 1, ""};
    Port out;
    if ((idx & 1u) == 0u) {
        out = (Port){PORT_ONEHOT, 3, 1, ""};            /* wrong width */
    } else {
        out = (Port){PORT_ONEHOT, (size_t)BBC_K, 1, ""};/* wrong tag below */
    }
    port_set_tag(&in, "r_state");
    if ((idx & 1u) != 0u) {
        port_set_tag(&out, "r_other");                  /* tag mismatch vs goal */
    }
    memset(b, 0, sizeof *b);
    if (btn_init(b, (size_t)BBC_K, out.field_width * out.field_count,
                 1, 4, 0.5, 1u) != 0) {
        return -1;
    }
    return btn_set_ports(b, in, out);
}

/* Fill the task ports + sources (goal "r_next", start residue "r_state"
   onehot(0), digit "digit" onehot(3)). */
static void bbc_set_task(BbcFixture *f) {
    f->goal = (Port){PORT_ONEHOT, (size_t)BBC_K, 1, ""};
    f->src_residue = (Port){PORT_ONEHOT, (size_t)BBC_K, 1, ""};
    f->src_digit = (Port){PORT_ONEHOT, (size_t)BBC_B, 1, ""};
    port_set_tag(&f->goal, "r_next");
    port_set_tag(&f->src_residue, "r_state");
    port_set_tag(&f->src_digit, "digit");

    res_onehot(f->residue_vec, BBC_START_RESIDUE, BBC_K);
    res_onehot(f->digit_vec, BBC_DIGIT, BBC_B);
    f->sources[0].type = f->src_residue;
    f->sources[0].values = f->residue_vec;
    f->sources[1].type = f->src_digit;
    f->sources[1].values = f->digit_vec;
}

/* --- Cause 1: cold-start (under-ranked-correct) -----------------------------
 * M competitors FIXED with a SPREAD of reliabilities (competitor i:
 * output_successes = 20 + 5*i, distinct, all high). delta's evidence is the
 * swept knob: output_successes = n, output_failures = 0. As n rises delta's
 * reliability climbs and its rank decreases (moves above competitors).
 *
 * Builds the whole fixture and populates the registry (delta added LAST so the
 * spread competitors occupy the early registry slots, matching the v5.1 device
 * where the correct producer is registered after the decoys). beam set to k.
 * Returns 0 on success, -1 on a build/certify failure. */
static int bbc_make_coldstart(BbcFixture *f, size_t m_competitors,
                              unsigned long delta_successes,
                              size_t beam_limit) {
    size_t i;

    memset(f, 0, sizeof *f);
    if (m_competitors > BBC_MAX_COMPETITORS) return -1;
    if (bbc_build_delta(f) != 0) return -1;

    f->competitor_count = m_competitors;
    for (i = 0; i < m_competitors; ++i) {
        if (bbc_build_competitor(&f->competitors[i], i) != 0) return -1;
        f->competitors[i].output_successes = 20u + 5u * (unsigned long)i;
        f->competitors[i].output_failures = 0u;
    }
    f->delta.output_successes = delta_successes;
    f->delta.output_failures = 0u;

    bbc_set_task(f);

    registry_init(&f->reg);
    for (i = 0; i < m_competitors; ++i) {
        sprintf(f->competitor_names[i], "competitor_%d", (int)i);
        registry_add(&f->reg, &f->competitors[i], f->competitor_names[i]);
    }
    registry_add(&f->reg, &f->delta, "residue_step");
    registry_set_dag_beam_limit(&f->reg, beam_limit);
    f->built = 1;
    return 0;
}

/* --- Cause 2: rank-poisoning (over-ranked-wrong) ----------------------------
 * delta FIXED at moderate reliability (output_successes = 8, output_failures =
 * 0 -> rel ~ 0.9). The swept knob is the number m of injected-high competitors
 * present (competitor j: output_successes = 200 + 10*j -> rel ~ 0.99, distinct,
 * all > delta). As m grows delta's rank rises past the beam. beam set to k.
 * Returns 0 on success, -1 on a build/certify failure. */
static int bbc_make_poisoning(BbcFixture *f, size_t m_competitors,
                              unsigned long delta_successes,
                              size_t beam_limit) {
    size_t i;

    memset(f, 0, sizeof *f);
    if (m_competitors > BBC_MAX_COMPETITORS) return -1;
    if (bbc_build_delta(f) != 0) return -1;

    f->competitor_count = m_competitors;
    for (i = 0; i < m_competitors; ++i) {
        if (bbc_build_competitor(&f->competitors[i], i) != 0) return -1;
        f->competitors[i].output_successes = 200u + 10u * (unsigned long)i;
        f->competitors[i].output_failures = 0u;
    }
    f->delta.output_successes = delta_successes;
    f->delta.output_failures = 0u;

    bbc_set_task(f);

    registry_init(&f->reg);
    for (i = 0; i < m_competitors; ++i) {
        sprintf(f->competitor_names[i], "competitor_%d", (int)i);
        registry_add(&f->reg, &f->competitors[i], f->competitor_names[i]);
    }
    registry_add(&f->reg, &f->delta, "residue_step");
    registry_set_dag_beam_limit(&f->reg, beam_limit);
    f->built = 1;
    return 0;
}

/* Release everything a fixture owns. Safe to call on a partially-built fixture
   (btn_free tolerates a zeroed BTN; registry_free tolerates a zeroed reg). */
static void bbc_fixture_free(BbcFixture *f) {
    size_t i;
    if (f->built) {
        registry_free(&f->reg);
    }
    btn_free(&f->delta);
    for (i = 0; i < f->competitor_count; ++i) {
        btn_free(&f->competitors[i]);
    }
}

/* --- Margin / rank reconstruction (replicates rank_by_reliability) ----------
 * Sort the registry entry indices by btn_reliability descending, ties keeping
 * registry order (stable). This mirrors src/router.c:302 exactly so the rank we
 * compute matches the probe's recovered_producer_rank. */
static void bbc_rank_order(const PrimitiveRegistry *reg, size_t *order) {
    size_t i, j;
    for (i = 0; i < reg->count; ++i) order[i] = i;
    for (i = 1; i < reg->count; ++i) {
        size_t key = order[i];
        double score = btn_reliability(reg->entries[key].btn);
        for (j = i; j > 0; --j) {
            double prev = btn_reliability(reg->entries[order[j - 1]].btn);
            if (prev < score) { order[j] = order[j - 1]; continue; }
            break;   /* ties keep registry order (power_mode DEFAULT) */
        }
        order[j] = key;
    }
}

/* The 0-based reliability rank of the entry named `name` (first strcmp match),
   or -1 if not present. Matches the planner's ranking. */
static int bbc_rank_of(const PrimitiveRegistry *reg, const char *name) {
    size_t order[BBC_MAX_ENTRIES];
    size_t i;
    if (reg->count > BBC_MAX_ENTRIES) return -1;
    bbc_rank_order(reg, order);
    for (i = 0; i < reg->count; ++i) {
        if (strcmp(reg->entries[order[i]].name, name) == 0) return (int)i;
    }
    return -1;
}

/* The reliability of the producer at the cutoff: the entry ranked at index
   `beam_limit` (0-based; the (k)th, just past the top-k). If beam_limit is at
   or past the registry size there is no such entry -> returns -1.0 (caller
   treats this as "no producer at the cutoff", margin undefined). */
static double bbc_cutoff_reliability(const PrimitiveRegistry *reg,
                                     size_t beam_limit) {
    size_t order[BBC_MAX_ENTRIES];
    if (reg->count > BBC_MAX_ENTRIES) return -1.0;
    if (beam_limit >= reg->count) return -1.0;
    bbc_rank_order(reg, order);
    return btn_reliability(reg->entries[order[beam_limit]].btn);
}

/* The reliability MARGIN = delta_reliability - cutoff_reliability. If there is
   no entry at the cutoff (beam >= count), returns delta_reliability (delta is
   trivially within the considered set; no blind spot from the cutoff). */
static double bbc_margin(const PrimitiveRegistry *reg, const char *delta_name,
                         size_t beam_limit) {
    double delta_rel = -1.0;
    double cutoff_rel;
    size_t i;
    for (i = 0; i < reg->count; ++i) {
        if (strcmp(reg->entries[i].name, delta_name) == 0) {
            delta_rel = btn_reliability(reg->entries[i].btn);
            break;
        }
    }
    cutoff_rel = bbc_cutoff_reliability(reg, beam_limit);
    if (cutoff_rel < 0.0) return delta_rel;
    return delta_rel - cutoff_rel;
}

#endif /* BELOWBEAM_CHARS_COMMON_H */
