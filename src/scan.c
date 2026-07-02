/*
 * scan.c -- implementation of reusable scan/step helpers.
 * See include/scan.h for API and motivation (attacking enumerable-boundary
 * and hand-construction walls for compositional sequential domains).
 */

#include "../include/scan.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- FNV-1a 64-bit mixing for structural plan digest ----------------------
 * Promoted from tests/structural_pref_common.h into the compiled source so
 * scan_plan_digest() is a proper public API (not test-only).              */

#define SPC_FNV_OFFSET 1469598103934665603ULL
#define SPC_FNV_PRIME  1099511628211ULL

static uint64_t spc_mix_u64(uint64_t hash, uint64_t value) {
    size_t i;
    for (i = 0; i < 8; ++i) {
        hash ^= (value & 0xffu);
        hash *= SPC_FNV_PRIME;
        value >>= 8;
    }
    return hash;
}

/* Mix a NUL-terminated string (its bytes plus a terminating 0 so "ab"+"c" and
   "a"+"bc" do not collide). */
static uint64_t spc_mix_str(uint64_t hash, const char *s) {
    size_t i;
    if (s != NULL) {
        for (i = 0; s[i] != '\0'; ++i) {
            hash ^= (uint64_t)(unsigned char)s[i];
            hash *= SPC_FNV_PRIME;
        }
    }
    hash ^= 0u;            /* string terminator marker */
    hash *= SPC_FNV_PRIME;
    return hash;
}

/* Mix a port's structural identity: family, field_width, field_count, tag. */
static uint64_t spc_mix_port(uint64_t hash, const Port *p) {
    hash = spc_mix_u64(hash, (uint64_t)p->family);
    hash = spc_mix_u64(hash, (uint64_t)p->field_width);
    hash = spc_mix_u64(hash, (uint64_t)p->field_count);
    hash = spc_mix_str(hash, p->tag);
    return hash;
}

/* Recursive Merkle hash of a DagNode: by name/structure, NOT by pointer. */
static uint64_t spc_hash_node(const DagNode *node) {
    uint64_t hash = SPC_FNV_OFFSET;
    size_t slot;

    if (node == NULL) {
        return 0u;
    }
    if (node->kind == DAG_SOURCE) {
        hash = spc_mix_str(hash, "SRC");
        hash = spc_mix_u64(hash, (uint64_t)(int64_t)node->source_index);
        return hash;
    }

    /* DAG_PRIMITIVE */
    hash = spc_mix_str(hash, "PRIM");
    hash = spc_mix_str(hash, node->name);
    hash = spc_mix_u64(hash, (uint64_t)(int64_t)node->output_index);
    if (node->btn != NULL &&
        (size_t)node->output_index < node->btn->output_port_count) {
        const Port *op = &node->btn->output_ports[node->output_index];
        hash = spc_mix_port(hash, op);
    }
    for (slot = 0; slot < node->child_count; ++slot) {
        hash = spc_mix_u64(hash, (uint64_t)slot);
        hash = spc_mix_u64(hash, (uint64_t)(int64_t)node->child_ports[slot]);
        hash = spc_mix_u64(hash, spc_hash_node(node->children[slot]));
    }
    return hash;
}

/* Public: structural Merkle digest of a plan (0 if plan or root is NULL). */
uint64_t scan_plan_digest(const DagPlan *plan) {
    if (plan == NULL || plan->root == NULL) return 0u;
    return spc_hash_node(plan->root);
}

/* ---- Perturbation sweep for structural_canonical_digest -------------------
 *
 * These are file-local copies of the helpers from
 * tests/structural_pref_common.h, promoted into the compiled source so that
 * structural_canonical_digest() is a proper public API.  They are kept static
 * (file-local) so they do NOT collide with the identically-named statics that
 * test files include from the header.
 *
 * The only intentional difference from the header originals: the internal call
 * inside spc_plan_under_perturbation uses scan_plan_digest() (the promoted
 * public API) instead of the local-static spc_plan_digest().             */

/* Apply permutation perm_idx in place to perm[0..n-1]. */
static void spc_permute_entries(RegistryEntry *perm, size_t n, size_t perm_idx) {
    size_t i;
    if (n < 2) return;
    if (perm_idx == 0) return;
    if (perm_idx < n) {
        RegistryEntry tmp[64];
        size_t r = perm_idx % n;
        size_t cap = sizeof(tmp) / sizeof(tmp[0]);
        if (n <= cap) {
            for (i = 0; i < n; ++i) tmp[i] = perm[(i + r) % n];
            for (i = 0; i < n; ++i) perm[i] = tmp[i];
        }
        return;
    }
    if (perm_idx == n) {
        for (i = 0; i < n / 2; ++i) {
            RegistryEntry t = perm[i];
            perm[i] = perm[n - 1 - i];
            perm[n - 1 - i] = t;
        }
        return;
    }
    {
        size_t pos = (perm_idx - n - 1) % (n - 1);
        RegistryEntry t = perm[pos];
        perm[pos] = perm[pos + 1];
        perm[pos + 1] = t;
    }
}

/* Plan single-root task under one perturbation cell; returns 1=planned,
   0=no plan, -1=error. Writes structural digest to *out_digest. */
static int spc_plan_under_perturbation(const PrimitiveRegistry *reg,
                                       const DagSource *sources, size_t n_sources,
                                       Port goal,
                                       size_t perm_idx, size_t beam, int memo_on,
                                       uint64_t *out_digest) {
    RegistryEntry *perm;
    PrimitiveRegistry lifted;
    DagPlan p;
    int planned = 0;

    if (out_digest != NULL) *out_digest = 0u;
    if (reg == NULL || out_digest == NULL) return -1;
    if (reg->count == 0) return 0;

    perm = malloc(reg->count * sizeof *perm);
    if (perm == NULL) return -1;
    memcpy(perm, reg->entries, reg->count * sizeof *perm);
    spc_permute_entries(perm, reg->count, perm_idx);

    lifted = *reg;
    lifted.entries = perm;
    lifted.count = reg->count;
    lifted.capacity = reg->count;
    lifted.dag_beam_limit = beam;
    lifted.disable_plan_memo = memo_on ? 0 : 1;

    memset(&p, 0, sizeof p);
    if (dag_plan(&lifted, sources, n_sources, goal, &p) == 0) {
        *out_digest = scan_plan_digest(&p);   /* promoted public API */
        planned = 1;
        dag_free(&p);
    }

    free(perm);
    return planned;
}

/* Perturbation grid: K permutations x beam in {1,2,4,8,0} x memo {off,on}. */
#define SPC_PERM_COUNT 8u
static const size_t SPC_BEAMS[] = { 1, 2, 4, 8, 0 };
#define SPC_BEAM_COUNT (sizeof(SPC_BEAMS) / sizeof(SPC_BEAMS[0]))
#define SPC_MEMO_COUNT 2u
#define SPC_CELL_COUNT (SPC_PERM_COUNT * SPC_BEAM_COUNT * SPC_MEMO_COUNT)

#define SPC_BASELINE_PERM 0u
#define SPC_BASELINE_BEAM 0u
#define SPC_BASELINE_MEMO 1

typedef struct {
    size_t perm_idx;
    size_t beam;
    int memo_on;
    int planned;
    uint64_t digest;
    int matches_baseline;
} SpcCell;

typedef struct {
    uint64_t baseline_digest;
    int baseline_planned;
    size_t K;
    size_t R;
    size_t D;
    size_t X;
    double derivation_lock_residual;
    size_t cell_count;
    SpcCell cells[SPC_CELL_COUNT];
} SpcSweep;

static size_t spc_count_distinct(const SpcCell *cells, size_t n) {
    size_t i, j, distinct = 0;
    for (i = 0; i < n; ++i) {
        int seen = 0;
        if (!cells[i].planned) continue;
        for (j = 0; j < i; ++j) {
            if (cells[j].planned && cells[j].digest == cells[i].digest) {
                seen = 1; break;
            }
        }
        if (!seen) ++distinct;
    }
    return distinct;
}

static int spc_run_sweep(const PrimitiveRegistry *reg,
                         const DagSource *sources, size_t n_sources,
                         Port goal, SpcSweep *out) {
    size_t pi, bi, mi, n = 0;

    if (reg == NULL || out == NULL) return -1;
    memset(out, 0, sizeof *out);

    out->baseline_planned = spc_plan_under_perturbation(
        reg, sources, n_sources, goal,
        SPC_BASELINE_PERM, SPC_BASELINE_BEAM, SPC_BASELINE_MEMO,
        &out->baseline_digest);
    if (out->baseline_planned < 0) return -1;

    for (pi = 0; pi < SPC_PERM_COUNT; ++pi) {
        for (bi = 0; bi < SPC_BEAM_COUNT; ++bi) {
            for (mi = 0; mi < SPC_MEMO_COUNT; ++mi) {
                SpcCell *c = &out->cells[n];
                int rc;
                c->perm_idx = pi;
                c->beam = SPC_BEAMS[bi];
                c->memo_on = (int)mi;
                rc = spc_plan_under_perturbation(reg, sources, n_sources, goal,
                                                 c->perm_idx, c->beam,
                                                 c->memo_on, &c->digest);
                if (rc < 0) return -1;
                c->planned = rc;
                c->matches_baseline = (c->planned && out->baseline_planned &&
                                       c->digest == out->baseline_digest);
                if (c->planned) {
                    ++out->K;
                    if (c->matches_baseline) ++out->R;
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

/* The CANONICAL digest of a goal's plan equivalence class: run the footprint-
   free perturbation sweep, return the SMALLEST structural digest among the
   distinct planned structures. min(digest) is a stable, content-addressed
   identity (unlike reproduction-count rank-0, which is grid-dependent). Returns
   0 if no plan exists. Read-only over reg (plans against permuted copies). */
uint64_t structural_canonical_digest(const PrimitiveRegistry *reg,
                                     const DagSource *sources, size_t n_sources,
                                     Port goal) {
    SpcSweep sweep;
    uint64_t best = 0; int have = 0; size_t i;
    if (reg == NULL) return 0;
    if (spc_run_sweep(reg, sources, n_sources, goal, &sweep) != 0) return 0;
    for (i = 0; i < sweep.cell_count; ++i) {
        if (!sweep.cells[i].planned) continue;
        if (!have || sweep.cells[i].digest < best) { best = sweep.cells[i].digest; have = 1; }
    }
    return have ? best : 0;
}

/* ---- Internal: recursive cartesian enumerator.
   Fills in_codes with the current combination, calls transition, encodes. */
static void enumerate_step(size_t port_idx,
                           const StepShape *shape,
                           int *in_codes,
                           step_transition_fn transition,
                           void *ctx,
                           double *inputs,
                           double *targets,
                           size_t *row,
                           size_t in_total,
                           size_t out_total) {
    if (port_idx == shape->num_in_ports) {
        int out_codes[DAG_MAX_SLOTS];
        transition(in_codes, shape->num_in_ports, out_codes, shape->num_out_ports, ctx);

        /* encode input row (0/1 one-hot style) */
        size_t off = 0;
        for (size_t p = 0; p < shape->num_in_ports; ++p) {
            size_t w = shape->in_widths[p];
            for (size_t j = 0; j < w; ++j) {
                inputs[*row * in_total + off + j] = (j == (size_t)in_codes[p]) ? 1.0 : 0.0;
            }
            off += w;
        }

        /* encode target row (soft 0.9/0.1) */
        off = 0;
        for (size_t p = 0; p < shape->num_out_ports; ++p) {
            size_t w = shape->out_widths[p];
            for (size_t j = 0; j < w; ++j) {
                targets[*row * out_total + off + j] = (j == (size_t)out_codes[p]) ? 0.9 : 0.1;
            }
            off += w;
        }

        ++(*row);
        return;
    }

    size_t w = shape->in_widths[port_idx];
    for (size_t v = 0; v < w; ++v) {
        in_codes[port_idx] = (int)v;
        enumerate_step(port_idx + 1, shape, in_codes, transition, ctx,
                       inputs, targets, row, in_total, out_total);
    }
}

size_t build_step_training_data(const StepShape *shape,
                                step_transition_fn transition,
                                void *ctx,
                                double *inputs,
                                double *targets) {
    if (shape == NULL || transition == NULL || inputs == NULL || targets == NULL) {
        return 0;
    }
    if (shape->num_in_ports == 0 || shape->num_out_ports == 0) {
        return 0;
    }

    size_t in_total = 0;
    for (size_t p = 0; p < shape->num_in_ports; ++p) {
        in_total += shape->in_widths[p];
    }
    size_t out_total = 0;
    for (size_t p = 0; p < shape->num_out_ports; ++p) {
        out_total += shape->out_widths[p];
    }

    size_t total_rows = 1;
    for (size_t p = 0; p < shape->num_in_ports; ++p) {
        total_rows *= shape->in_widths[p];
        /* guard overflow roughly */
        if (total_rows > (size_t)-1 / 2) return 0;
    }

    int *codes = (int *)malloc(shape->num_in_ports * sizeof(int));
    if (codes == NULL) return 0;

    size_t row = 0;
    enumerate_step(0, shape, codes, transition, ctx, inputs, targets, &row,
                   in_total, out_total);

    free(codes);
    return row;
}

int certify_step(BinaryTransformNetwork *step,
                 const char *name,
                 const double *inputs,
                 const double *targets,
                 size_t samples,
                 double margin_floor,
                 CertifyReport *report) {
    if (step == NULL || name == NULL || inputs == NULL || targets == NULL || samples == 0) {
        return -1;
    }

    Contract c;
    /* We assume the caller has already set the ports on *step exactly as the
       shape described. We build a borrowed contract over the *current* ports. */
    if (contract_init_borrowed(&c, name, step, inputs, targets, samples) != 0) {
        return -1;
    }

    return btn_certify_robust(step, &c, margin_floor, report);
}

int make_layered_scan_contract(const Contract *step_contract,
                               const char *scan_name,
                               Contract *out_scan_contract) {
    if (step_contract == NULL || scan_name == NULL || out_scan_contract == NULL) {
        return -1;
    }

    /* Simple layered view: the scan "is" the step for interface purposes,
       but we record the parent for the layering / freeze machinery.
       In a fuller version we could copy the step table or mark it thin.
       Here we just set up the parent relationship using the existing API. */
    *out_scan_contract = *step_contract;  /* shallow copy of ports etc. */
    strncpy(out_scan_contract->name, scan_name, CONTRACT_NAME_MAX - 1);
    out_scan_contract->name[CONTRACT_NAME_MAX - 1] = '\0';

    if (contract_set_parent(out_scan_contract, step_contract->name) != 0) {
        return -1;
    }

    /* The caller can still decide whether to duplicate the exemplar table
       or rely on the parent (layering in generated.h / freeze will see the parent). */
    return 0;
}

/* ---- contract_from_iterative_scan implementation ----------------------- */

int contract_from_iterative_scan(
    BinaryTransformNetwork *step,
    const char *step_name,
    const StepWiring *wiring,
    const char *scan_name,
    size_t small_n,
    const DagSource *initial_states,
    size_t n_state,
    const DagSource *sample_items,
    const Contract *step_contract,
    size_t max_samples_for_emit,
    Contract *out
) {
    if (step == NULL || wiring == NULL || scan_name == NULL || small_n == 0 ||
        initial_states == NULL || sample_items == NULL || out == NULL) {
        return -1;
    }

    /* Temporary storage for the small unrolled plan (hand-built style) */
    DagNode *temp_steps = (DagNode *)malloc(small_n * sizeof(DagNode));
    if (temp_steps == NULL) return -1;

    /* We also need temporary source nodes for the initials and items for this small plan.
       For simplicity, we re-use the caller's provided initial_states and sample_items
       as the node objects (they are DagSource but the builder treats them as nodes
       in the hand-built sense; the caller must have set .kind etc. or we create minimal). */
    /* To avoid mutation, we allocate minimal wrappers. */
    DagNode *state_nodes = (DagNode *)malloc(n_state * sizeof(DagNode));
    DagNode *item_nodes  = (DagNode *)malloc(small_n * sizeof(DagNode));
    if (!state_nodes || !item_nodes) {
        free(temp_steps);
        free(state_nodes);
        free(item_nodes);
        return -1;
    }

    /* Initialize minimal source nodes from the provided DagSource types/values */
    for (size_t s = 0; s < n_state; ++s) {
        state_nodes[s].kind = DAG_SOURCE;
        state_nodes[s].source_index = (int)s;
        /* The builder only needs the nodes to point; actual values come from
           the sources array passed to contract_from_dag via the teacher ctx. */
    }
    for (size_t i = 0; i < small_n; ++i) {
        item_nodes[i].kind = DAG_SOURCE;
        item_nodes[i].source_index = (int)(n_state + i);
    }

    DagPlan small_plan;
    int rc = dag_build_iterative_scan(step, step_name, wiring,
                                      state_nodes, n_state,
                                      item_nodes, small_n,
                                      temp_steps, &small_plan);
    if (rc != 0) {
        free(temp_steps); free(state_nodes); free(item_nodes);
        return -1;
    }

    /* Build the combined sources array for contract emission (initial states + items) */
    /* We don't have the actual value arrays here; the caller is expected to have
       set up sample data such that the teacher can use the sources. For this helper
       we pass the provided initial_states + sample_items as the sources list. */
    const DagSource *all_sources = initial_states; /* the first n_state are states, then items follow in the array the caller prepared */

    /* Emit the contract for the small unrolled scan using the existing machinery */
    rc = contract_from_dag(&small_plan, all_sources, n_state + small_n,
                           scan_name, max_samples_for_emit, out);

    /* Apply layering if a step contract was supplied */
    if (rc == 0 && step_contract != NULL) {
        (void)contract_set_parent(out, step_contract->name);
    }

    /* Cleanup: hand-built plan, no dag_free needed */
    free(temp_steps);
    free(state_nodes);
    free(item_nodes);

    return rc;
}

/* ---- scan_run_simple wrapper (easy execution for new domains) ----------- */

int scan_run_simple(
    BinaryTransformNetwork *step,
    const StepWiring *wiring,
    const double *initial_state_values,
    const double * const *item_values,
    size_t n_steps,
    double *final_out,
    size_t out_cap
) {
    if (step == NULL || wiring == NULL || initial_state_values == NULL ||
        item_values == NULL || n_steps == 0 || final_out == NULL) {
        return -1;
    }

    /* Allocate everything needed for a hand-built scan */
    size_t n_state = wiring->n_state_slots;
    DagNode *state_nodes = (DagNode *)calloc(n_state, sizeof(DagNode));
    DagNode *item_nodes  = (DagNode *)calloc(n_steps, sizeof(DagNode));
    DagNode *step_nodes  = (DagNode *)calloc(n_steps, sizeof(DagNode));
    DagSource *srcs = (DagSource *)calloc(n_state + n_steps, sizeof(DagSource));
    if (!state_nodes || !item_nodes || !step_nodes || !srcs) {
        free(state_nodes); free(item_nodes); free(step_nodes); free(srcs);
        return -1;
    }

    DagPlan plan;

    /* Prepare state source nodes + srcs entries (values point to caller's data) */
    size_t val_off = 0;
    for (size_t s = 0; s < n_state; ++s) {
        state_nodes[s].kind = DAG_SOURCE;
        state_nodes[s].source_index = (int)s;
        /* Determine width from step's known input port for that slot */
        size_t w = step->input_ports[ wiring->state_in_slots[s] ].field_count *
                   step->input_ports[ wiring->state_in_slots[s] ].field_width;
        srcs[s].type = step->input_ports[ wiring->state_in_slots[s] ]; /* approximate */
        srcs[s].values = (double *)(initial_state_values + val_off);
        val_off += w;
    }

    /* Prepare item nodes + srcs */
    for (size_t i = 0; i < n_steps; ++i) {
        item_nodes[i].kind = DAG_SOURCE;
        item_nodes[i].source_index = (int)(n_state + i);
        size_t data_slot = wiring->data_in_slots[0]; /* assume at least one */
        srcs[n_state + i].type = step->input_ports[data_slot];
        srcs[n_state + i].values = (double *)item_values[i];
    }

    int rc = dag_build_iterative_scan(step, "scan_step", wiring,
                                      state_nodes, n_state,
                                      item_nodes, n_steps,
                                      step_nodes, &plan);
    if (rc == 0) {
        rc = dag_execute(&plan, srcs, n_state + n_steps, final_out, out_cap);
    }

    free(state_nodes);
    free(item_nodes);
    free(step_nodes);
    free(srcs);

    return rc;
}
