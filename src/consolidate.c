/*
 * Chunk consolidation: distill a proven route/DAG plan into a new single
 * primitive. The plan is only ever used as a STRICT teacher -- enumerate the
 * canonical input domain, label each input by executing the plan, train a
 * student on the labels, verify the student against them, and seed the
 * student's reliability counters with the verification outcome. The result
 * is a regular primitive; the planner/executor core is untouched.
 */
#include "../include/consolidate.h"

#include "../include/arena.h"
#include "../include/plan_table.h"

#include <stdlib.h>
#include <string.h>

void consolidate_config_defaults(ConsolidateConfig *cfg) {
    if (cfg == NULL) {
        return;
    }
    cfg->initial_hidden = 0;  /* auto: min(max_hidden, max(in, out) totals) */
    cfg->max_hidden = 128;
    cfg->learning_rate = 0.8;
    cfg->max_epochs = 160000;
    cfg->growth_window = 1000;
    cfg->target_loss = 0.0015;
    cfg->min_improvement = 0.01;
    cfg->seed = 131u;
    cfg->min_verify_rate = 1.0;
    cfg->max_samples = 4096;
}

/* ---- shared distillation core ------------------------------------------ */

static int consolidate_core(
    BinaryTransformNetwork *const *members,
    size_t n_members,
    const Port *in_ports,
    size_t n_in,
    const Port *out_ports,
    size_t n_out,
    PlanTeacherFn teacher,
    void *ctx,
    const ConsolidateConfig *cfg_opt,
    BinaryTransformNetwork *out_student,
    ConsolidateReport *report
) {
    ConsolidateConfig cfg;
    ConsolidateReport rep;
    PlanTable table;
    double *clean = NULL;
    BinaryTransformNetwork student;
    Arena arena = {0};
    size_t out_total = 0;
    size_t s;
    int rc = -1;

    for (s = 0; s < n_out; ++s) {
        out_total += plan_port_total(out_ports[s]);
    }

    if (cfg_opt != NULL) {
        cfg = *cfg_opt;
    } else {
        consolidate_config_defaults(&cfg);
    }
    memset(&rep, 0, sizeof rep);
    memset(&student, 0, sizeof student);
    memset(&table, 0, sizeof table);

    if (plan_table_build(in_ports, n_in, out_total, teacher, ctx,
                         members, n_members, cfg.max_samples, &table) != 0) {
        goto done;
    }
    rep.samples = table.kept;
    rep.teacher_aborts = table.aborts;
    if (table.kept == 0) {
        goto done;
    }

    arena_init(&arena);
    clean = arena_alloc(&arena, out_total * sizeof *clean);
    if (clean == NULL) {
        goto done;
    }

    /* initial_hidden 0 = auto. A student must NOT start from the usual
       1-neuron seed: during the first growth windows every input projects
       through that bottleneck, the output layer saturates on a low-rank
       approximation, and squared-error sigmoid gradients (~sigma' at the
       rails) are too weak to repair the confidently-wrong bits afterwards
       -- seed-dependent refusals in practice. Starting at the task's width
       skips the bottleneck era entirely (measured: 24/24 seeds verify vs
       ~1/8 from a 1-neuron start). */
    {
        size_t initial = cfg.initial_hidden;

        if (initial == 0) {
            initial = table.in_total > out_total ? table.in_total : out_total;
            if (initial > cfg.max_hidden) {
                initial = cfg.max_hidden;
            }
        }
        if (btn_init(&student, table.in_total, out_total, initial,
                     cfg.max_hidden, cfg.learning_rate, cfg.seed) != 0) {
            goto done;
        }
    }
    if (btn_set_io_ports(&student, in_ports, n_in, out_ports, n_out) != 0) {
        btn_free(&student);
        goto done;
    }

    /* Deep distillation now supports CCE path too (via adapt after student init) */
    rep.final_loss = btn_train_dynamic(&student, table.inputs, table.targets,
                                       table.kept, cfg.max_epochs,
                                       cfg.growth_window, cfg.target_loss,
                                       cfg.min_improvement);

    /* Verification: the student alone must reproduce the teacher, and its
       RAW output must be in-domain on EVERY segment -- the same bar the
       executors score, so the seeded counters mean what deployment
       counters mean. */
    for (s = 0; s < table.kept; ++s) {
        const double *raw = btn_forward(&student,
                                        table.inputs + s * table.in_total);
        const double *want = table.targets + s * out_total;
        int ok = raw != NULL;
        size_t off = 0;
        size_t op;

        for (op = 0; ok && op < n_out; ++op) {
            size_t tot = plan_port_total(out_ports[op]);
            size_t i;

            if (!port_validate(out_ports[op], raw + off) ||
                port_canonicalize(out_ports[op], raw + off, clean) != 0) {
                ok = 0;
                break;
            }
            for (i = 0; i < tot; ++i) {
                if (clean[i] != want[off + i]) {
                    ok = 0;
                    break;
                }
            }
            off += tot;
        }
        if (ok) {
            ++rep.verified;
        } else {
            ++rep.missed;
        }
    }

    if ((double)rep.verified < cfg.min_verify_rate * (double)table.kept) {
        btn_free(&student);
        goto done;
    }

#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
    atomic_store_explicit(&student.output_successes, rep.verified, memory_order_relaxed);
    atomic_store_explicit(&student.output_failures, rep.missed, memory_order_relaxed);
#else
    student.output_successes = rep.verified;
    student.output_failures = rep.missed;
#endif
    *out_student = student;
    rc = 0;

done:
    if (report != NULL) {
        *report = rep;
    }
    plan_table_free(&table);
    arena_reset(&arena);
    return rc;
}

/* ---- route plans -------------------------------------------------------- */

int consolidate_route(
    const RoutePlan *plan,
    const ConsolidateConfig *cfg,
    BinaryTransformNetwork *out_student,
    ConsolidateReport *report
) {
    RoutePlan teacher;
    Port in_port;
    Port out_port;
    size_t i;

    if (report != NULL) {
        memset(report, 0, sizeof *report);
    }
    /* A 1-step plan is already a primitive; 0 steps is the identity. */
    if (plan == NULL || out_student == NULL || plan->length < 2) {
        return -1;
    }
    for (i = 0; i < plan->length; ++i) {
        if (plan->steps[i] == NULL) {
            return -1;
        }
    }

    in_port = plan->steps[0]->input_ports[0];
    out_port = plan->steps[plan->length - 1]->output_ports[0];

    teacher = *plan;
    teacher.strict = 1;

    return consolidate_core(
        (BinaryTransformNetwork *const *)plan->steps, plan->length,
        &in_port, 1, &out_port, 1,
        plan_route_teacher, &teacher,
        cfg, out_student, report);
}

/* ---- DAG plans ----------------------------------------------------------- */

int consolidate_dag(
    const DagPlan *plan,
    const DagSource *sources,
    size_t n_sources,
    const ConsolidateConfig *cfg,
    BinaryTransformNetwork *out_student,
    ConsolidateReport *report
) {
    PlanDagTeacherCtx ctx;
    BinaryTransformNetwork **members = NULL;
    Arena arena;
    Port in_ports[DAG_MAX_SLOTS];
    size_t offsets[DAG_MAX_SLOTS];
    Port out_port;
    size_t n_members;
    size_t offset = 0;
    size_t i;
    int rc;

    if (report != NULL) {
        memset(report, 0, sizeof *report);
    }
    if (plan == NULL || out_student == NULL || sources == NULL ||
        n_sources == 0 || n_sources > DAG_MAX_SLOTS ||
        plan->root == NULL || plan->root->kind != DAG_PRIMITIVE ||
        plan->root->btn == NULL) {
        return -1;
    }

    /* The chunk's arity must tell the truth: every declared source consumed
       exactly once (dag_plan already guarantees at most once). */
    {
        size_t counts[DAG_MAX_SLOTS] = {0};

        if (plan_dag_count_sources(plan->root, counts, n_sources) != 0) {
            return -1;
        }
        for (i = 0; i < n_sources; ++i) {
            if (counts[i] != 1) {
                return -1;
            }
        }
    }

    n_members = plan_dag_collect_members(plan->root, NULL, 0, 0);
    /* One primitive is already a primitive. */
    if (n_members < 2) {
        return -1;
    }
    arena_init(&arena);
    members = arena_alloc(&arena, n_members * sizeof *members);
    if (members == NULL) {
        arena_reset(&arena);
        return -1;
    }
    plan_dag_collect_members(plan->root, members, n_members, 0);

    for (i = 0; i < n_sources; ++i) {
        in_ports[i] = sources[i].type;
        offsets[i] = offset;
        offset += plan_port_total(sources[i].type);
    }
    out_port = plan->root->btn->output_ports[plan->root->output_index];

    memset(&ctx.teacher, 0, sizeof ctx.teacher); /* owned stays NULL: borrowed */
    ctx.teacher.root = plan->root;
    ctx.teacher.strict = 1;
    ctx.declared = sources;
    ctx.n_sources = n_sources;
    ctx.offsets = offsets;

    rc = consolidate_core(
        (BinaryTransformNetwork *const *)members, n_members,
        in_ports, n_sources, &out_port, 1,
        plan_dag_teacher, &ctx,
        cfg, out_student, report);

    arena_reset(&arena);
    return rc;
}

/* ---- circuit plans -------------------------------------------------------- */

int consolidate_circuit(
    const CircuitPlan *plan,
    const DagSource *sources,
    size_t n_sources,
    const ConsolidateConfig *cfg,
    BinaryTransformNetwork *out_student,
    ConsolidateReport *report
) {
    PlanCircuitTeacherCtx ctx;
    BinaryTransformNetwork **members = NULL;
    Arena arena;
    Port in_ports[DAG_MAX_SLOTS];
    size_t offsets[DAG_MAX_SLOTS];
    Port out_ports[CIRCUIT_MAX_ROOTS];
    size_t n_members;
    size_t offset = 0;
    size_t i;
    int rc;

    if (report != NULL) {
        memset(report, 0, sizeof *report);
    }
    if (plan == NULL || out_student == NULL || sources == NULL ||
        n_sources == 0 || n_sources > DAG_MAX_SLOTS ||
        plan->root_count == 0 || plan->root_count > CIRCUIT_MAX_ROOTS) {
        return -1;
    }
    for (i = 0; i < plan->root_count; ++i) {
        const DagNode *root = plan->roots[i];

        /* Bare-source roots are pass-throughs, not behavior to distill. */
        if (root == NULL || root->kind != DAG_PRIMITIVE || root->btn == NULL ||
            (size_t)plan->root_ports[i] >= root->btn->output_port_count) {
            return -1;
        }
        out_ports[i] = root->btn->output_ports[plan->root_ports[i]];
    }

    /* The chunk's arity must tell the truth: every declared source
       referenced exactly once, counted sharing-aware. */
    {
        size_t counts[DAG_MAX_SLOTS] = {0};

        if (plan_circuit_count_sources(plan, counts, n_sources) != 0) {
            return -1;
        }
        for (i = 0; i < n_sources; ++i) {
            if (counts[i] != 1) {
                return -1;
            }
        }
    }

    /* Distinct executions: one primitive is already a primitive. */
    n_members = plan_circuit_collect_members(plan, NULL, 0);
    if (n_members < 2) {
        return -1;
    }
    arena_init(&arena);
    members = arena_alloc(&arena, n_members * sizeof *members);
    if (members == NULL) {
        arena_reset(&arena);
        return -1;
    }
    plan_circuit_collect_members(plan, members, n_members);

    for (i = 0; i < n_sources; ++i) {
        in_ports[i] = sources[i].type;
        offsets[i] = offset;
        offset += plan_port_total(sources[i].type);
    }

    memset(&ctx, 0, sizeof ctx);
    ctx.teacher = *plan;     /* borrowed roots; never freed through ctx */
    ctx.teacher.strict = 1;
    ctx.declared = sources;
    ctx.n_sources = n_sources;
    ctx.offsets = offsets;

    rc = consolidate_core(
        (BinaryTransformNetwork *const *)members, n_members,
        in_ports, n_sources, out_ports, plan->root_count,
        plan_circuit_teacher, &ctx,
        cfg, out_student, report);

    arena_reset(&arena);
    return rc;
}
