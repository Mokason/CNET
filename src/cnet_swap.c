/* Unit-tested swap law. Live doors: registry_add_certified (same-name)
   and library_evolve / library_admit_candidate call cnet_swap_admit.
   src/cnet_swap.c is in LIBRARY so cnet.so contains the law.
   Replace only when the new brick dominates old coverage and every CERT
   composition that used the old brick still passes hop guards.
   n_comps==0 is not a composition proof (ADD-ALONGSIDE, never REPLACE).
   Otherwise add-alongside. Compression and soft routers are not signals.
   Teacher/residual adapters never admit. */
#include "../include/cnet_swap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void report_set(CnetSwapReport *r, CnetSwapVerdict v, const char *why,
                       int dominated, int comps) {
    if (r == NULL) return;
    r->verdict = v;
    r->reason = why;
    r->dominated = dominated;
    r->compositions_hold = comps;
}

static int cov_ok(const CnetSwapCoverage *c) {
    return c != NULL && c->inputs != NULL && c->n_rows > 0 && c->in_dim > 0;
}

static int row_equal(const double *a, const double *b, size_t n) {
    return memcmp(a, b, n * sizeof(double)) == 0;
}

int cnet_swap_coverage_subset(const CnetSwapCoverage *old_cov,
                              const CnetSwapCoverage *new_cov) {
    size_t i, j;
    int found;

    if (old_cov == NULL || new_cov == NULL) return -1;
    if (old_cov->n_rows == 0 || old_cov->inputs == NULL || old_cov->in_dim == 0)
        return 0;
    if (new_cov->inputs == NULL || new_cov->n_rows == 0 || new_cov->in_dim == 0)
        return 0;
    if (old_cov->in_dim != new_cov->in_dim) return 0;
    if (old_cov->targets != NULL && new_cov->targets != NULL &&
        old_cov->out_dim != new_cov->out_dim) return 0;

    for (i = 0; i < old_cov->n_rows; ++i) {
        const double *oi = old_cov->inputs + i * old_cov->in_dim;
        found = 0;
        for (j = 0; j < new_cov->n_rows; ++j) {
            const double *ni = new_cov->inputs + j * new_cov->in_dim;
            if (!row_equal(oi, ni, old_cov->in_dim)) continue;
            if (old_cov->targets != NULL && new_cov->targets != NULL) {
                const double *ot = old_cov->targets + i * old_cov->out_dim;
                const double *nt = new_cov->targets + j * new_cov->out_dim;
                if (!row_equal(ot, nt, old_cov->out_dim)) continue;
            }
            found = 1;
            break;
        }
        if (!found) return 0;
    }
    return 1;
}

int cnet_swap_matches_old_rows(const BinaryTransformNetwork *new_btn,
                               const CnetSwapCoverage *old_cov) {
    Contract c;
    int rc;

    if (new_btn == NULL || !cov_ok(old_cov) || old_cov->targets == NULL)
        return -1;
    if (old_cov->in_dim != new_btn->input_count ||
        old_cov->out_dim != new_btn->output_count)
        return 0;
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, "swap_old_cov", new_btn,
                               old_cov->inputs, old_cov->targets,
                               old_cov->n_rows) != 0) {
        return 0;
    }
    rc = btn_certify((BinaryTransformNetwork *)new_btn, &c, NULL);
    contract_free(&c);
    return rc == 0 ? 1 : 0;
}

int cnet_swap_dominates(const BinaryTransformNetwork *new_btn,
                        const CnetSwapCoverage *old_cov,
                        const CnetSwapCoverage *new_cov) {
    int have_table, have_replay;
    int table_ok = 0, replay_ok = 0;

    if (old_cov == NULL) return -1;
    if (!cov_ok(old_cov)) return 0;

    have_table = (new_cov != NULL);
    have_replay = (new_btn != NULL && old_cov->targets != NULL);
    if (!have_table && !have_replay) return 0;

    if (have_table) {
        table_ok = (cnet_swap_coverage_subset(old_cov, new_cov) == 1);
        if (!table_ok && !have_replay) return 0;
    }
    if (have_replay) {
        replay_ok = (cnet_swap_matches_old_rows(new_btn, old_cov) == 1);
        if (!replay_ok && !have_table) return 0;
    }
    if (have_table && have_replay) return (table_ok && replay_ok) ? 1 : 0;
    if (have_table) return table_ok ? 1 : 0;
    return replay_ok ? 1 : 0;
}

int cnet_swap_hop_allow(const char *unit, const BinaryTransformNetwork *btn,
                        const double *input, size_t in_len, void *ctx) {
    const CnetSwapCoverage *cov = (const CnetSwapCoverage *)ctx;
    size_t i;
    (void)unit;
    (void)btn;
    if (cov == NULL || input == NULL || !cov_ok(cov) || in_len != cov->in_dim)
        return 1;
    for (i = 0; i < cov->n_rows; ++i) {
        if (row_equal(cov->inputs + i * cov->in_dim, input, cov->in_dim))
            return 0;
    }
    return 1;
}

static int step_is_old(const RoutePlan *plan, size_t s,
                       const BinaryTransformNetwork *old_btn,
                       const char *old_name) {
    if (old_btn != NULL && plan->steps[s] == old_btn) return 1;
    if (old_name != NULL && old_name[0] != '\0' &&
        plan->names[s] != NULL && strcmp(plan->names[s], old_name) == 0)
        return 1;
    return 0;
}

int cnet_swap_compositions_hold(const BinaryTransformNetwork *old_btn,
                                const char *old_name,
                                const BinaryTransformNetwork *new_btn,
                                const CnetSwapComposition *comps,
                                size_t n_comps,
                                const DagNodeGuard *guard) {
    size_t c, r, s;
    RoutePlan plan;
    double *out = NULL;
    size_t out_cap = 0;
    int ok = 1;

    if (new_btn == NULL) return -1;
    if (n_comps == 0) return 0; /* not a composition proof */
    if (comps == NULL) return -1;

    for (c = 0; c < n_comps && ok; ++c) {
        size_t need;
        if (comps[c].plan.length == 0 || comps[c].plan.length > ROUTE_MAX_STEPS)
            return 0;
        if (comps[c].inputs == NULL || comps[c].n_rows == 0 ||
            comps[c].in_dim == 0)
            return 0;
        plan = comps[c].plan;
        for (s = 0; s < plan.length; ++s) {
            if (step_is_old(&plan, s, old_btn, old_name))
                plan.steps[s] = new_btn;
        }
        need = plan.steps[plan.length - 1]->output_count;
        if (need == 0) return 0;
        if (need > out_cap) {
            double *grown = (double *)realloc(out, need * sizeof(double));
            if (grown == NULL) {
                free(out);
                return -1;
            }
            out = grown;
            out_cap = need;
        }
        for (r = 0; r < comps[c].n_rows; ++r) {
            int rc = route_execute_guarded(
                &plan,
                comps[c].inputs + r * comps[c].in_dim,
                comps[c].in_dim,
                out, out_cap, NULL, guard);
            if (rc != 0) {
                ok = 0;
                break;
            }
        }
    }
    free(out);
    return ok ? 1 : 0;
}

static int decide_inner(const BinaryTransformNetwork *old_btn,
                        const char *old_name,
                        const BinaryTransformNetwork *new_btn,
                        const CnetSwapCoverage *old_cov,
                        const CnetSwapCoverage *new_cov,
                        const CnetSwapComposition *comps, size_t n_comps,
                        const DagNodeGuard *guard,
                        CnetSwapReport *report) {
    int dom, hold;

    if (new_btn == NULL || old_cov == NULL) {
        report_set(report, CNET_SWAP_REFUSE, "bad_args", 0, 0);
        return -1;
    }
    if (btn_is_adapter(new_btn)) {
        report_set(report, CNET_SWAP_REFUSE, "teacher_residual_never_admits",
                   0, 0);
        return -1;
    }
    dom = cnet_swap_dominates(new_btn, old_cov, new_cov);
    if (dom < 0) {
        report_set(report, CNET_SWAP_REFUSE, "bad_args", 0, 0);
        return -1;
    }
    if (n_comps == 0) {
        report_set(report, CNET_SWAP_ADD_ALONGSIDE,
                   dom ? "no_compositions_to_prove" : "does_not_dominate",
                   dom ? 1 : 0, 0);
        return 0;
    }
    hold = cnet_swap_compositions_hold(old_btn, old_name, new_btn,
                                       comps, n_comps, guard);
    if (hold < 0) {
        report_set(report, CNET_SWAP_REFUSE, "bad_args", dom, 0);
        return -1;
    }

    if (dom && hold) {
        report_set(report, CNET_SWAP_REPLACE, "dominate_and_compositions_hold",
                   1, 1);
        return 0;
    }
    if (!dom && hold) {
        report_set(report, CNET_SWAP_ADD_ALONGSIDE, "does_not_dominate",
                   0, 1);
        return 0;
    }
    if (dom && !hold) {
        report_set(report, CNET_SWAP_ADD_ALONGSIDE,
                   "compositions_would_break", 1, 0);
        return 0;
    }
    report_set(report, CNET_SWAP_ADD_ALONGSIDE,
               "no_dominate_and_compositions_break", 0, 0);
    return 0;
}

int cnet_swap_decide(const BinaryTransformNetwork *old_btn,
                     const char *old_name,
                     const BinaryTransformNetwork *new_btn,
                     const CnetSwapCoverage *old_cov,
                     const CnetSwapCoverage *new_cov,
                     const CnetSwapComposition *comps, size_t n_comps,
                     const DagNodeGuard *guard,
                     CnetSwapReport *report) {
    return decide_inner(old_btn, old_name, new_btn, old_cov, new_cov,
                        comps, n_comps, guard, report);
}

static size_t find_named(const PrimitiveRegistry *reg, const char *name) {
    size_t i;
    if (reg == NULL || name == NULL) return (size_t)-1;
    for (i = 0; i < reg->count; ++i) {
        if (reg->entries[i].name != NULL &&
            strcmp(reg->entries[i].name, name) == 0)
            return i;
    }
    return (size_t)-1;
}

static int stamp_certified(RegistryEntry *e, BinaryTransformNetwork *btn) {
    e->btn = btn;
    e->certified = 1;
    e->cert_btn_digest = contract_btn_digest(btn);
    e->state = PRIM_FROZEN;
    return 0;
}

static int apply_replace(PrimitiveRegistry *reg, const char *old_name,
                         BinaryTransformNetwork *new_btn, const Contract *new_c) {
    size_t idx;
    if (reg == NULL || old_name == NULL || new_btn == NULL || new_c == NULL)
        return -1;
    if (btn_certify(new_btn, new_c, NULL) != 0) return -1;
    idx = find_named(reg, old_name);
    if (idx == (size_t)-1) return -1;
    if (registry_store_cert_coverage(&reg->entries[idx], new_c) != 0)
        return -1;
    stamp_certified(&reg->entries[idx], new_btn);
    return 0;
}

static int apply_alongside(PrimitiveRegistry *reg, const char *old_name,
                           const char *alongside_name,
                           BinaryTransformNetwork *new_btn,
                           const Contract *new_c) {
    size_t before, idx;
    if (reg == NULL || alongside_name == NULL || alongside_name[0] == '\0' ||
        new_btn == NULL || new_c == NULL)
        return -1;
    if (old_name != NULL && strcmp(old_name, alongside_name) == 0)
        return -1;
    if (find_named(reg, alongside_name) != (size_t)-1) return -1;
    before = reg->count;
    if (registry_add_certified(reg, new_btn, alongside_name, new_c) != 0)
        return -1;
    /* registry_add_certified appends on a new name. A same-name replace
       would be a bug here — refuse to pretend we added alongside. */
    if (reg->count != before + 1) return -1;
    idx = find_named(reg, old_name);
    if (old_name != NULL && idx == (size_t)-1) return -1;
    (void)idx;
    return 0;
}

int cnet_swap_admit(PrimitiveRegistry *reg,
                    const char *old_name,
                    BinaryTransformNetwork *new_btn,
                    const char *alongside_name,
                    const Contract *new_c,
                    const CnetSwapCoverage *old_cov,
                    const CnetSwapCoverage *new_cov,
                    const CnetSwapComposition *comps, size_t n_comps,
                    const DagNodeGuard *guard,
                    CnetSwapReport *report) {
    CnetSwapReport local;
    const BinaryTransformNetwork *old_btn = NULL;
    size_t idx;
    int rc;

    if (report == NULL) report = &local;
    if (reg == NULL || old_name == NULL || new_c == NULL) {
        report_set(report, CNET_SWAP_REFUSE, "bad_args", 0, 0);
        return -1;
    }
    if (btn_is_adapter(new_btn)) {
        report_set(report, CNET_SWAP_REFUSE, "teacher_residual_never_admits",
                   0, 0);
        return -1;
    }
    idx = find_named(reg, old_name);
    if (idx != (size_t)-1) old_btn = reg->entries[idx].btn;

    rc = decide_inner(old_btn, old_name, new_btn, old_cov, new_cov,
                      comps, n_comps, guard, report);
    if (rc != 0) return -1;
    if (report->verdict == CNET_SWAP_REFUSE) return -1;

    if (report->verdict == CNET_SWAP_REPLACE) {
        if (apply_replace(reg, old_name, new_btn, new_c) != 0) {
            report_set(report, CNET_SWAP_REFUSE, "replace_apply_failed",
                       report->dominated, report->compositions_hold);
            return -1;
        }
        return 0;
    }

    if (apply_alongside(reg, old_name, alongside_name, new_btn, new_c) != 0) {
        report_set(report, CNET_SWAP_REFUSE, "alongside_apply_failed",
                   report->dominated, report->compositions_hold);
        return -1;
    }
    report->verdict = CNET_SWAP_ADD_ALONGSIDE;
    return 0;
}

int cnet_swap_replace(PrimitiveRegistry *reg,
                      const char *old_name,
                      BinaryTransformNetwork *new_btn,
                      const Contract *new_c,
                      const CnetSwapCoverage *old_cov,
                      const CnetSwapCoverage *new_cov,
                      const CnetSwapComposition *comps, size_t n_comps,
                      const DagNodeGuard *guard,
                      CnetSwapReport *report) {
    CnetSwapReport local;
    const BinaryTransformNetwork *old_btn = NULL;
    size_t idx;
    int rc;

    if (report == NULL) report = &local;
    if (reg == NULL || old_name == NULL || new_c == NULL) {
        report_set(report, CNET_SWAP_REFUSE, "bad_args", 0, 0);
        return -1;
    }
    if (btn_is_adapter(new_btn)) {
        report_set(report, CNET_SWAP_REFUSE, "teacher_residual_never_admits",
                   0, 0);
        return -1;
    }
    idx = find_named(reg, old_name);
    if (idx != (size_t)-1) old_btn = reg->entries[idx].btn;

    rc = decide_inner(old_btn, old_name, new_btn, old_cov, new_cov,
                      comps, n_comps, guard, report);
    if (rc != 0) return -1;
    if (report->verdict != CNET_SWAP_REPLACE) {
        report->verdict = CNET_SWAP_REFUSE;
        if (report->reason == NULL || report->reason[0] == '\0' ||
            strcmp(report->reason, "dominate_and_compositions_hold") == 0)
            report->reason = "replace_refused";
        if (!report->compositions_hold &&
            (report->reason == NULL ||
             strstr(report->reason, "teacher") == NULL))
            report->reason = "swap_would_break_composition";
        return -1;
    }
    if (apply_replace(reg, old_name, new_btn, new_c) != 0) {
        report_set(report, CNET_SWAP_REFUSE, "replace_apply_failed",
                   report->dominated, report->compositions_hold);
        return -1;
    }
    return 0;
}

/* ---- live-door bind / hook ------------------------------------------------ */

static const CnetSwapComposition *g_door_comps;
static size_t g_door_n_comps;
static const DagNodeGuard *g_door_guard;

void cnet_swap_bind_compositions(const CnetSwapComposition *comps, size_t n_comps,
                                 const DagNodeGuard *guard) {
    g_door_comps = comps;
    g_door_n_comps = n_comps;
    g_door_guard = guard;
}

void cnet_swap_unbind_compositions(void) {
    g_door_comps = NULL;
    g_door_n_comps = 0;
    g_door_guard = NULL;
}

void cnet_swap_bound_compositions(const CnetSwapComposition **comps, size_t *n_comps,
                                  const DagNodeGuard **guard) {
    if (comps != NULL) *comps = g_door_comps;
    if (n_comps != NULL) *n_comps = g_door_n_comps;
    if (guard != NULL) *guard = g_door_guard;
}

int cnet_swap_old_cov_from_entry(CnetSwapCoverage *cov, const RegistryEntry *e) {
    if (cov == NULL || e == NULL || e->cert_cov == NULL) return -1;
    if (e->cert_cov->inputs == NULL || e->cert_cov->n_rows == 0 ||
        e->cert_cov->in_dim == 0)
        return -1;
    cov->inputs = e->cert_cov->inputs;
    cov->targets = e->cert_cov->targets;
    cov->n_rows = e->cert_cov->n_rows;
    cov->in_dim = e->cert_cov->in_dim;
    cov->out_dim = e->cert_cov->out_dim;
    return 0;
}

int cnet_swap_cov_from_contract(CnetSwapCoverage *cov, const Contract *c) {
    size_t i, in_dim = 0, out_dim = 0;
    if (cov == NULL || c == NULL || c->inputs == NULL || c->exemplar_count == 0)
        return -1;
    for (i = 0; i < c->input_port_count; ++i)
        in_dim += c->input_ports[i].field_width * c->input_ports[i].field_count;
    for (i = 0; i < c->output_port_count; ++i)
        out_dim += c->output_ports[i].field_width * c->output_ports[i].field_count;
    if (in_dim == 0) return -1;
    cov->inputs = c->inputs;
    cov->targets = c->outputs;
    cov->n_rows = c->exemplar_count;
    cov->in_dim = in_dim;
    cov->out_dim = out_dim;
    return 0;
}

const char *cnet_swap_alongside_name(const PrimitiveRegistry *reg,
                                     const char *old_name) {
    unsigned n;
    if (old_name == NULL || old_name[0] == '\0') return NULL;
    for (n = 2; n < 100; ++n) {
        char *buf = (char *)malloc(CONTRACT_NAME_MAX);
        if (buf == NULL) return NULL;
        if (snprintf(buf, CONTRACT_NAME_MAX, "%s_v%u", old_name, n) < 0) {
            free(buf);
            return NULL;
        }
        if (find_named(reg, buf) == (size_t)-1)
            return buf;
        free(buf);
    }
    return NULL;
}

int cnet_swap_registry_hook(PrimitiveRegistry *reg,
                            BinaryTransformNetwork *new_btn,
                            const char *name,
                            const Contract *new_c) {
    CnetSwapCoverage old_cov, new_cov;
    CnetSwapReport rep;
    const CnetSwapComposition *comps = NULL;
    size_t n_comps = 0;
    const DagNodeGuard *guard = NULL;
    const char *alongside;
    size_t idx;

    if (reg == NULL || new_btn == NULL || name == NULL || new_c == NULL)
        return -1;
    if (btn_is_adapter(new_btn)) return -1;
    if (cnet_swap_cov_from_contract(&new_cov, new_c) != 0) return -1;
    idx = find_named(reg, name);
    if (idx == (size_t)-1) return -1;
    /* old_cov is the persisted incumbent table, never the incoming table.
       Same table both sides is a dominate lie. No persisted table =>
       empty old_cov => cannot REPLACE (ADD-ALONGSIDE / no_old_coverage). */
    if (cnet_swap_old_cov_from_entry(&old_cov, &reg->entries[idx]) != 0)
        memset(&old_cov, 0, sizeof old_cov);
    cnet_swap_bound_compositions(&comps, &n_comps, &guard);
    alongside = cnet_swap_alongside_name(reg, name);
    if (alongside == NULL) return -1;
    memset(&rep, 0, sizeof rep);
    return cnet_swap_admit(reg, name, new_btn, alongside, new_c,
                           &old_cov, &new_cov, comps, n_comps, guard, &rep);
}
