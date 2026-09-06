#include "cnet_capsule_core.h"
#include "cnet_capsule.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define CORE_STATES 1024u
#define CORE_CAPSULES 4096u
#define CORE_COVERAGE_BANKS ((CORE_CAPSULES + HYBRID_COVERAGE_MAX - 1) / HYBRID_COVERAGE_MAX)
typedef struct { size_t left; struct timespec start; double seconds; } CoreBudget;
static void budget_init(CoreBudget *b, size_t work) {
    b->left = work; b->seconds = 2; clock_gettime(CLOCK_MONOTONIC, &b->start);
}
/* Inventory-wide checks scale with admitted count, not kernel complexity.
 * Small/dense inputs retain the old cap; per-query work remains 65536. */
static size_t admission_work(size_t count) {
    size_t banks = (count + 255u) / 256u;
    if (!banks) banks = 1;
    if (banks > CORE_CAPSULES / 256u) banks = CORE_CAPSULES / 256u;
    return banks * 2000000u;
}
static int charge(CoreBudget *b) {
    if (!b->left) return -1;
    if ((--b->left & 255u) == 0) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - b->start.tv_sec) * 1e9 + now.tv_nsec - b->start.tv_nsec > b->seconds * 1e9) return -1;
    }
    return 0;
}
typedef struct {
    Port port;
    double value[64];
    size_t depth, parent, edge;
} CoreState;
typedef struct { const HybridCoverage *record; HybridAi *bank; } CoreCoverageRef;

struct CnetCapsuleCore {
    CnetBase base;
    /* Private banks preserve the public HybridAi ABI and legacy trace bound.
     * A capsule imports one unit and at most one sampled coverage record. */
    HybridAi *coverage[CORE_COVERAGE_BANKS];
    PrimitiveRegistry registry;
    size_t edge_order[CORE_CAPSULES], indexed_count;
    Port edge_keys[CORE_CAPSULES];
    int index_ready;
    CoreCoverageRef guards[CORE_CAPSULES];
    size_t guard_count;
    int guards_ready;
};

static int guard_order(const void *aa, const void *bb) {
    const CoreCoverageRef *a = aa, *b = bb;
    return strcmp(a->record->unit, b->record->unit);
}
static int build_guard_index(CnetCapsuleCore *c) {
    c->guards_ready = 0; c->guard_count = 0;
    for (size_t bank=0; bank<CORE_COVERAGE_BANKS; bank++) {
        HybridAi *h = c->coverage[bank];
        if (!h) continue;
        for (size_t j=0; j<h->coverage_count; j++) if (h->coverage[j].active) {
            if (c->guard_count == CORE_CAPSULES) return -1;
            c->guards[c->guard_count++] = (CoreCoverageRef){&h->coverage[j], h};
        }
    }
    qsort(c->guards, c->guard_count, sizeof c->guards[0], guard_order);
    for (size_t i=1; i<c->guard_count; i++)
        if (!strcmp(c->guards[i-1].record->unit, c->guards[i].record->unit)) return -1;
    c->guards_ready = 1; return 0;
}
static const CoreCoverageRef *find_guard(const CnetCapsuleCore *c, const char *unit) {
    size_t lo=0, hi=c->guard_count;
    while (lo<hi) {
        size_t mid=lo+(hi-lo)/2;
        int cmp=strcmp(c->guards[mid].record->unit, unit);
        if (cmp<0) lo=mid+1; else hi=mid;
    }
    return lo<c->guard_count && !strcmp(c->guards[lo].record->unit,unit) ? &c->guards[lo] : NULL;
}

static int port_order(Port a, Port b) {
    int tag = strcmp(a.tag, b.tag);
    if (tag) return tag;
    if (a.family != b.family) return (a.family > b.family) - (a.family < b.family);
    if (a.field_width != b.field_width) return (a.field_width > b.field_width) - (a.field_width < b.field_width);
    return (a.field_count > b.field_count) - (a.field_count < b.field_count);
}
static int build_edge_index(CnetCapsuleCore *c) {
    c->index_ready = 0;
    if (c->registry.count > CORE_CAPSULES) return -1;
    for (size_t i = 0; i < c->registry.count; i++) {
        BinaryTransformNetwork *btn = c->registry.entries[i].btn;
        if (!btn || btn->input_port_count != 1) return -1;
        c->edge_keys[i] = btn->input_ports[0];
        size_t j = i;
        /* Stable within a signature: preserve original route tie order. */
        while (j && port_order(c->edge_keys[c->edge_order[j-1]], c->edge_keys[i]) > 0) {
            c->edge_order[j] = c->edge_order[j-1]; j--;
        }
        c->edge_order[j] = i;
    }
    c->indexed_count = c->registry.count; c->index_ready = 1;
    return 0;
}
static int edge_range(CnetCapsuleCore *c, Port port, size_t *begin, size_t *end,
                      CoreBudget *budget, size_t *local_work) {
    if (!c->index_ready || c->indexed_count != c->registry.count) return -1;
    size_t low = 0, high = c->indexed_count;
    while (low < high) {
        if ((local_work && !(*local_work)--) || charge(budget)) return -1;
        size_t mid = low + (high-low)/2;
        int order = port_order(c->edge_keys[c->edge_order[mid]], port);
        if (order < 0) low = mid+1; else high = mid;
    }
    *begin = low;
    /* Walk only this signature's run, charging every comparison. Sparse
     * inventories avoid a second full binary lookup; dense runs remain bounded. */
    while (low < c->indexed_count) {
        if ((local_work && !(*local_work)--) || charge(budget)) return -1;
        if (port_order(c->edge_keys[c->edge_order[low]], port)) break;
        low++;
    }
    *end = low;
    return 0;
}

static int import_capsule(CnetCapsuleCore *c, const char *path, char *error, size_t cap) {
    if (c->base.unit_count >= CORE_CAPSULES) {
        if (error && cap) snprintf(error, cap, "capsule_count_limit");
        return -1;
    }
    size_t bank = c->base.unit_count / HYBRID_COVERAGE_MAX;
    if (!c->coverage[bank]) {
        c->coverage[bank] = calloc(1, sizeof *c->coverage[bank]);
        if (!c->coverage[bank]) {
            if (error && cap) snprintf(error, cap, "coverage_allocation_failed");
            return -1;
        }
        hybrid_ai_init(c->coverage[bank]);
    }
    CnetCapsuleReport report;
    if (cnet_capsule_import(&c->base, c->coverage[bank], path, &report)) {
        if (error && cap) snprintf(error, cap, "import_refused:%.120s", report.reject_reason);
        return -1;
    }
    return 0;
}

static int same_port(Port a, Port b);
static int supported(Port p);
static int coverage_labelled(const CnetCapsuleCore *c) {
    CoreBudget budget; budget_init(&budget, admission_work(c->registry.count));
    for (size_t i = 0; i < c->registry.count; i++) {
        const RegistryEntry *e = &c->registry.entries[i];
        const BinaryTransformNetwork *b = e->btn;
        const RegistryCertCoverage *labels = e->cert_cov;
        if (b->input_port_count != 1 || b->output_port_count != 1 ||
            !supported(b->input_ports[0]) || !supported(b->output_ports[0]) ||
            b->hidden_count > 256 || !labels || !labels->inputs || !labels->targets ||
            !labels->n_rows || labels->n_rows > 4096 || labels->in_dim != b->input_count ||
            labels->out_dim != b->output_count) return 0;
        if (!c->guards_ready) return 0;
        const CoreCoverageRef *ref = find_guard(c, e->name);
        if (ref) {
            const HybridCoverage *h = ref->record;
            if (h->in_dim != labels->in_dim || !same_port(h->input_port, b->input_ports[0]) ||
                !same_port(h->goal_port, b->output_ports[0])) return 0;
            for (size_t r = 0; r < h->n_rows; r++) {
                int found = 0;
                for (size_t k = 0; k < labels->n_rows; k++) {
                    if (charge(&budget)) return 0;
                    if (!memcmp(h->rows + r*h->in_dim, labels->inputs + k*labels->in_dim, h->in_dim*sizeof(double))) {
                        found = 1; break;
                    }
                }
                if (!found) return 0;
            }
        }
    }
    return 1;
}
static int entry_input_order(const void *aa, const void *bb) {
    const RegistryEntry *a = *(const RegistryEntry * const *)aa;
    const RegistryEntry *b = *(const RegistryEntry * const *)bb;
    int order = port_order(a->btn->input_ports[0], b->btn->input_ports[0]);
    return order ? order : port_order(a->btn->output_ports[0], b->btn->output_ports[0]);
}
static int entry_output_order(const void *aa, const void *bb) {
    const RegistryEntry *a = *(const RegistryEntry * const *)aa;
    const RegistryEntry *b = *(const RegistryEntry * const *)bb;
    return port_order(a->btn->output_ports[0], b->btn->output_ports[0]);
}
static int contracts_consistent(const PrimitiveRegistry *reg) {
    CoreBudget budget; budget_init(&budget, 2000000);
    if (reg->count > CORE_CAPSULES) return 0;
    if (!reg->count) return 1;
    const RegistryEntry **sorted = calloc(reg->count, sizeof *sorted);
    if (!sorted) return 0;
    int ok = 0;
    for (size_t i = 0; i < reg->count; i++) {
        sorted[i] = &reg->entries[i];
        if (!sorted[i]->btn || sorted[i]->btn->input_port_count != 1 ||
            sorted[i]->btn->output_port_count != 1) goto done;
    }
    /* Validate each textual direction independently. Do not reorder registry
     * entries: their order is the stable route tie-breaker. */
    qsort(sorted, reg->count, sizeof *sorted, entry_output_order);
    for (size_t i = 1; i < reg->count; i++) {
        Port a = sorted[i-1]->btn->output_ports[0], b = sorted[i]->btn->output_ports[0];
        if (charge(&budget) || (!strcmp(a.tag, b.tag) && !same_port(a,b))) goto done;
    }
    qsort(sorted, reg->count, sizeof *sorted, entry_input_order);
    for (size_t i = 1; i < reg->count; i++) {
        Port a = sorted[i-1]->btn->input_ports[0], b = sorted[i]->btn->input_ports[0];
        if (charge(&budget) || (!strcmp(a.tag, b.tag) && !same_port(a,b))) goto done;
    }
    for (size_t i = 0; i < reg->count; i++) for (size_t j = i; j > 0;) {
        j--;
        if (charge(&budget)) goto done;
        const RegistryEntry *a = sorted[i], *b = sorted[j];
        if (!same_port(a->btn->input_ports[0], b->btn->input_ports[0]) ||
            !same_port(a->btn->output_ports[0], b->btn->output_ports[0])) break;
        const RegistryCertCoverage *x = a->cert_cov, *y = b->cert_cov;
        if (!x || !y || x->in_dim != y->in_dim || x->out_dim != y->out_dim) goto done;
        for (size_t p = 0; p < x->n_rows; p++) for (size_t q = 0; q < y->n_rows; q++) {
            if (charge(&budget)) goto done;
            if (!memcmp(x->inputs + p*x->in_dim, y->inputs + q*y->in_dim, x->in_dim*sizeof(double)) &&
                 memcmp(x->targets + p*x->out_dim, y->targets + q*y->out_dim, x->out_dim*sizeof(double))) goto done;
        }
    }
    ok = 1;
done:
    free(sorted); return ok;
}

void cnet_capsule_core_close(CnetCapsuleCore *c) {
    if (!c) return;
    registry_free(&c->registry);
    for (size_t bank = 0; bank < CORE_COVERAGE_BANKS; bank++) if (c->coverage[bank]) {
        hybrid_ai_free(c->coverage[bank]);
        free(c->coverage[bank]);
    }
    cnb_free(&c->base);
    free(c);
}

CnetCapsuleCore *cnet_capsule_core_open(const char *root, char *error, size_t cap) {
    struct dirent **names = NULL;
    int count = -1, loaded = 0;
    const char *why = "capsule_directory_unavailable";
    CnetCapsuleCore *c = calloc(1, sizeof *c);
    if (error && cap) error[0] = 0;
    if (!c) return NULL;
    cnb_init(&c->base);
    registry_init_production(&c->registry);
    if (!root || !root[0] || (count = scandir(root, &names, NULL, alphasort)) < 0) goto fail;
    for (int i = 0; i < count; i++) {
        char path[1200]; struct stat st;
        if (names[i]->d_name[0] == '.') continue;
        int n = snprintf(path, sizeof path, "%s/%s", root, names[i]->d_name);
        if (n < 0 || (size_t)n >= sizeof path || lstat(path, &st)) goto fail;
        if (!S_ISDIR(st.st_mode)) { why = "unexpected_capsule_entry"; goto fail; }
        if (++loaded > (int)CORE_CAPSULES) { why = "capsule_count_limit"; goto fail; }
        if (import_capsule(c, path, error, cap)) goto fail;
    }
    size_t skipped = 0;
    if (cnb_load_registry(&c->base, &c->registry, &skipped) < 0 || skipped ||
        c->registry.count != c->base.unit_count) {
        why = "certification_replay_failed"; goto fail;
    }
    if (build_guard_index(c) || !coverage_labelled(c)) { why = "unlabelled_coverage_or_unsupported_kernel"; goto fail; }
    if (!contracts_consistent(&c->registry)) { why = "conflicting_contracts_or_comparison_budget"; goto fail; }
    if (build_edge_index(c)) { why = "edge_index_refused"; goto fail; }
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
    return c;
fail:
    if (error && cap && !error[0]) snprintf(error, cap, "%s", why);
    for (int i = 0; i < count; i++) free(names[i]);
    free(names); cnet_capsule_core_close(c); return NULL;
}

static int same_port(Port a, Port b) {
    return a.family == b.family && a.field_width == b.field_width &&
           a.field_count == b.field_count && !strcmp(a.tag, b.tag);
}

CnetCapsuleCore *cnet_capsule_core_open_candidate(const char *root,
        const char *candidate, char *error, size_t cap) {
    CnetCapsuleCore *c = cnet_capsule_core_open(root, error, cap);
    if (!c) return NULL;
    if (import_capsule(c, candidate, error, cap)) {
        cnet_capsule_core_close(c); return NULL;
    }
    registry_free(&c->registry); registry_init_production(&c->registry);
    size_t skipped = 0;
    if (cnb_load_registry(&c->base, &c->registry, &skipped) < 0 || skipped ||
        c->registry.count != c->base.unit_count || c->registry.count > CORE_CAPSULES ||
        build_guard_index(c) || !coverage_labelled(c) || !contracts_consistent(&c->registry) || build_edge_index(c)) {
        if (error && cap) snprintf(error, cap, "candidate_contract_conflict");
        cnet_capsule_core_close(c); return NULL;
    }
    return c;
}

static int resolve_port(CnetCapsuleCore *c, const char *tag, int output, Port *p) {
    int found = 0;
    for (size_t i = 0; i < c->registry.count; i++) {
        BinaryTransformNetwork *b = c->registry.entries[i].btn;
        size_t n = output ? b->output_port_count : b->input_port_count;
        if (n != 1) continue;
        Port candidate = output ? b->output_ports[0] : b->input_ports[0];
        if (strcmp(candidate.tag, tag)) continue;
        if (found && !same_port(*p, candidate)) return -1;
        *p = candidate; found = 1;
    }
    if (!found || p->field_count != 1 || !p->field_width) return -1;
    if (p->family == PORT_ONEHOT) return p->field_width <= 64 ? 0 : -1;
    if (p->family == PORT_BINARY_MSB || p->family == PORT_BINARY_LSB)
        return p->field_width <= 16 ? 0 : -1;
    return -1;
}

typedef struct { CnetCapsuleCore *core; const char *blocked; } CoreGuard;
static int covered(const char *unit, const BinaryTransformNetwork *btn,
                   const double *in, size_t len, void *ctx) {
    CoreGuard *g = ctx;
    CnetCapsuleCore *c = g->core;
    if (btn->input_port_count != 1 || btn->output_port_count != 1) { g->blocked = unit; return -1; }
    if (!c->guards_ready) { g->blocked = unit; return -1; }
    const CoreCoverageRef *ref = find_guard(c, unit);
    /* Canonical import proves exhaustive scope when there is no record. */
    if (!ref || hybrid_coverage_admits_exact(ref->bank, unit, btn->input_ports[0],
                btn->output_ports[0], in, len)) return 0;
    g->blocked = unit; return -1;
}

static int supported(Port p) {
    return p.field_count == 1 && p.field_width &&
        ((p.family == PORT_ONEHOT && p.field_width <= 64) ||
         ((p.family == PORT_BINARY_MSB || p.family == PORT_BINARY_LSB) && p.field_width <= 16));
}
static unsigned decode(Port p, const double *v) {
    unsigned value = 0;
    for (size_t i = 0; i < p.field_width; i++) if (v[i] == 1.0) {
        if (p.family == PORT_ONEHOT) value = (unsigned)i;
        else value |= 1u << (p.family == PORT_BINARY_MSB ? p.field_width - 1 - i : i);
    }
    return value;
}
/* Values, not types alone, determine coverage. Execute each expansion through
 * the canonical strict executor; no guessed/model-free serving fast path. */
static int search(CnetCapsuleCore *c, Port pin, Port goal, const double *input,
                  CnetCapsuleCoreReply *r, CoreBudget *budget) {
    CoreState *states = calloc(CORE_STATES, sizeof *states);
    if (!states) return -1;
    states[0].port = pin; memcpy(states[0].value, input, pin.field_width * sizeof(double));
    size_t count = 1, work = 65536; int rc = -1;
    CoreGuard cg = {c, NULL}; DagNodeGuard guard = {covered, &cg};
    snprintf(r->reason, sizeof r->reason, "no_covered_certified_plan");
    for (size_t head = 0; head < count; head++) {
        CoreState *s = &states[head];
        if (s->depth == ROUTE_MAX_STEPS) continue;
        size_t begin, end;
        if (edge_range(c, s->port, &begin, &end, budget, &work)) goto exhausted;
        for (size_t edge = begin; edge < end; edge++) {
            size_t i = c->edge_order[edge];
            if (!work-- || charge(budget)) goto exhausted;
            RegistryEntry *e = &c->registry.entries[i]; BinaryTransformNetwork *b = e->btn;
            /* The snapshot is private. Audit only the selected entry, through
             * the canonical auditor, before every execution (also in replay).
             * Fail immediately on demotion: audit frees that entry's labels. */
            PrimitiveRegistry selected = {0}; selected.entries = e; selected.count = 1;
            if (registry_audit_certified(&selected)) {
                snprintf(r->reason, sizeof r->reason, "certification_audit_refused"); goto done;
            }
            if (!e->certified || e->state == PRIM_RESET || b->input_port_count != 1 ||
                b->output_port_count != 1 || !same_port(s->port, b->input_ports[0])) continue;
            Port next = b->output_ports[0];
            if (!supported(next) || b->hidden_count > 256) { snprintf(r->reason, sizeof r->reason, "unsupported_kernel_shape"); goto done; }
            RoutePlan step = {0}; step.length = 1; step.strict = 1; step.goal = next;
            step.steps[0] = b; step.names[0] = e->name;
            double out[64];
            int exec = route_execute_guarded(&step, s->value, s->port.field_width, out, next.field_width, NULL, &guard);
            if (exec == ROUTE_EXEC_REFUSED_GUARD) continue;
            if (exec) { snprintf(r->reason, sizeof r->reason, "execution_refused"); goto done; }
            /* A positive executed cycle may reach the initial state as goal. */
            if (same_port(next, goal)) {
                size_t chain[ROUTE_MAX_STEPS], depth = s->depth + 1, at = head;
                chain[depth - 1] = i;
                for (size_t n = depth - 1; n; n--) { chain[n - 1] = states[at].edge; at = states[at].parent; }
                size_t used = 0;
                for (size_t n = 0; n < depth; n++) {
                    int k = snprintf(r->units + used, sizeof r->units - used, "%s%s", n ? "," : "", c->registry.entries[chain[n]].name);
                    if (k < 0 || (size_t)k >= sizeof r->units - used) { snprintf(r->reason, sizeof r->reason, "receipt_overflow"); goto done; }
                    used += (size_t)k;
                }
                r->value = decode(goal, out); r->hops = depth; r->verified = 1; r->reason[0] = 0;
                rc = 0; goto done;
            }
            int seen = 0;
            for (size_t n = 0; n < count; n++) {
                if (!work-- || charge(budget)) goto exhausted;
                if (same_port(states[n].port, next) && !memcmp(states[n].value, out, next.field_width * sizeof(double))) { seen = 1; break; }
            }
            if (seen) continue;
            if (count == CORE_STATES) goto exhausted;
            CoreState *added = &states[count++]; added->port = next; added->depth = s->depth + 1;
            added->parent = head; added->edge = i;
            memcpy(added->value, out, next.field_width * sizeof(double));
        }
    }
    goto done;
exhausted:
    snprintf(r->reason, sizeof r->reason, "search_budget_exhausted");
done:
    if (rc) { r->verified = 0; r->value = 0; r->hops = 0; r->units[0] = 0; }
    free(states); return rc;
}

/* Compose SEALED LABELS through exact guarded joins. These expected outputs
 * never come from executing CNET, and are used only to check admission. */
static int replay_labels(CnetCapsuleCore *labels, CnetCapsuleCore *candidate,
                         CoreBudget *budget, size_t *checked) {
    CoreState *states = calloc(CORE_STATES, sizeof *states);
    if (!states) return -1;
    int rc = -1;
    CoreGuard cg = {labels, NULL};
    for (size_t source = 0; source < labels->registry.count; source++) {
        const RegistryEntry *origin = &labels->registry.entries[source];
        if (!origin->btn || !origin->certified || !origin->cert_cov) goto done;
        Port input_port = origin->btn->input_ports[0];
        for (size_t row = 0; row < origin->cert_cov->n_rows; row++) {
            const double *input = origin->cert_cov->inputs + row*origin->cert_cov->in_dim;
            if (covered(origin->name, origin->btn, input, input_port.field_width, &cg)) continue;
            size_t count = 1; states[0].port = input_port; states[0].depth = 0;
            memcpy(states[0].value, input, input_port.field_width*sizeof(double));
            for (size_t head = 0; head < count; head++) {
                CoreState *s = &states[head];
                if (s->depth == ROUTE_MAX_STEPS) continue;
                size_t begin, end;
                if (edge_range(labels, s->port, &begin, &end, budget, NULL)) goto done;
                for (size_t edge = begin; edge < end; edge++) {
                    size_t i = labels->edge_order[edge];
                    if (charge(budget)) goto done;
                    const RegistryEntry *e = &labels->registry.entries[i];
                    if (!e->btn || !e->certified || !e->cert_cov) goto done;
                    if (!same_port(s->port, e->btn->input_ports[0]) ||
                        covered(e->name, e->btn, s->value, s->port.field_width, &cg)) continue;
                    const RegistryCertCoverage *table = e->cert_cov;
                    for (size_t r = 0; r < table->n_rows; r++) {
                        if (charge(budget)) goto done;
                        if (memcmp(s->value, table->inputs + r*table->in_dim, table->in_dim*sizeof(double))) continue;
                        const double *expected = table->targets + r*table->out_dim;
                        Port next = e->btn->output_ports[0]; CnetCapsuleCoreReply result = {0};
                        if (search(candidate, input_port, next, input, &result, budget) ||
                            !result.verified || result.value != decode(next, expected)) goto done;
                        (*checked)++;
                        int seen = 0;
                        for (size_t n = 0; n < count; n++) {
                            if (charge(budget)) goto done;
                            if (same_port(states[n].port, next) && !memcmp(states[n].value, expected, table->out_dim*sizeof(double))) { seen = 1; break; }
                        }
                        if (seen) continue;
                        if (count == CORE_STATES) goto done;
                        CoreState *added = &states[count++]; added->port = next; added->depth = s->depth + 1;
                        memcpy(added->value, expected, table->out_dim*sizeof(double));
                    }
                }
            }
        }
    }
    rc = 0;
done:
    free(states); return rc;
}

int cnet_capsule_core_validate_growth(CnetCapsuleCore *before, CnetCapsuleCore *candidate,
                                    size_t *label_obligations) {
    if (!before || !candidate || !label_obligations) return -1;
    *label_obligations = 0;
    size_t count = before->registry.count > candidate->registry.count ? before->registry.count : candidate->registry.count;
    CoreBudget budget; budget_init(&budget, admission_work(count));
    /* Full-inventory replay now audits every selected kernel as well. Its
     * cooperative deadline scales with inventory work (2..32 seconds), while
     * ordinary queries and the other admission phases retain two seconds. */
    budget.seconds = (double)admission_work(count) / 1000000.0;
    /* Checking the candidate's closure also rejects NEW contradictory paths,
     * even when the old shortest answer would otherwise remain unchanged. */
    if (replay_labels(before, candidate, &budget, label_obligations) ||
        replay_labels(candidate, candidate, &budget, label_obligations)) return -1;
    return 0;
}

int cnet_capsule_core_ask(CnetCapsuleCore *c, const char *request,
                         CnetCapsuleCoreReply *r) {
    char verb[16], in_tag[32], out_tag[32], value[32], extra;
    Port pin = {0}, pout = {0};
    double in[64] = {0};
    if (!r) return -1;
    memset(r, 0, sizeof *r);
    snprintf(r->reason, sizeof r->reason, "invalid_typed_request");
    if (!c || !request || sscanf(request, "%15s %31s %31s %31s %c", verb,
        in_tag, out_tag, value, &extra) != 4 || strcmp(verb, "capsule")) return -1;
    for (size_t i = 0; value[i]; i++) if (!isdigit((unsigned char)value[i])) return -1;
    errno = 0; char *end;
    unsigned long x = strtoul(value, &end, 10);
    if (errno || *end || x > 65535) return -1;
    if (resolve_port(c, in_tag, 0, &pin) || resolve_port(c, out_tag, 1, &pout)) {
        snprintf(r->reason, sizeof r->reason, "unknown_or_ambiguous_interface"); return -1;
    }
    unsigned long domain = pin.family == PORT_ONEHOT ? pin.field_width : (1UL << pin.field_width);
    if (x >= domain) return -1;
    for (size_t i = 0; i < pin.field_width; i++) {
        size_t bit = pin.family == PORT_BINARY_MSB ? pin.field_width - 1 - i : i;
        in[i] = pin.family == PORT_ONEHOT ? (x == i) : (double)((x >> bit) & 1UL);
    }
    CoreBudget budget; budget_init(&budget, 65536);
    return search(c, pin, pout, in, r, &budget);
}
