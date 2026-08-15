#include "../include/library.h"
#include "../include/cnet_dc_type.h"
#include "../include/scan.h"
#include "../include/specialist.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Have we already invented this exact behavior? Keyed on the contract: an
   existing entry whose ports match AND that replays every exemplar exactly. */
static int contract_already_known(PrimitiveRegistry *reg, const Contract *c) {
    size_t i;
    for (i = 0; i < reg->count; ++i) {
        if (reg->entries[i].btn != NULL &&
            btn_certify(reg->entries[i].btn, c, NULL) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Does any supplied law have a real violation against the current registry?
   property_check returns 0 iff the law holds. A nonzero return with
   report.violated > 0 is a regression; a nonzero return with report.inputs == 0
   means the law could not be evaluated (unresolved name / RAW / over-cap) --
   a warning, not a violation, so it does not trigger rollback. */
static int law_violated(const Property *laws, size_t n_laws,
                        const PrimitiveRegistry *reg, size_t max_samples) {
    size_t i;
    for (i = 0; i < n_laws; ++i) {
        PropertyReport pr;
        if (property_check(&laws[i], reg, max_samples, &pr) != 0 && pr.violated > 0) {
            return 1;
        }
    }
    return 0;
}

/* Steps 5-7 of the pipeline, shared by route and dag. Does NOT free student in
   any case -- the caller frees it when this returns 0 (not accepted). Returns 1
   if the chunk was registered and recorded, 0 otherwise. */
/* 3C: the teacher sub-plan's cost + primitive names, captured before the plan
   is freed (names copied by value so they survive dag_free/circuit_free). */
typedef struct {
    size_t mac;
    char   names[EXPAND_MAX_PRIMS][64];
    size_t count;   /* 0 when the teacher exceeds EXPAND_MAX_PRIMS -> no recipe */
} TeacherInfo;

/* Native-chunk admission through the one specialist door: wrap as a
   Specialist(kind=btn) and admit (certify + register + stamp kind). The
   low-level registry_add_certified stays an internal of the admission layer. */
static int admit_native_btn(PrimitiveRegistry *reg, BinaryTransformNetwork *btn,
                            const char *name, const Contract *c) {
    Specialist s;
    if (specialist_wrap_btn(&s, btn, name) != 0) return -1;
    return specialist_admit(reg, &s, c);
}

static int finalize_chunk(PrimitiveRegistry *reg, BinaryTransformNetwork *student,
                          const char *name, const Contract *c,
                          const Property *laws, size_t n_laws, size_t max_samples,
                          const TeacherInfo *ti, LibraryReport *report) {
    size_t before, idx, teacher_mac = ti->mac;

    if (report->chunk_count >= LIBRARY_MAX_CHUNKS) return 0;
    if (contract_already_known(reg, c)) return 0;          /* step 5: dedup */

    before = reg->count;
    if (admit_native_btn(reg, student, name, c) != 0) return 0;  /* step 6 */

    if (law_violated(laws, n_laws, reg, max_samples)) {    /* step 7: guard */
        /* rollback only if we actually appended; unique names mean
           registry_add_certified appends, but a replace would leave
           count == before, and removing then would drop a different entry */
        if (reg->count == before + 1) registry_remove_last(reg);
        report->rolled_back++;
        return 0;
    }

    idx = report->chunk_count;                             /* record */
    snprintf(report->names[idx], sizeof report->names[idx], "%s", name);
    report->chunks[idx] = student;
    {
        size_t student_mac = btn_cost(student);
        int beneficial = (student_mac < teacher_mac) ? 1 : 0;
        report->teacher_mac_estimate[idx] = teacher_mac;
        report->student_mac_estimate[idx] = student_mac;
        if (student_mac > 0)      report->compression_ratio[idx] = (double)teacher_mac / (double)student_mac;
        else if (teacher_mac > 0) report->compression_ratio[idx] = INFINITY;
        else                      report->compression_ratio[idx] = 1.0;
        report->compute_beneficial[idx] = beneficial;
        /* 3C: persist the cost truth on the chunk's entry; a compute-HEAVY chunk
           also stores its lean teacher recipe (names) for LOW-power plan-time
           expansion. Advisory -- a failure here never unwinds the mint. */
        {
            const char *ptrs[EXPAND_MAX_PRIMS];
            size_t k, n = beneficial ? 0 : ti->count;
            for (k = 0; k < n; ++k) ptrs[k] = ti->names[k];
            (void)registry_set_expansion(reg, name, n ? ptrs : NULL, n,
                                         teacher_mac, student_mac, beneficial);
        }
    }
    if (ti->count >= 1) {
        size_t k, n = ti->count;
        if (n > LIBRARY_TRACE_LEN) n = LIBRARY_TRACE_LEN;
        report->traces[idx].length = n;
        for (k = 0; k < n; ++k)
            snprintf(report->traces[idx].steps[k],
                     sizeof report->traces[idx].steps[k], "%s", ti->names[k]);
        report->trace_count = idx + 1;
    }
    report->chunk_count++;
    return 1;
}

typedef struct {
    const LibraryGateConfig *cfg;       /* NULL or cfg->enabled==0 -> off */
} GateState;

void library_gate_config_defaults(LibraryGateConfig *g) {
    if (g == NULL) return;
    g->enabled = 0; g->evidence_threshold = 0.9; g->min_evidence = 16;
}
static int gate_on(const GateState *gs) { return gs && gs->cfg && gs->cfg->enabled; }

/* Every primitive carries >= min_evidence outcomes AND reliability >= threshold
   (the lifecycle 0.9/16 promotion bar). NULL primitive -> not clear. */
static int btn_evidence_clear(const BinaryTransformNetwork *p,
                              double threshold, size_t min_evidence) {
    unsigned long ev;
    if (p == NULL) return 0;
    ev = (unsigned long)p->output_successes + (unsigned long)p->output_failures;
    return btn_reliability(p) >= threshold && ev >= min_evidence;
}
static int route_evidence_clear(const RoutePlan *plan, const GateState *gs) {
    size_t i;
    if (!gate_on(gs)) return 1;
    for (i = 0; i < plan->length; ++i)
        if (!btn_evidence_clear(plan->steps[i], gs->cfg->evidence_threshold,
                                gs->cfg->min_evidence)) return 0;
    return 1;
}
static int dag_evidence_clear(const DagPlan *plan, const GateState *gs) {
    size_t i;
    if (!gate_on(gs)) return 1;
    if (plan->owned == NULL)
        return (plan->root && plan->root->kind == DAG_PRIMITIVE && plan->root->btn)
            ? btn_evidence_clear(plan->root->btn, gs->cfg->evidence_threshold, gs->cfg->min_evidence)
            : 0;
    for (i = 0; i < plan->owned_count; ++i)
        if (plan->owned[i] && plan->owned[i]->kind == DAG_PRIMITIVE &&
            !btn_evidence_clear(plan->owned[i]->btn, gs->cfg->evidence_threshold,
                                gs->cfg->min_evidence)) return 0;
    return 1;
}

static void route_teacher_info(const RoutePlan *p, TeacherInfo *ti) {
    size_t i;
    ti->mac = 0;
    ti->count = 0;
    for (i = 0; i < p->length; ++i) ti->mac += btn_cost(p->steps[i]);
    if (p->length >= 1 && p->length <= EXPAND_MAX_PRIMS) {
        for (i = 0; i < p->length; ++i)
            (void)snprintf(ti->names[i], sizeof ti->names[i], "%s",
                           p->names[i] != NULL ? p->names[i] : "");
        ti->count = p->length;
    }
}
static void owned_teacher_info(DagNode *const *owned, size_t owned_count,
                               TeacherInfo *ti) {
    size_t i, np = 0;
    ti->mac = 0;
    ti->count = 0;
    if (owned == NULL) return;
    for (i = 0; i < owned_count; ++i)
        if (owned[i] != NULL && owned[i]->kind == DAG_PRIMITIVE) {
            ti->mac += btn_cost(owned[i]->btn);
            ++np;
        }
    if (np >= 1 && np <= EXPAND_MAX_PRIMS) {
        size_t w = 0;
        for (i = 0; i < owned_count; ++i)
            if (owned[i] != NULL && owned[i]->kind == DAG_PRIMITIVE) {
                (void)snprintf(ti->names[w], sizeof ti->names[w], "%s",
                               owned[i]->name != NULL ? owned[i]->name : "");
                ++w;
            }
        ti->count = np;
    }
}

/* Route task (single source): plan -> worth-it -> distill -> contract -> finalize. */
static int evolve_route(PrimitiveRegistry *reg, const LibraryTask *task,
                        const Property *laws, size_t n_laws,
                        const ConsolidateConfig *cfg, LibraryReport *report,
                        GateState *gs) {
    RoutePlan plan;
    BinaryTransformNetwork *student;
    Contract c = {0};
    int accepted;

    if (route_plan(reg, task->sources[0], task->goal, &plan) != 0) return 0;
    if (plan.length < 2) return 0;                         /* worth-it guard */
    /* DreamCoder Core type gate: refuse an ill-typed chain. Existing
       planners already match ports, so this is fail-closed, not a floor drop. */
    if (cnet_dc_route_well_typed(&plan, task->sources[0]) != 0) return 0;
    if (!route_evidence_clear(&plan, gs)) { report->deferred++; return 0; }

    student = calloc(1, sizeof *student);
    if (student == NULL) return 0;
    if (consolidate_route(&plan, cfg, student, NULL) != 0) {
        free(student);                                     /* refusal: arrays never allocated */
        return 0;
    }
    if (contract_from_route(&plan, task->name, cfg->max_samples, &c) != 0) {
        btn_free(student); free(student);
        return 0;
    }
    {
        TeacherInfo ti;
        route_teacher_info(&plan, &ti);
        accepted = finalize_chunk(reg, student, task->name, &c, laws, n_laws,
                                  cfg->max_samples, &ti, report);
    }
    contract_free(&c);
    if (!accepted) { btn_free(student); free(student); }
    return accepted;
}

/* Distinct primitive executions in a planner-built plan (sharing-aware via
   the owned table). The worth-it guard for dag tasks. */
static int count_dag_primitives(const DagPlan *p) {
    size_t i;
    int n = 0;
    if (p->owned != NULL) {
        for (i = 0; i < p->owned_count; ++i) {
            if (p->owned[i] != NULL && p->owned[i]->kind == DAG_PRIMITIVE) ++n;
        }
    } else if (p->root != NULL && p->root->kind == DAG_PRIMITIVE) {
        n = 1;  /* hand-built fallback; planner plans always set owned */
    }
    return n;
}

/* Dag task (>=2 sources): plan -> worth-it -> distill -> contract -> finalize.
   Source values are never read by dag_plan/consolidate_dag/contract_from_dag
   (they enumerate the canonical domain), so .values = NULL is correct. */
static int evolve_dag(PrimitiveRegistry *reg, const LibraryTask *task,
                      const Property *laws, size_t n_laws,
                      const ConsolidateConfig *cfg, LibraryReport *report,
                      GateState *gs) {
    DagSource sources[LIBRARY_MAX_SOURCES];
    DagPlan plan = {0};
    BinaryTransformNetwork *student;
    Contract c = {0};
    size_t i;
    int accepted;

    for (i = 0; i < task->n_sources; ++i) {
        sources[i].type = task->sources[i];
        sources[i].values = NULL;
    }
    if (dag_plan(reg, sources, task->n_sources, task->goal, &plan) != 0) return 0;
    if (count_dag_primitives(&plan) < 2) { dag_free(&plan); return 0; }
    if (cnet_dc_dag_well_typed(&plan, sources, task->n_sources, task->goal) != 0) {
        dag_free(&plan);
        return 0;
    }
    if (!dag_evidence_clear(&plan, gs)) { report->deferred++; dag_free(&plan); return 0; }

    student = calloc(1, sizeof *student);
    if (student == NULL) { dag_free(&plan); return 0; }
    if (consolidate_dag(&plan, sources, task->n_sources, cfg, student, NULL) != 0) {
        free(student); dag_free(&plan);
        return 0;
    }
    if (contract_from_dag(&plan, sources, task->n_sources, task->name,
                          cfg->max_samples, &c) != 0) {
        btn_free(student); free(student); dag_free(&plan);
        return 0;
    }
    {
        TeacherInfo ti;
        owned_teacher_info(plan.owned, plan.owned_count, &ti);
        dag_free(&plan);  /* finalize needs only reg, student, contract */
        accepted = finalize_chunk(reg, student, task->name, &c, laws, n_laws,
                                  cfg->max_samples, &ti, report);
    }
    contract_free(&c);
    if (!accepted) { btn_free(student); free(student); }
    return accepted;
}

/* Distinct primitive nodes in a circuit plan (sharing-aware: owned[] holds each
   node once). Worth-it guard + consolidate_circuit's ">= 2 distinct primitive
   executions" precondition. */
static int count_circuit_primitives(const CircuitPlan *p) {
    size_t i; int n = 0;
    if (p->owned == NULL) return 0;
    for (i = 0; i < p->owned_count; ++i)
        if (p->owned[i] != NULL && p->owned[i]->kind == DAG_PRIMITIVE) ++n;
    return n;
}

/* EVIDENCE_CLEAR for circuits: a FLAT loop over owned[] (the sharing-aware,
   deduplicated multi-root closure -- a node shared across roots appears exactly
   once, by the port-disjoint fan-out rule), reusing btn_evidence_clear. Not
   recursive; no sub-graph dedup needed -- owned[] already is the dedup. */
static int circuit_evidence_clear(const CircuitPlan *plan, const GateState *gs) {
    size_t i;
    if (!gate_on(gs)) return 1;
    for (i = 0; i < plan->owned_count; ++i)
        if (plan->owned[i] && plan->owned[i]->kind == DAG_PRIMITIVE &&
            !btn_evidence_clear(plan->owned[i]->btn, gs->cfg->evidence_threshold,
                                gs->cfg->min_evidence)) return 0;
    return 1;
}

/* Multi-output circuit distillation under the gate. Returns 1 if a chunk was
   minted, else 0. */
static int evolve_circuit(PrimitiveRegistry *reg, const LibraryTask *task,
                          const Property *laws, size_t n_laws,
                          const ConsolidateConfig *cfg, LibraryReport *report,
                          GateState *gs) {
    DagSource sources[LIBRARY_MAX_SOURCES];
    CircuitPlan plan;
    BinaryTransformNetwork *student;
    Contract c = {0};
    size_t i;
    int accepted;

    for (i = 0; i < task->n_sources; ++i) {
        sources[i].type = task->sources[i];
        sources[i].values = NULL;
    }
    memset(&plan, 0, sizeof plan);
    if (dag_plan_circuit(reg, sources, task->n_sources,
                         task->goals, task->n_goals, &plan) != 0) return 0;
    if (count_circuit_primitives(&plan) < 2) { circuit_free(&plan); return 0; }
    if (cnet_dc_circuit_well_typed(&plan, sources, task->n_sources,
                                  task->goals, task->n_goals) != 0) {
        circuit_free(&plan);
        return 0;
    }
    if (!circuit_evidence_clear(&plan, gs)) {
        report->deferred++; circuit_free(&plan); return 0;
    }

    student = calloc(1, sizeof *student);
    if (student == NULL) { circuit_free(&plan); return 0; }
    if (consolidate_circuit(&plan, sources, task->n_sources, cfg, student, NULL) != 0) {
        free(student); circuit_free(&plan); return 0;
    }
    if (contract_from_circuit(&plan, sources, task->n_sources, task->name,
                              cfg->max_samples, &c) != 0) {
        btn_free(student); free(student); circuit_free(&plan); return 0;
    }
    {
        TeacherInfo ti;
        owned_teacher_info(plan.owned, plan.owned_count, &ti);
        circuit_free(&plan);
        accepted = finalize_chunk(reg, student, task->name, &c, laws, n_laws,
                                  cfg->max_samples, &ti, report);
    }
    contract_free(&c);
    if (!accepted) { btn_free(student); free(student); }
    return accepted;
}

/* Dispatch by arity. */
static int try_consolidate(PrimitiveRegistry *reg, const LibraryTask *task,
                           const Property *laws, size_t n_laws,
                           const ConsolidateConfig *cfg, LibraryReport *report,
                           GateState *gs) {
    if (task->n_sources == 0 || task->n_sources > LIBRARY_MAX_SOURCES) {
        return 0;  /* malformed arity: nothing to do */
    }
    if (task->n_goals >= 2) {
        return evolve_circuit(reg, task, laws, n_laws, cfg, report, gs);
    }
    if (task->n_sources == 1) {
        return evolve_route(reg, task, laws, n_laws, cfg, report, gs);
    }
    if (task->n_sources >= 2) {
        return evolve_dag(reg, task, laws, n_laws, cfg, report, gs);
    }
    return 0;  /* n_sources == 0: nothing to do */
}

static int evolve_run(PrimitiveRegistry *reg, const LibraryTask *tasks, size_t n_tasks,
                      const Property *laws, size_t n_laws, const ConsolidateConfig *cfg,
                      GateState *gs, size_t max_iterations, LibraryReport *report) {
    ConsolidateConfig cfg_local; size_t iter, t;
    if (report == NULL) return 0;
    memset(report, 0, sizeof *report);
    if (cfg != NULL) cfg_local = *cfg; else consolidate_config_defaults(&cfg_local);
    for (iter = 0; iter < max_iterations; ++iter) {
        size_t added = 0; report->iterations_run = iter + 1;
        for (t = 0; t < n_tasks; ++t)
            added += (size_t)try_consolidate(reg, &tasks[t], laws, n_laws, &cfg_local, report, gs);
        if (added == 0) break;
    }
    if (report->trace_count >= 2)
        (void)library_sleep_compress(reg, report->traces, report->trace_count,
                                     laws, n_laws, &cfg_local, report);
    return 0;
}
int library_evolve(PrimitiveRegistry *reg,
                   const LibraryTask *tasks, size_t n_tasks,
                   const Property *laws, size_t n_laws,
                   const ConsolidateConfig *cfg,
                   size_t max_iterations,
                   LibraryReport *report) {
    return evolve_run(reg, tasks, n_tasks, laws, n_laws, cfg, NULL, max_iterations, report);
}
int library_evolve_gated(PrimitiveRegistry *reg,
                         const LibraryTask *tasks, size_t n_tasks,
                         const Property *laws, size_t n_laws,
                         const ConsolidateConfig *cfg,
                         const LibraryGateConfig *gate,
                         size_t max_iterations,
                         LibraryReport *report) {
    GateState gs; gs.cfg = gate;
    return evolve_run(reg, tasks, n_tasks, laws, n_laws, cfg, &gs, max_iterations, report);
}


static int trace_has_subseq(const LibraryTrace *tr, char steps[][CONTRACT_NAME_MAX],
                            size_t n) {
    size_t i, j;
    if (tr == NULL || steps == NULL || n == 0 || n > tr->length) return 0;
    for (i = 0; i + n <= tr->length; ++i) {
        int ok = 1;
        for (j = 0; j < n; ++j) {
            if (strcmp(tr->steps[i + j], steps[j]) != 0) {
                ok = 0;
                break;
            }
        }
        if (ok) return 1;
    }
    return 0;
}

static const BinaryTransformNetwork *reg_lookup(const PrimitiveRegistry *reg,
                                                const char *name) {
    size_t i;
    if (reg == NULL || name == NULL) return NULL;
    for (i = 0; i < reg->count; ++i) {
        if (reg->entries[i].name != NULL &&
            strcmp(reg->entries[i].name, name) == 0)
            return reg->entries[i].btn;
    }
    return NULL;
}

int library_sleep_compress(PrimitiveRegistry *reg,
                           const LibraryTrace *traces, size_t n_traces,
                           const Property *laws, size_t n_laws,
                           const ConsolidateConfig *cfg,
                           LibraryReport *report) {
    size_t t, len, off, best_len = 0, best_t = 0, best_off = 0;
    char brick[LIBRARY_TRACE_LEN][CONTRACT_NAME_MAX];
    int found = 0;
    RoutePlan plan;
    BinaryTransformNetwork *student;
    Contract c;
    TeacherInfo ti;
    ConsolidateConfig cfg_local;
    size_t i;
    int accepted;

    if (report != NULL && report->sleep_compressed) return 0;
    if (reg == NULL || traces == NULL || n_traces < 2 || report == NULL)
        return 0;
    if (cfg != NULL) cfg_local = *cfg;
    else consolidate_config_defaults(&cfg_local);

    /* Longest contiguous subsequence in >=2 traces, proper in at least one. */
    for (t = 0; t < n_traces; ++t) {
        if (traces[t].length < 2) continue;
        for (len = traces[t].length; len >= 2; --len) {
            if (len < best_len) break;
            for (off = 0; off + len <= traces[t].length; ++off) {
                size_t hits = 0, u, k;
                int proper = 0;
                for (k = 0; k < len; ++k)
                    snprintf(brick[k], sizeof brick[k], "%s",
                             traces[t].steps[off + k]);
                for (u = 0; u < n_traces; ++u) {
                    if (!trace_has_subseq(&traces[u], brick, len)) continue;
                    hits++;
                    if (len < traces[u].length) proper = 1;
                }
                if (hits >= 2 && proper && len > best_len) {
                    best_len = len;
                    best_t = t;
                    best_off = off;
                    found = 1;
                }
            }
        }
    }
    if (!found) return 0;
    for (i = 0; i < best_len; ++i)
        snprintf(brick[i], sizeof brick[i], "%s",
                 traces[best_t].steps[best_off + i]);
    /* CSE itself is sleep. Distill below is optional and still fail-closed. */
    snprintf(report->sleep_brick, sizeof report->sleep_brick, "%s",
             "shared_subplan");
    report->sleep_compressed = 1;

    memset(&plan, 0, sizeof plan);
    if (best_len > ROUTE_MAX_STEPS) return 0;
    for (i = 0; i < best_len; ++i) {
        const BinaryTransformNetwork *btn = reg_lookup(reg, brick[i]);
        if (btn == NULL || btn->input_port_count != 1 ||
            btn->output_port_count == 0)
            return 0; /* not a unary reconstructable route */
        plan.steps[i] = btn;
        plan.names[i] = brick[i];
    }
    plan.length = best_len;
    plan.goal = plan.steps[best_len - 1]->output_ports[0];
    if (cnet_dc_route_well_typed(&plan, plan.steps[0]->input_ports[0]) != 0)
        return 0;

    student = calloc(1, sizeof *student);
    if (student == NULL) return 0;
    if (consolidate_route(&plan, &cfg_local, student, NULL) != 0) {
        free(student);
        return 0;
    }
    memset(&c, 0, sizeof c);
    if (contract_from_route(&plan, "sleep_brick", cfg_local.max_samples, &c) !=
        0) {
        btn_free(student);
        free(student);
        return 0;
    }
    route_teacher_info(&plan, &ti);
    accepted = finalize_chunk(reg, student, "sleep_brick", &c, laws, n_laws,
                              cfg_local.max_samples, &ti, report);
    contract_free(&c);
    if (!accepted) {
        btn_free(student);
        free(student);
        return 0;
    }
    return 0;
}

void library_report_free(LibraryReport *report) {
    size_t i;
    if (report == NULL) return;
    for (i = 0; i < report->chunk_count; ++i) {
        if (report->chunks[i] != NULL) {
            /* chunks[i] is heap-allocated by library_evolve; btn_free releases
               its internal buffers, then free releases the struct itself. */
            btn_free(report->chunks[i]);
            free(report->chunks[i]);
            report->chunks[i] = NULL;
        }
    }
    report->chunk_count = 0;
}
