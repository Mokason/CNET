/*
 * structural_pref_common.h -- shared static helpers for the Structural
 * Preference Sidecar -- Derivation Lock (SHADOW_ONLY) study + anchor.
 * (spec: docs/superpowers/specs/2026-06-19-structural-preference-sidecar-design.md)
 *
 * The headline metric is the DERIVATION LOCK residual: among ALREADY-VALID
 * structures, re-derive the same single-root planning task under bounded
 * perturbations and count how often the SAME canonical plan structure (by a
 * Merkle-style digest) reappears. Low residual = many derivation paths converge
 * (an attractor); high residual = ordering-dependent (fragile). It is advisory
 * only -- zero authority, ranks within valid, never touches the validator,
 * planner, registry, or reliability counters.
 *
 * SINGLE-ROOT only (dag_plan / DagPlan). The multi-root circuit attractor is
 * a different API (dag_plan_circuit / CircuitPlan) and is out of scope here.
 *
 * The perturbations are FOOTPRINT-FREE (no Delta-4 needed):
 *   - PRIMARY: candidate-order permutation -- plan against a PERMUTED COPY of
 *     the registry's entry array (shallow per-entry, so the borrowed BTNs +
 *     name pointers are shared; the array is permuted, the BTNs and the
 *     original array are never written). Equal-reliability producers are
 *     ordered by the registry-order tiebreak (rank_by_reliability,
 *     src/router.c:302), so permuting the entry array flips which equal
 *     producer wins -> surfaces alternative valid structures.
 *   - SECONDARY: search-depth robustness -- beam in {1,2,4,8,0} and
 *     disable_plan_memo {off,on} on a SCALAR shallow copy.
 *
 * The digest is BY NAME/STRUCTURE, NOT pointer: two plans with the same
 * structure must hash equal even if allocated differently.
 *
 * Static functions, included by both the anchor (test_structural_pref.c) and
 * the study (structural_pref_study.c); compile with -Wno-unused-function (like
 * residue_common.h / belowbeam_chars_common.h).
 */
#ifndef STRUCTURAL_PREF_COMMON_H
#define STRUCTURAL_PREF_COMMON_H

#include "../include/nn.h"
#include "../include/router.h"
#include "../include/scan.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Delegates to the promoted single source of truth in src/scan.c. */
static uint64_t spc_plan_digest(const DagPlan *plan) {
    return scan_plan_digest(plan);
}

/* ---- The candidate-order perturbation (footprint-free, NO Delta-4) ----------
 *
 * Plan against a PERMUTED COPY of reg->entries[]. The copy is shallow (shares
 * the borrowed BTN + name pointers); the original entries array and all BTN
 * counters are NEVER written. perm_idx selects a deterministic permutation;
 * beam / memo are the secondary scalar perturbation (applied to the lifted
 * shallow copy only).
 *
 * On success writes the structural digest to *out_digest and returns 1 (planned).
 * Returns 0 if no plan exists (and leaves *out_digest = 0). Returns -1 on a bad
 * argument / allocation failure.
 */

/* Apply permutation perm_idx in place to perm[0..n-1] (a deterministic family
   of orderings: identity, rotations, reversal, then fixed adjacent swaps). */
static void spc_permute_entries(RegistryEntry *perm, size_t n, size_t perm_idx) {
    size_t i;

    if (n < 2) {
        return;  /* nothing to permute */
    }
    if (perm_idx == 0) {
        return;  /* identity = baseline ordering */
    }
    if (perm_idx < n) {
        /* rotations by perm_idx positions */
        RegistryEntry tmp[64];
        size_t r = perm_idx % n;
        size_t cap = sizeof(tmp) / sizeof(tmp[0]);
        if (n <= cap) {
            for (i = 0; i < n; ++i) {
                tmp[i] = perm[(i + r) % n];
            }
            for (i = 0; i < n; ++i) {
                perm[i] = tmp[i];
            }
        }
        return;
    }
    if (perm_idx == n) {
        /* full reversal */
        for (i = 0; i < n / 2; ++i) {
            RegistryEntry t = perm[i];
            perm[i] = perm[n - 1 - i];
            perm[n - 1 - i] = t;
        }
        return;
    }
    /* perm_idx > n: a fixed adjacent swap at position (perm_idx - n - 1) % (n-1) */
    {
        size_t pos = (perm_idx - n - 1) % (n - 1);
        RegistryEntry t = perm[pos];
        perm[pos] = perm[pos + 1];
        perm[pos + 1] = t;
    }
}

/* Plan the single-root task (sources -> goal) under one perturbation cell. */
static int spc_plan_under_perturbation(const PrimitiveRegistry *reg,
                                       const DagSource *sources, size_t n_sources,
                                       Port goal,
                                       size_t perm_idx, size_t beam, int memo_on,
                                       uint64_t *out_digest) {
    RegistryEntry *perm;
    PrimitiveRegistry lifted;
    DagPlan p;
    int planned = 0;

    if (out_digest != NULL) {
        *out_digest = 0u;
    }
    if (reg == NULL || out_digest == NULL) {
        return -1;
    }
    if (reg->count == 0) {
        return 0;  /* nothing to plan over */
    }

    perm = malloc(reg->count * sizeof *perm);
    if (perm == NULL) {
        return -1;
    }
    memcpy(perm, reg->entries, reg->count * sizeof *perm);  /* shallow */
    spc_permute_entries(perm, reg->count, perm_idx);

    /* A scalar shallow copy with the permuted entry array swapped in. The
       original reg (and its entries array + BTNs) is never touched. */
    lifted = *reg;
    lifted.entries = perm;
    lifted.count = reg->count;
    lifted.capacity = reg->count;
    /* Secondary perturbation: search depth (beam) + memo. beam 0 = planner
       default; disable_plan_memo is the inverse of memo_on. */
    lifted.dag_beam_limit = beam;
    lifted.disable_plan_memo = memo_on ? 0 : 1;

    memset(&p, 0, sizeof p);
    if (dag_plan(&lifted, sources, n_sources, goal, &p) == 0) {
        *out_digest = spc_plan_digest(&p);
        planned = 1;
        dag_free(&p);
    }

    free(perm);
    return planned;
}

/* ---- The perturbation grid + sweep helper ----------------------------------
 *
 * grid = K candidate-order permutations x beam in {1,2,4,8,0} x memo {off,on}.
 * Baseline = identity permutation (perm 0), default beam (0), memo on.
 */

#define SPC_PERM_COUNT 8u                 /* K candidate-order permutations */
static const size_t SPC_BEAMS[] = { 1, 2, 4, 8, 0 };
#define SPC_BEAM_COUNT (sizeof(SPC_BEAMS) / sizeof(SPC_BEAMS[0]))
#define SPC_MEMO_COUNT 2u                 /* memo off, memo on */
#define SPC_CELL_COUNT (SPC_PERM_COUNT * SPC_BEAM_COUNT * SPC_MEMO_COUNT)

/* Baseline cell coordinates. */
#define SPC_BASELINE_PERM 0u
#define SPC_BASELINE_BEAM 0u   /* default beam */
#define SPC_BASELINE_MEMO 1    /* memo on */

/* One swept cell. */
typedef struct {
    size_t perm_idx;
    size_t beam;
    int memo_on;
    int planned;        /* 1 if dag_plan succeeded */
    uint64_t digest;    /* structural digest (0 if not planned) */
    int matches_baseline;
} SpcCell;

/* The whole-fixture sweep result. */
typedef struct {
    uint64_t baseline_digest;
    int baseline_planned;
    size_t K;       /* cells that planned */
    size_t R;       /* cells matching baseline digest */
    size_t D;       /* distinct digests among planned cells */
    size_t X;       /* cells that did not plan */
    double derivation_lock_residual;   /* 1 - R/K */
    size_t cell_count;
    SpcCell cells[SPC_CELL_COUNT];
} SpcSweep;

/* Count distinct digests among the planned cells (O(n^2), grid is tiny). */
static size_t spc_count_distinct(const SpcCell *cells, size_t n) {
    size_t i, j, distinct = 0;
    for (i = 0; i < n; ++i) {
        int seen = 0;
        if (!cells[i].planned) {
            continue;
        }
        for (j = 0; j < i; ++j) {
            if (cells[j].planned && cells[j].digest == cells[i].digest) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            ++distinct;
        }
    }
    return distinct;
}

/* Run the full perturbation grid over one fixture and fill *out. Returns 0 on
   success, -1 on a bad argument. Read-only over reg (every cell plans against a
   permuted COPY); reg and its BTN counters are left untouched. */
static int spc_run_sweep(const PrimitiveRegistry *reg,
                         const DagSource *sources, size_t n_sources,
                         Port goal, SpcSweep *out) {
    size_t pi, bi, mi, n = 0;

    if (reg == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof *out);

    /* Baseline digest first (identity perm, default beam, memo on). */
    out->baseline_planned = spc_plan_under_perturbation(
        reg, sources, n_sources, goal,
        SPC_BASELINE_PERM, SPC_BASELINE_BEAM, SPC_BASELINE_MEMO,
        &out->baseline_digest);
    if (out->baseline_planned < 0) {
        return -1;
    }

    for (pi = 0; pi < SPC_PERM_COUNT; ++pi) {
        for (bi = 0; bi < SPC_BEAM_COUNT; ++bi) {
            for (mi = 0; mi < SPC_MEMO_COUNT; ++mi) {
                SpcCell *c = &out->cells[n];
                int rc;
                c->perm_idx = pi;
                c->beam = SPC_BEAMS[bi];
                c->memo_on = (int)mi;   /* 0 = off, 1 = on */
                rc = spc_plan_under_perturbation(reg, sources, n_sources, goal,
                                                 c->perm_idx, c->beam,
                                                 c->memo_on, &c->digest);
                if (rc < 0) {
                    return -1;
                }
                c->planned = rc;
                c->matches_baseline = (c->planned && out->baseline_planned &&
                                       c->digest == out->baseline_digest);
                if (c->planned) {
                    ++out->K;
                    if (c->matches_baseline) {
                        ++out->R;
                    }
                } else {
                    ++out->X;
                }
                ++n;
            }
        }
    }
    out->cell_count = n;
    out->D = spc_count_distinct(out->cells, n);
    out->derivation_lock_residual =
        (out->K > 0) ? (1.0 - (double)out->R / (double)out->K) : 0.0;
    return 0;
}

/* The boring derivation-lock score (laws/compression deferred for these
   synthetic fixtures): reproduced_fraction - distinct_valid_penalty.
   reproduced_fraction = R/K; distinct_valid_penalty = (D-1)/K. */
static double spc_score(const SpcSweep *s) {
    double reproduced_fraction, distinct_valid_penalty;
    if (s == NULL || s->K == 0) {
        return 0.0;
    }
    reproduced_fraction = (double)s->R / (double)s->K;
    distinct_valid_penalty = (s->D > 0)
        ? (double)(s->D - 1) / (double)s->K
        : 0.0;
    return reproduced_fraction - distinct_valid_penalty;
}

/* ---- Step A: post-hoc candidate RANKING (SHADOW_PLUS_RECOMMEND; planner untouched) --
 *
 * Group the sweep's planned cells by digest, rank the distinct structures by
 * reproduction-count DESC (ties -> lowest first-seen perm_idx, deterministic),
 * each with a re-derivation recipe (the first-seen cell's perm/beam/memo). ALL
 * distinct structures are returned -- NO prune. Read-only over reg (reuses the
 * footprint-free sweep). The most-reproduced structure (rank 0) is the most
 * order-robust = the recommended canonical structure. This makes the sidecar
 * RECOMMEND, not merely report; the planner/registry are NOT touched
 * (planner_influence stays 0 -- consuming the order IN the planner is Step B). */

typedef struct {
    uint64_t digest;
    size_t   count;      /* # planned cells producing this digest */
    size_t   perm_idx;   /* recipe: first-seen cell's perturbation that... */
    size_t   beam;       /*   ...reproduces this digest */
    int      memo_on;
} SpcRankedEntry;

typedef struct {
    SpcRankedEntry entries[SPC_CELL_COUNT];
    size_t   count;            /* number of distinct structures (== sweep D) */
    uint64_t baseline_digest;  /* the planner's default pick (sweep baseline) */
    int      baseline_rank;    /* index of baseline_digest in entries[], or -1 */
} SpcRanked;

static int structural_pref_rank(const PrimitiveRegistry *reg,
                                const DagSource *sources, size_t n_sources,
                                Port goal, SpcRanked *out) {
    SpcSweep sweep;
    size_t i, j, n = 0;

    if (reg == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof *out);
    out->baseline_rank = -1;
    if (spc_run_sweep(reg, sources, n_sources, goal, &sweep) != 0) {
        return -1;
    }
    out->baseline_digest = sweep.baseline_digest;

    /* group planned cells by digest; the first-seen cell becomes the recipe */
    for (i = 0; i < sweep.cell_count; ++i) {
        const SpcCell *c = &sweep.cells[i];
        int found = 0;
        if (!c->planned) {
            continue;
        }
        for (j = 0; j < n; ++j) {
            if (out->entries[j].digest == c->digest) {
                out->entries[j].count++;
                found = 1;
                break;
            }
        }
        if (!found) {
            out->entries[n].digest = c->digest;
            out->entries[n].count = 1;
            out->entries[n].perm_idx = c->perm_idx;
            out->entries[n].beam = c->beam;
            out->entries[n].memo_on = c->memo_on;
            ++n;
        }
    }
    out->count = n;

    /* sort by count DESC; tie -> lower recipe perm_idx (insertion sort, tiny n) */
    for (i = 1; i < n; ++i) {
        SpcRankedEntry key = out->entries[i];
        j = i;
        while (j > 0 &&
               (out->entries[j - 1].count < key.count ||
                (out->entries[j - 1].count == key.count &&
                 out->entries[j - 1].perm_idx > key.perm_idx))) {
            out->entries[j] = out->entries[j - 1];
            --j;
        }
        out->entries[j] = key;
    }

    /* where does the planner's default (baseline) sit in the ranking? */
    for (j = 0; j < n; ++j) {
        if (out->entries[j].digest == out->baseline_digest) {
            out->baseline_rank = (int)j;
            break;
        }
    }
    return 0;
}

/* Re-derive the rank-0 (recommended canonical) structure from its recipe and
   write its digest to *out_digest. Returns 1 if it planned, else 0/-1. Only
   re-runs the footprint-free perturbation -- proves the recommendation is
   actionable (a real, reproducible plan) without touching the planner. */
static int structural_pref_recommend_digest(const PrimitiveRegistry *reg,
                                            const DagSource *sources, size_t n_sources,
                                            Port goal, const SpcRanked *r,
                                            uint64_t *out_digest) {
    if (reg == NULL || r == NULL || out_digest == NULL || r->count == 0) {
        return -1;
    }
    return spc_plan_under_perturbation(reg, sources, n_sources, goal,
                                       r->entries[0].perm_idx,
                                       r->entries[0].beam,
                                       r->entries[0].memo_on, out_digest);
}

/* ---- B0: gate-probe helpers (opt-in ORDER_ONLY rank-artifact bias) ----------
 *
 * B0 probes whether A's structural ranking, frozen into a CircuitRankArtifact,
 * can be consumed through the EXISTING v2.2 ORDER_ONLY path as an opt-in
 * additive traversal bias that flips the lure's selected producer toward the
 * attractor A recommends -- full set preserved, zero authority, default runtime
 * still attention_mode=OFF. (spec:
 * docs/superpowers/specs/2026-06-19-structural-preference-B0-gate-probe-design.md)
 *
 * Keying limitation (reported, not hidden): the planner's lookup passes
 * task_key="" and producer_output_port=0 and IGNORES root_goal/output_sig
 * (src/router.c:949,1720-1728), so an artifact row keyed on
 * (task_key="", producer_name, output_port=0) biases per-producer-name GLOBALLY,
 * not per-task. */

/* Plans the rank-0 recipe; copies the root producer name to out_name (cap bytes).
   Returns 1 if planned with a DAG_PRIMITIVE root, else 0/-1. Footprint-free
   (mirrors spc_plan_under_perturbation: plan against a permuted shallow COPY of
   the entry array; the original reg + BTNs are never written). */
static int spc_root_name_under_perturbation(const PrimitiveRegistry *reg,
        const DagSource *sources, size_t n_sources, Port goal,
        size_t perm_idx, size_t beam, int memo_on, char *out_name, size_t cap) {
    RegistryEntry *perm;
    PrimitiveRegistry lifted;
    DagPlan p;
    int rc = 0;

    if (out_name != NULL && cap > 0) {
        out_name[0] = '\0';
    }
    if (reg == NULL || out_name == NULL || cap == 0) {
        return -1;
    }
    if (reg->count == 0) {
        return 0;  /* nothing to plan over */
    }

    perm = malloc(reg->count * sizeof *perm);
    if (perm == NULL) {
        return -1;
    }
    memcpy(perm, reg->entries, reg->count * sizeof *perm);  /* shallow */
    spc_permute_entries(perm, reg->count, perm_idx);

    lifted = *reg;
    lifted.entries = perm;
    lifted.count = reg->count;
    lifted.capacity = reg->count;
    lifted.dag_beam_limit = beam;
    lifted.disable_plan_memo = memo_on ? 0 : 1;

    memset(&p, 0, sizeof p);
    if (dag_plan(&lifted, sources, n_sources, goal, &p) == 0) {
        if (p.root != NULL && p.root->kind == DAG_PRIMITIVE &&
            p.root->name != NULL) {
            strncpy(out_name, p.root->name, cap - 1);
            out_name[cap - 1] = '\0';
            rc = 1;
        } else {
            rc = 0;  /* planned, but root is not a DAG_PRIMITIVE */
        }
        dag_free(&p);
    }

    free(perm);
    return rc;
}

/* Build a CircuitRankArtifact from A's ranking: re-derive rank-0's producer name
   (via the rank-0 recipe), init the artifact, add ONE row keyed
   { task_key="", producer_name=<that name>, producer_output_port=0,
   rank_prior=+1.0, no_*_authority=1 }, set row_count=1, order_only=1, frozen=1.
   Returns 0 on success, -1 on a bad argument / failure to re-derive a name.
   Footprint-free (only re-runs the read-only perturbation). */
static int structural_pref_build_artifact(const PrimitiveRegistry *reg,
        const DagSource *sources, size_t n_sources, Port goal,
        const SpcRanked *r, CircuitRankArtifact *out) {
    char name[64];
    int planned;
    CircuitRankArtifactRow *row;

    if (reg == NULL || r == NULL || out == NULL || r->count == 0) {
        return -1;
    }
    planned = spc_root_name_under_perturbation(reg, sources, n_sources, goal,
                                               r->entries[0].perm_idx,
                                               r->entries[0].beam,
                                               r->entries[0].memo_on,
                                               name, sizeof name);
    if (planned != 1) {
        return -1;  /* rank-0 did not re-derive to a named DAG_PRIMITIVE root */
    }

    circuit_rank_artifact_init(out);
    row = &out->rows[0];
    memset(row, 0, sizeof *row);
    row->task_key[0] = '\0';                 /* MUST be "" (planner lookup key) */
    snprintf(row->producer_name, sizeof row->producer_name, "%s", name);
    row->producer_output_port = 0;           /* MUST be 0 (planner lookup key) */
    row->rank_prior = 1.0;                    /* additive +prior toward rank-0 */
    row->order_only = 1;
    row->no_prune_authority = 1;
    row->no_cert_authority = 1;
    row->no_registry_authority = 1;
    out->row_count = 1;
    out->order_only = 1;
    out->frozen = 1;
    return 0;
}

/* Read the SELECTED root producer name under a given (mode, artifact) config on
   a NON-const registry COPY (scalar shallow copy: entries + BTNs shared, so the
   caller's reg is never mutated). dag_plan against the copy; copy root->name to
   out_name. Returns 1 if planned with a DAG_PRIMITIVE root, else 0/-1. */
static int spc_selected_producer(const PrimitiveRegistry *reg,
        const DagSource *sources, size_t n_sources, Port goal,
        CNETAttentionMode mode, const CircuitRankArtifact *artifact,
        char *out_name, size_t cap) {
    PrimitiveRegistry copy;
    DagPlan p;
    int rc = 0;

    if (out_name != NULL && cap > 0) {
        out_name[0] = '\0';
    }
    if (reg == NULL || out_name == NULL || cap == 0) {
        return -1;
    }

    copy = *reg;                 /* scalar shallow copy -- entries array shared */
    copy.attention_mode = mode;
    copy.rank_artifact = artifact;

    memset(&p, 0, sizeof p);
    if (dag_plan(&copy, sources, n_sources, goal, &p) == 0) {
        if (p.root != NULL && p.root->kind == DAG_PRIMITIVE &&
            p.root->name != NULL) {
            strncpy(out_name, p.root->name, cap - 1);
            out_name[cap - 1] = '\0';
            rc = 1;
        } else {
            rc = 0;
        }
        dag_free(&p);
    }
    return rc;
}

/* ---- The fixtures (single-root, distinct-named, built fresh) ----------------
 *
 * Concrete ports: X = ONEHOT 4 "x" (the one source), G_a = ONEHOT 4 "ga",
 * G_l = ONEHOT 4 "gl". The source carries a one-hot values buffer so dag_plan
 * is well-formed (we only ever call dag_plan). The source tag differs from the
 * goal tag, so the source can never satisfy the goal directly -> a producer is
 * always required.
 *
 * Attractor: ONE producer solo: X -> G_a. Every perturbation -> the same single
 *   structure -> D = 1, residual 0.
 * Lure: TWO DISTINCT-NAMED producers alt_a: X -> G_l and alt_b: X -> G_l (same
 *   signature, distinct names, both fresh = equal reliability). Permutations
 *   flip which wins -> >= 2 digests -> D >= 2, residual > 0.
 *
 * "Valid" here = dag_plan returns a plan (type-consistent). The BTNs are
 * synthetic + untrained (we measure derivation structure, not execution).
 * require_certified stays 0.
 */

#define SPC_X_WIDTH 4

typedef struct {
    BinaryTransformNetwork producers[2];   /* attractor uses [0]; lure uses [0],[1] */
    size_t producer_count;
    PrimitiveRegistry reg;

    Port goal;
    Port src_type;
    double src_values[SPC_X_WIDTH];
    DagSource sources[1];

    int built;
} SpcFixture;

/* Build one synthetic producer X -> goal (untrained). in is ONEHOT 4 "x";
   out is ONEHOT 4 with tag goal_tag. Returns 0 on success, -1 on failure. */
static int spc_build_producer(BinaryTransformNetwork *b, const char *goal_tag) {
    Port in = { PORT_ONEHOT, SPC_X_WIDTH, 1, "" };
    Port out = { PORT_ONEHOT, SPC_X_WIDTH, 1, "" };
    port_set_tag(&in, "x");
    port_set_tag(&out, goal_tag);
    memset(b, 0, sizeof *b);
    if (btn_init(b, SPC_X_WIDTH, SPC_X_WIDTH, 1, 4, 0.5, 1u) != 0) {
        return -1;
    }
    return btn_set_ports(b, in, out);
}

/* Fill the task source (X = ONEHOT 4 "x", one-hot at index 0). */
static void spc_set_task(SpcFixture *f, const char *goal_tag) {
    size_t i;
    f->goal = (Port){ PORT_ONEHOT, SPC_X_WIDTH, 1, "" };
    f->src_type = (Port){ PORT_ONEHOT, SPC_X_WIDTH, 1, "" };
    port_set_tag(&f->goal, goal_tag);
    port_set_tag(&f->src_type, "x");
    for (i = 0; i < SPC_X_WIDTH; ++i) {
        f->src_values[i] = (i == 0) ? 1.0 : 0.0;
    }
    f->sources[0].type = f->src_type;
    f->sources[0].values = f->src_values;
}

/* Build the ATTRACTOR fixture: one producer solo: X -> G_a ("ga").
   Returns 0 on success, -1 on a build failure. */
static int spc_make_attractor(SpcFixture *f) {
    memset(f, 0, sizeof *f);
    if (spc_build_producer(&f->producers[0], "ga") != 0) {
        return -1;
    }
    f->producer_count = 1;
    spc_set_task(f, "ga");
    registry_init(&f->reg);
    registry_add(&f->reg, &f->producers[0], "solo");
    f->built = 1;
    return 0;
}

/* Build the LURE fixture: two distinct-named producers alt_a, alt_b: X -> G_l
   ("gl"), same signature -> equal (fresh) reliability. Returns 0/-1. */
static int spc_make_lure(SpcFixture *f) {
    memset(f, 0, sizeof *f);
    if (spc_build_producer(&f->producers[0], "gl") != 0) {
        return -1;
    }
    if (spc_build_producer(&f->producers[1], "gl") != 0) {
        btn_free(&f->producers[0]);
        return -1;
    }
    f->producer_count = 2;
    spc_set_task(f, "gl");
    registry_init(&f->reg);
    registry_add(&f->reg, &f->producers[0], "alt_a");
    registry_add(&f->reg, &f->producers[1], "alt_b");
    f->built = 1;
    return 0;
}

/* Release everything a fixture owns. Safe on a partially-built fixture. */
static void spc_fixture_free(SpcFixture *f) {
    size_t i;
    if (f->built) {
        registry_free(&f->reg);
    }
    for (i = 0; i < f->producer_count; ++i) {
        btn_free(&f->producers[i]);
    }
}

#endif /* STRUCTURAL_PREF_COMMON_H */
