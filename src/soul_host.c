/* soul_host.c - thin, P/Invoke-friendly C ABI over the CNET certified engine.
 * Loads a .cnb base into a registry; runs named units safely; routes to a
 * unit by its REAL typed ports; reports real reliability. Heavy lifting is
 * the public engine (cnb_*, registry_*, route_*, btn_*), exported from the
 * shared lib (make cnet_dll).
 */

#include "../include/soul_host.h"
#include "../include/base.h"
#include "../include/router.h"
#include "../include/nn.h"
#include "../include/contract/contract.h"
#include "../include/specialist.h"
#include "../include/specialist_health.h"
#include "../include/gap_lane.h"
#include "../include/cce/cce_router.h"
#include "../include/hybrid_ai.h"
#include "../include/residual_gguf.h"
#include "../include/residual_http.h"
#include "../include/cnet_route_log.h"
#include "../include/cnet_evidence_bundle.h"
#include "../include/cnet_health_layers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <time.h>

struct SoulHost {
    CnetBase base;
    PrimitiveRegistry reg;
    int loaded;
    /* Contract cache for the health tick: rematerialized on demand from the
       sealed unit blobs; one heap Contract per name so pointers stay stable
       across cache growth. Owned by the host, freed in soul_close. */
    Contract **contracts;
    char (*contract_names)[CNB_NAME_MAX];
    size_t contract_count, contract_cap;
    /* Gap inbox (CNET_GAP_INBOX env at soul_open; "" = disabled): serving
       no-plans are appended here for the gap lane to ingest — the serving
       process detects, the lane learns. */
    char gap_inbox[512];
    /* Counterfactual route evidence (CNET_COUNTERFACTUAL env at soul_open;
       default OFF): a REPORT-ONLY shadow channel on soul_route. Certification
       outranks evidence (docs/dispatch.md) — the channel may RANK or REPORT
       but never changes a served answer or a refusal. counterfactual_last
       holds the report attached to the last SERVED route ("" = none). */
    int counterfactual_enabled;
    char counterfactual_last[512];
    /* Remounted oracle descriptors (soul_mount_oracles). One slot per base
       oracle descriptor, sized ONCE to base.oracle_count (fixed after load) so
       a slot's &bound_entries[i] never moves — the adapter BTN and the live
       registry entry both borrow it. Host-owned; freed exactly once in
       soul_close AFTER registry_free drops the borrows. */
    OracleEntry *bound_entries;              /* oracle_count; borrowed by adapters */
    BinaryTransformNetwork *bound_adapters;  /* oracle_count adapter BTNs (btn_free frees ctx) */
    Contract **bound_contracts;              /* oracle_count recovered sealed contracts (owned) */
    unsigned char *bound_mounted;            /* oracle_count: 1 = live in the registry */
    size_t bound_cap;                        /* == oracle_count once allocated, else 0 */
    size_t mounted_count;
    /* Live residual (Tier C) — lazy-bound on first miss when env set. */
    char base_path[512];
    HybridAi hybrid;
    ResidualGguf *owned_residual;
    ResidualHttp *owned_residual_http;
    int residual_tried;       /* lazy init attempted */
    int hermetic_residual;    /* rot1 stand-in (tests) */
    int residual_window;
    int last_source;
    uint64_t certified_serves;
    uint64_t residual_serves;
    uint64_t gap_notes;
    uint64_t structure_mines;
    uint64_t structure_seals;
    /* Optional route-decision JSONL (CNET_ROUTE_LOG at soul_open). */
    char *route_log_path;
    /* Evidence-bundle sidecar path (CNET_EVIDENCE_STORE or <base>.evidence.jsonl). */
    char evidence_store[576];
};

/* Health-tick contract source: the base itself. A miss materializes the
   sealed unit (per-blob CNU1 seal verified by cnb_get_unit), keeps the
   Contract, and discards the temporary BTN — the registry instance stays
   the single execution truth. */
static const Contract *soul_contract_lookup(const char *name, void *ctx) {
    SoulHost *h = (SoulHost *)ctx;
    size_t i;
    BinaryTransformNetwork tmp;
    Contract *c;
    if (!h || !name) return NULL;
    for (i = 0; i < h->contract_count; ++i)
        if (strcmp(h->contract_names[i], name) == 0) return h->contracts[i];
    if (strlen(name) + 1 > sizeof h->contract_names[0]) return NULL;
    c = (Contract *)calloc(1, sizeof *c);
    if (!c) return NULL;
    memset(&tmp, 0, sizeof tmp);
    if (cnb_get_unit(&h->base, name, &tmp, c) != 0) {
        free(c);
        return NULL;
    }
    btn_free(&tmp);
    if (h->contract_count == h->contract_cap) {
        size_t cap = h->contract_cap ? h->contract_cap * 2 : 8;
        Contract **nc = (Contract **)realloc(h->contracts, cap * sizeof *nc);
        char (*nn)[CNB_NAME_MAX] = (char (*)[CNB_NAME_MAX])
            realloc(h->contract_names, cap * sizeof *nn);
        if (nc) h->contracts = nc;
        if (nn) h->contract_names = nn;
        if (!nc || !nn) { contract_free(c); free(c); return NULL; }
        h->contract_cap = cap;
    }
    h->contracts[h->contract_count] = c;
    snprintf(h->contract_names[h->contract_count],
             sizeof h->contract_names[0], "%s", name);
    h->contract_count++;
    return c;
}

/* Sum of a contract's port totals (field_width * field_count), in doubles. */
static size_t ports_total(const Port *ports, size_t n) {
    size_t t = 0, i;
    for (i = 0; i < n; ++i) t += ports[i].field_width * ports[i].field_count;
    return t;
}

static double soul_mono_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static const char *soul_route_outcome(int code, int last_source) {
    if (code >= 0) {
        if (last_source == SOUL_SOURCE_RESIDUAL) return "ok_residual";
        if (last_source == SOUL_SOURCE_PROBE) return "ok_probe";
        if (last_source == SOUL_SOURCE_CERTIFIED) return "ok";
        return "ok";
    }
    switch (code) {
        case -1: return "err_invalid";
        case -2: return "err_not_found";
        case -3: return "no_plan";
        case -4: return "err_buffer";
        case -5: return "err_execute";
        default: return "err_unknown";
    }
}

static const char *soul_source_profile(int last_source) {
    switch (last_source) {
        case SOUL_SOURCE_CERTIFIED: return "certified";
        case SOUL_SOURCE_RESIDUAL:  return "residual";
        case SOUL_SOURCE_PROBE:     return "probe";
        default: return "none";
    }
}

/* Plan cost: 1 (decision) + sum of adapter_cost (or 1 per step if unset). */
static double soul_plan_cost(const RoutePlan *plan) {
    double c = 1.0;
    size_t i;
    if (!plan || plan->length == 0) return c;
    for (i = 0; i < plan->length; ++i) {
        size_t hint = plan->steps[i] ? plan->steps[i]->adapter_cost : 0;
        c += hint > 0 ? (double)hint : 1.0;
    }
    return c;
}

/* Entropy proxy: 1 - mean Laplace reliability of plan steps (or 1 if empty). */
static float soul_plan_entropy(const RoutePlan *plan) {
    double sum = 0.0;
    size_t i, n;
    if (!plan || plan->length == 0) return 1.0f;
    n = plan->length;
    for (i = 0; i < n; ++i) {
        double rel = plan->steps[i] ? btn_reliability(plan->steps[i]) : 0.5;
        if (rel < 0.0) rel = 0.0;
        if (rel > 1.0) rel = 1.0;
        sum += rel;
    }
    return (float)(1.0 - sum / (double)n);
}

/* Best-effort; never fails the serve path. */
static void soul_emit_route_log(SoulHost *h,
                                const char *event,
                                const char *mechanism,
                                const char *goal_tag,
                                const RoutePlan *plan,
                                int outcome_code,
                                int last_source,
                                double route_ms,
                                double total_ms) {
    CnetRouteLogEvent ev;
    const char *unit = "";
    uint32_t plen = 0;
    if (!h || !h->route_log_path || !h->route_log_path[0]) return;
    if (plan && plan->length > 0) {
        plen = (uint32_t)plan->length;
        if (plan->names[plan->length - 1])
            unit = plan->names[plan->length - 1];
    }
    memset(&ev, 0, sizeof ev);
    ev.event = event ? event : "soul_route";
    ev.mechanism = mechanism ? mechanism : CNET_ROUTE_MECH_PLANNER;
    ev.role = goal_tag ? goal_tag : "";
    ev.role_canonical = ev.role;
    ev.selected_expert = plen;
    ev.selected_unit = unit;
    ev.plan_length = plen;
    ev.expert_profile = soul_source_profile(last_source);
    ev.entropy = plan ? soul_plan_entropy(plan) : 1.0f;
    ev.outcome = soul_route_outcome(outcome_code, last_source);
    ev.outcome_code = outcome_code;
    ev.route_latency_ms = route_ms;
    ev.total_latency_ms = total_ms;
    ev.cost = plan ? soul_plan_cost(plan)
                   : (outcome_code >= 0 ? 1.0 : 1.0);
    if (last_source == SOUL_SOURCE_RESIDUAL && (!plan || plan->length == 0))
        ev.cost = 1.0; /* residual decision cost only (no certified steps) */
    ev.model_id = h->base_path[0] ? h->base_path : NULL;
    (void)cnet_route_log_append(h->route_log_path, &ev);
}

static int soul_state_dir(const char *base_path, char *out, size_t cap) {
    const char *slash;
    const char *backslash;
    size_t len;
    int n;
    if (!base_path || !out || cap == 0) return -1;
    slash = strrchr(base_path, '/');
    backslash = strrchr(base_path, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
    if (!slash) {
        /* Relative bare filename: use "<base>.state" so cwd is never a
           shared dump for every base (prevents cross-test/host contamination). */
        n = snprintf(out, cap, "%s.state", base_path);
        return n >= 0 && (size_t)n < cap ? 0 : -1;
    }
    len = (size_t)(slash - base_path);
    if (len == 0) len = 1;
    /* "<dir>/<file>.state" next to the base */
    {
        size_t flen = strlen(slash + 1);
        if (len + 1 + flen + 6 + 1 > cap) return -1;
        memcpy(out, base_path, len);
        out[len] = '/';
        memcpy(out + len + 1, slash + 1, flen);
        memcpy(out + len + 1 + flen, ".state", 7);
        return 0;
    }
}

/* The registry is the live source of truth after cnb_load_registry has replayed
   certification. Rematerializing a fresh BTN from CNB per call would split
   execution evidence from the planner's instance. */
static RegistryEntry *soul_find_unit(SoulHost *h, const char *name) {
    size_t i;
    if (!h || !h->loaded || !name) return NULL;
    for (i = 0; i < h->reg.count; ++i) {
        RegistryEntry *entry = &h->reg.entries[i];
        if (entry->btn && entry->name && strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

static void soul_record_result(BinaryTransformNetwork *btn, int success) {
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
    if (success) atomic_fetch_add_explicit(&btn->output_successes, 1, memory_order_relaxed);
    else atomic_fetch_add_explicit(&btn->output_failures, 1, memory_order_relaxed);
#else
    if (success) btn->output_successes++;
    else btn->output_failures++;
#endif
}

CNET_API int soul_open(const char *base_path, const char *model_path,
                       SoulHost **out) {
    char state_dir[1024];
    (void)model_path;                  /* hybrid model loading is host-side */
    if (!base_path || !out) return -1;

    SoulHost *h = (SoulHost *)calloc(1, sizeof *h);
    if (!h) return -2;

    cnb_init(&h->base);
    if (cnb_load(&h->base, base_path) != 0) {
        cnb_free(&h->base);
        free(h);
        return -3;
    }
    registry_init_production(&h->reg);
    {
        size_t skipped = 0;
        if (cnb_load_registry(&h->base, &h->reg, &skipped) != 0) {
            registry_free(&h->reg);
            cnb_free(&h->base);
            free(h);
            return -4;
        }
    }
    if (soul_state_dir(base_path, state_dir, sizeof state_dir) != 0 ||
        registry_restore_runtime_state(&h->reg, state_dir) != 0) {
        registry_free(&h->reg);
        cnb_free(&h->base);
        free(h);
        return -5;
    }
    /* Restore host serve counters if a prior soul_close wrote them. */
    {
        char serve_path[1100];
        if ((size_t)snprintf(serve_path, sizeof serve_path,
                             "%s/soul_serve.stats", state_dir) <
            sizeof serve_path) {
            FILE *sf = fopen(serve_path, "r");
            if (sf) {
                char magic[32];
                int ver = 0;
                unsigned long long cs = 0, rs = 0, gn = 0, sm = 0, ss = 0;
                if (fscanf(sf, "%31s %d", magic, &ver) == 2 &&
                    strcmp(magic, "CNET_SERVE") == 0 && ver == 1) {
                    char key[64];
                    unsigned long long val;
                    while (fscanf(sf, "%63s %llu", key, &val) == 2) {
                        if (strcmp(key, "certified_serves") == 0) cs = val;
                        else if (strcmp(key, "residual_serves") == 0) rs = val;
                        else if (strcmp(key, "gap_notes") == 0) gn = val;
                        else if (strcmp(key, "structure_mines") == 0) sm = val;
                        else if (strcmp(key, "structure_seals") == 0) ss = val;
                    }
                    h->certified_serves = cs;
                    h->residual_serves = rs;
                    h->gap_notes = gn;
                    h->structure_mines = sm;
                    h->structure_seals = ss;
                }
                fclose(sf);
            }
        }
    }
    h->reg.require_certified = 1;
    {
        const char *inbox = getenv("CNET_GAP_INBOX");
        if (inbox && inbox[0] && strlen(inbox) < sizeof h->gap_inbox)
            memcpy(h->gap_inbox, inbox, strlen(inbox) + 1);
    }
    {
        const char *cf = getenv("CNET_COUNTERFACTUAL");
        h->counterfactual_enabled = cf && cf[0] && strcmp(cf, "0") != 0;
    }
    {
        const char *rl = cnet_route_log_path_from_env();
        if (rl && rl[0]) {
            size_t n = strlen(rl);
            h->route_log_path = (char *)malloc(n + 1);
            if (!h->route_log_path) {
                registry_free(&h->reg);
                cnb_free(&h->base);
                free(h);
                return -2;
            }
            memcpy(h->route_log_path, rl, n + 1);
        }
    }
    {
        size_t n = strlen(base_path);
        if (n >= sizeof h->base_path) n = sizeof h->base_path - 1;
        memcpy(h->base_path, base_path, n);
        h->base_path[n] = '\0';
    }
    {
        const char *es = cnet_evidence_store_path_from_env();
        h->evidence_store[0] = '\0';
        if (es && es[0] && strlen(es) < sizeof h->evidence_store)
            memcpy(h->evidence_store, es, strlen(es) + 1);
        else
            (void)cnet_evidence_store_path_for_base(
                h->base_path, h->evidence_store, sizeof h->evidence_store);
    }
    hybrid_ai_init(&h->hybrid);
    h->owned_residual = NULL;
    h->owned_residual_http = NULL;
    h->residual_tried = 0;
    h->hermetic_residual = 0;
    h->residual_window = 0;
    h->last_source = SOUL_SOURCE_NONE;
    {
        const char *hr = getenv("CNET_SOUL_RESIDUAL_HERMETIC");
        if (hr && hr[0] == '1') h->hermetic_residual = 1;
    }
    /* Eager HTTP residual when configured — health_tick reports residual_bound
     * without waiting for a Tier-C miss (Bonsai path). */
    {
        const char *http = getenv("CNET_RESIDUAL_HTTP");
        const char *prefer = getenv("CNET_SOUL_RESIDUAL_PREFER_HERMETIC");
        if (http && http[0] && !(prefer && prefer[0] == '1' && h->hermetic_residual)) {
            ResidualHttp *rh = NULL;
            const char *win = getenv("CNET_RESIDUAL_WINDOW");
            if (residual_http_open(&rh, http, win, 32) == 0 && rh &&
                hybrid_bind_residual(&h->hybrid, "residual_http",
                                     residual_http_oracle, rh) == 0) {
                h->owned_residual_http = rh;
                h->residual_window = residual_http_window_n(rh);
                h->residual_tried = 1;
            } else if (rh) {
                residual_http_close(rh);
            }
        } else if (h->hermetic_residual && prefer && prefer[0] == '1') {
            /* Eager soft residual when hermetic is preferred */
            size_t d = 256;
            if (hybrid_bind_residual(&h->hybrid, "hermetic_residual",
                                     hybrid_hermetic_residual,
                                     (void *)(uintptr_t)d) == 0) {
                h->residual_window = (int)d;
                h->residual_tried = 1;
            }
        }
    }
    h->loaded = 1;
    *out = h;
    return 0;
}

/* Lazy residual bind: real GGUF if CNET_RESIDUAL_GGUF set, else hermetic rot1.
 * CNET_SOUL_RESIDUAL_PREFER_HERMETIC=1 skips GGUF and binds hermetic first
 * (avoids dual-GPU contention with the personal-AI teacher). */
static int soul_ensure_residual(SoulHost *h, size_t want_dim) {
    const char *path;
    const char *prefer;
    if (!h) return -1;
    if (h->hybrid.residual.bound) {
        if (h->hermetic_residual && want_dim > 0 && !h->owned_residual) {
            hybrid_bind_residual(&h->hybrid, "hermetic_residual",
                                 hybrid_hermetic_residual,
                                 (void *)(uintptr_t)want_dim);
            h->residual_window = (int)want_dim;
        }
        return 0;
    }
    if (h->residual_tried && !h->hermetic_residual) return -1;

    prefer = getenv("CNET_SOUL_RESIDUAL_PREFER_HERMETIC");
    /* Real HTTP residual (Bonsai) outranks soft hermetic when URL is set. */
    {
        const char *http = getenv("CNET_RESIDUAL_HTTP");
        if (http && http[0]) {
            ResidualHttp *rh = NULL;
            const char *win = getenv("CNET_RESIDUAL_WINDOW");
            h->residual_tried = 1;
            if (residual_http_open(&rh, http, win, 32) != 0 || !rh) {
                fprintf(stderr, "soul_host: residual HTTP open failed (%s)%s\n",
                        http,
                        h->hermetic_residual ? " — falling back" : "");
                if (!h->hermetic_residual && !(prefer && prefer[0] == '1'))
                    return -1;
            } else if (hybrid_bind_residual(&h->hybrid, "residual_http",
                                            residual_http_oracle, rh) != 0) {
                residual_http_close(rh);
                fprintf(stderr, "soul_host: residual HTTP bind failed%s\n",
                        h->hermetic_residual ? " — falling back" : "");
                if (!h->hermetic_residual && !(prefer && prefer[0] == '1'))
                    return -1;
            } else {
                h->owned_residual_http = rh;
                h->residual_window = residual_http_window_n(rh);
                fprintf(stderr, "soul_host: residual HTTP bound window=%d url=%s\n",
                        h->residual_window, http);
                return 0;
            }
        }
    }

    if (prefer && prefer[0] == '1' && h->hermetic_residual) {
        size_t d = want_dim > 0 ? want_dim : 256;
        if (hybrid_bind_residual(&h->hybrid, "hermetic_residual",
                                 hybrid_hermetic_residual,
                                 (void *)(uintptr_t)d) != 0)
            return -1;
        h->residual_window = (int)d;
        h->residual_tried = 1;
        return 0;
    }

    path = getenv("CNET_RESIDUAL_GGUF");
    if (path && path[0]) {
        ResidualGguf *r = NULL;
        const char *win = getenv("CNET_RESIDUAL_WINDOW");
        h->residual_tried = 1;
        if (residual_gguf_open(&r, path, win, 32) != 0 || !r) {
            fprintf(stderr, "soul_host: residual GGUF open failed (%s)%s\n", path,
                    h->hermetic_residual ? " — falling back to hermetic" : "");
            if (!h->hermetic_residual) return -1;
        } else if (hybrid_bind_residual(&h->hybrid, "residual_gguf",
                                        residual_gguf_oracle, r) != 0) {
            residual_gguf_close(r);
            fprintf(stderr, "soul_host: residual GGUF bind failed%s\n",
                    h->hermetic_residual ? " — falling back to hermetic" : "");
            if (!h->hermetic_residual) return -1;
        } else {
            h->owned_residual = r;
            h->residual_window = residual_gguf_window_n(r);
            fprintf(stderr, "soul_host: residual GGUF bound window=%d\n",
                    h->residual_window);
            return 0;
        }
    }
    if (h->hermetic_residual) {
        size_t d = want_dim > 0 ? want_dim : 4;
        if (hybrid_bind_residual(&h->hybrid, "hermetic_residual",
                                 hybrid_hermetic_residual,
                                 (void *)(uintptr_t)d) != 0)
            return -1;
        h->residual_window = (int)d;
        h->residual_tried = 1;
        return 0;
    }
    h->residual_tried = 1;
    return -1;
}

static int soul_try_residual_answer(SoulHost *h, Port input, Port goal,
                                    const double *in, size_t in_total,
                                    double *out, size_t out_total) {
    if (!h || !in || !out) return -1;
    if (input.family != PORT_ONEHOT || goal.family != PORT_ONEHOT)
        return -1;
    if (in_total != out_total || in_total == 0) return -1;
    if (soul_ensure_residual(h, in_total) != 0) return -1;
    if (!h->hybrid.residual.bound) return -1;
    /* Real GGUF residual only answers its fixed window size. */
    /* Real GGUF residual only answers its fixed window size.
     * HTTP residual same rule when bound. */
    if (h->owned_residual && (int)in_total != h->residual_window) return -1;
    if (h->owned_residual_http && (int)in_total != h->residual_window)
        return -1;
    if (hybrid_try_residual(&h->hybrid, input, goal, in, in_total, out,
                            out_total) != 0)
        return -1;
    h->residual_serves++;
    h->last_source = SOUL_SOURCE_RESIDUAL;
    return 0;
}

/* Mine residual traces → certified unit → seal into the open CNB. */
CNET_API int soul_structure_mine(SoulHost *h) {
    BinaryTransformNetwork *stu = NULL;
    size_t min_hits = 3;
    const char *mh;
    int mrc;
    size_t in_dim, out_dim, n_rows, r, j;
    double *inputs = NULL, *targets = NULL;
    Contract c;
    int reused = 0;
    char name[64];

    if (!h || !h->loaded) return -1;
    mh = getenv("CNET_PERSONAL_STRUCTURE_MIN_HITS");
    if (mh && mh[0]) {
        long v = atol(mh);
        if (v >= 1) min_hits = (size_t)v;
    }
    mrc = hybrid_structure_mine(&h->hybrid, &h->reg, min_hits, &stu);
    if (mrc != 0) return mrc == 1 ? 1 : mrc;
    if (!stu) return -2;
    h->structure_mines++;

    /* Rebuild labeled table for durable seal (cnb_add_unit needs a contract). */
    if (stu->input_port_count < 1 || stu->output_port_count < 1) return -3;
    in_dim = stu->input_ports[0].field_width * stu->input_ports[0].field_count;
    out_dim = stu->output_ports[0].field_width * stu->output_ports[0].field_count;
    if (in_dim == 0 || out_dim == 0) return -3;
    n_rows = in_dim <= 16 ? in_dim : 16;
    inputs = (double *)calloc(n_rows * in_dim, sizeof(double));
    targets = (double *)calloc(n_rows * out_dim, sizeof(double));
    if (!inputs || !targets) {
        free(inputs);
        free(targets);
        return -4;
    }
    for (r = 0; r < n_rows; r++) {
        for (j = 0; j < in_dim; j++)
            inputs[r * in_dim + j] = (j == r) ? 1.0 : 0.0;
        if (h->hybrid.residual.bound &&
            h->hybrid.residual.fn(inputs + r * in_dim, targets + r * out_dim,
                                  h->hybrid.residual.ctx) != 0) {
            /* fallback: student forward */
            const double *pred = btn_forward(stu, inputs + r * in_dim);
            if (pred)
                memcpy(targets + r * out_dim, pred, out_dim * sizeof(double));
        } else if (!h->hybrid.residual.bound) {
            const double *pred = btn_forward(stu, inputs + r * in_dim);
            if (pred)
                memcpy(targets + r * out_dim, pred, out_dim * sizeof(double));
        }
    }
    /* hybrid_structure_mine already bumped structure_mines; name matches admit. */
    snprintf(name, sizeof name, "hyb_struct_%zu",
             h->hybrid.structure_mines > 0 ? h->hybrid.structure_mines - 1
                                           : 0);
    memset(&c, 0, sizeof c);
    /* Prefer student self-labels for seal: unit already admitted via residual
     * teacher; self-consistency is what cnb_add_unit certification needs. */
    for (r = 0; r < n_rows; r++) {
        const double *pred = btn_forward(stu, inputs + r * in_dim);
        if (pred)
            memcpy(targets + r * out_dim, pred, out_dim * sizeof(double));
    }
    if (contract_init_borrowed(&c, name, stu, inputs, targets, n_rows) != 0) {
        fprintf(stderr, "soul_host: structure seal contract failed name=%s\n",
                name);
        free(inputs);
        free(targets);
        /* Live registry still has the mined unit. */
        return 0;
    }
    if (cnb_add_unit(&h->base, stu, &c, &reused) != 0) {
        fprintf(stderr, "soul_host: structure seal cnb_add_unit failed name=%s\n",
                name);
        contract_free(&c);
        free(inputs);
        free(targets);
        return 0; /* mined live; durable seal optional */
    }
    contract_free(&c);
    free(inputs);
    free(targets);
    if (h->evidence_store[0]) {
        CnetEvidenceOpts eopts;
        memset(&eopts, 0, sizeof eopts);
        eopts.counterfactual_stability = -1.0f;
        (void)cnet_evidence_record(&h->base, name, h->evidence_store, &eopts);
    }
    if (h->base_path[0] && cnb_save(&h->base, h->base_path) != 0) {
        fprintf(stderr, "soul_host: structure seal cnb_save failed path=%s\n",
                h->base_path);
        return 0;
    }
    h->structure_seals++;
    fprintf(stderr, "soul_host: structure-mined + sealed unit '%s' (reused=%d)\n",
            name, reused);
    return 0;
}

CNET_API int soul_serve_stats(SoulHost *h, SoulServeStats *out) {
    if (!h || !h->loaded || !out) return -1;
    memset(out, 0, sizeof *out);
    out->certified_serves = h->certified_serves;
    out->residual_serves = h->residual_serves;
    out->gap_notes = h->gap_notes;
    out->structure_mines = h->structure_mines;
    out->structure_seals = h->structure_seals;
    out->residual_bound = h->hybrid.residual.bound ? 1 : 0;
    out->residual_window = h->residual_window;
    out->last_source = h->last_source;
    out->units = (int)h->reg.count;
    return 0;
}

CNET_API int soul_last_source(SoulHost *h) {
    if (!h || !h->loaded) return -1;
    return h->last_source;
}

CNET_API int soul_unit_count(SoulHost *h) {
    if (!h || !h->loaded || h->reg.count > (size_t)INT_MAX) return -1;
    return (int)h->reg.count;
}

CNET_API int soul_unit_name(SoulHost *h, int index, char *out, int out_cap) {
    const char *name;
    size_t need;
    if (!h || !h->loaded || !out || out_cap <= 0 || index < 0 ||
        (size_t)index >= h->reg.count) return -1;
    name = h->reg.entries[(size_t)index].name;
    if (!name || !h->reg.entries[(size_t)index].btn) return -2;
    need = strlen(name) + 1;
    if (need > (size_t)out_cap) {
        out[0] = '\0';
        return -3;
    }
    memcpy(out, name, need);
    return 0;
}

static int soul_copy_descriptor_atom(const char *value, char *out, int out_cap) {
    size_t need;
    if (!value || !out || out_cap <= 0) return -1;
    need = strlen(value) + 1;
    if (need > (size_t)out_cap) {
        out[0] = '\0';
        return -3;
    }
    memcpy(out, value, need);
    return 0;
}

CNET_API int soul_oracle_count(SoulHost *h) {
    if (!h || !h->loaded || h->base.oracle_count > (size_t)INT_MAX) return -1;
    return (int)h->base.oracle_count;
}

CNET_API int soul_oracle_name(SoulHost *h, int index, char *out, int out_cap) {
    if (!h || !h->loaded || index < 0 ||
        (size_t)index >= h->base.oracle_count) return -1;
    return soul_copy_descriptor_atom(h->base.oracles[(size_t)index].name,
                                     out, out_cap);
}

CNET_API int soul_oracle_kind(SoulHost *h, int index, char *out, int out_cap) {
    if (!h || !h->loaded || index < 0 ||
        (size_t)index >= h->base.oracle_count) return -1;
    return soul_copy_descriptor_atom(h->base.oracles[(size_t)index].kind,
                                     out, out_cap);
}

CNET_API int soul_oracle_identity(
    SoulHost *h, int index,
    uint64_t *behavior_digest,
    uint64_t *artifact_digest,
    uint64_t *contract_digest,
    uint64_t *config_digest,
    uint64_t *retrieval_snapshot_digest,
    uint64_t *toolchain_digest) {
    const CnbOracleDesc *desc;
    if (!h || !h->loaded || index < 0 ||
        (size_t)index >= h->base.oracle_count) return -1;
    desc = &h->base.oracles[(size_t)index];
    if (behavior_digest) *behavior_digest = desc->behavior_digest;
    if (artifact_digest) *artifact_digest = desc->identity.artifact_digest;
    if (contract_digest) *contract_digest = desc->identity.contract_digest;
    if (config_digest) *config_digest = desc->identity.config_digest;
    if (retrieval_snapshot_digest)
        *retrieval_snapshot_digest = desc->identity.retrieval_snapshot_digest;
    if (toolchain_digest) *toolchain_digest = desc->identity.toolchain_digest;
    return 0;
}

CNET_API int soul_oracle_artifact_sha256(SoulHost *h, int index,
                                         unsigned char out32[32]) {
    if (!h || !h->loaded || !out32 || index < 0 ||
        (size_t)index >= h->base.oracle_count) return -1;
    memcpy(out32, h->base.oracles[(size_t)index].identity.artifact_sha256, 32);
    return 0;
}

CNET_API int soul_oracle_runtime_libs_digest(SoulHost *h, int index,
                                             uint64_t *out) {
    if (!h || !h->loaded || !out || index < 0 ||
        (size_t)index >= h->base.oracle_count) return -1;
    *out = h->base.oracles[(size_t)index].identity.runtime_libs_digest;
    return 0;
}

CNET_API int soul_unit_provenance(SoulHost *h, const char *name,
                                  char *out, int out_cap) {
    const char *prov;
    if (!h || !h->loaded || !name || !out || out_cap <= 0) return -1;
    prov = cnb_unit_provenance(&h->base, name);
    if (!prov) return -2;
    return soul_copy_descriptor_atom(prov, out, out_cap);
}

CNET_API int soul_unit_dims(SoulHost *h, const char *name,
                            int *in_total, int *out_total) {
    RegistryEntry *entry = soul_find_unit(h, name);
    size_t in_n, out_n;
    if (!entry) return -2;
    in_n = ports_total(entry->btn->input_ports, entry->btn->input_port_count);
    out_n = ports_total(entry->btn->output_ports, entry->btn->output_port_count);
    if (in_n > (size_t)INT_MAX || out_n > (size_t)INT_MAX) return -3;
    if (in_total)  *in_total  = (int)in_n;
    if (out_total) *out_total = (int)out_n;
    return 0;
}

CNET_API int soul_run(SoulHost *h, const char *name,
                      const double *in, double *out, int out_cap) {
    RegistryEntry *entry = soul_find_unit(h, name);
    BinaryTransformNetwork *btn;
    double *clean_in;
    size_t in_total, out_total, n, off, p;
    const double *result;
    int valid = 1;
    if (!entry || !in || !out || out_cap <= 0) return -1;
    btn = entry->btn;
    in_total = ports_total(btn->input_ports, btn->input_port_count);
    out_total = ports_total(btn->output_ports, btn->output_port_count);
    clean_in = malloc(in_total * sizeof *clean_in);
    if (!clean_in) return -3;
    off = 0;
    for (p = 0; p < btn->input_port_count; ++p) {
        size_t width = btn->input_ports[p].field_width * btn->input_ports[p].field_count;
        if (!port_validate(btn->input_ports[p], in + off) ||
            port_canonicalize(btn->input_ports[p], in + off, clean_in + off) != 0) {
            free(clean_in);
            return -4;
        }
        off += width;
    }
    result = btn_forward(btn, clean_in);
    free(clean_in);
    if (!result) return -3;
    off = 0;
    for (p = 0; p < btn->output_port_count; ++p) {
        size_t width = btn->output_ports[p].field_width * btn->output_ports[p].field_count;
        if (!port_validate(btn->output_ports[p], result + off)) valid = 0;
        off += width;
    }
    soul_record_result(btn, valid);

    n = (out_total < (size_t)out_cap) ? out_total : (size_t)out_cap;  /* never overflow */
    memcpy(out, result, n * sizeof(double));
    return (int)out_total;   /* > out_cap signals truncation to the caller */
}

/* ---- counterfactual route evidence (REPORT-ONLY shadow channel) -----------
   With CNET_COUNTERFACTUAL set at soul_open, a SERVED route additionally
   projects the same-shape certified roster as an ephemeral CCE recall forest
   (branch = live unit, centroid = the unit's first sealed exemplar input,
   goodness = its live Laplace reliability) and asks the layer-1 recall router
   how stable the served unit's goal ownership is against ranked alternatives
   (cce_router_sample_counterfactuals + cce_router_consistency_score).
   Layer-1 recall has NO authority (docs/dispatch.md): this runs strictly
   AFTER route_execute has produced the answer, executes no unit, and only
   writes a report string plus one stderr telemetry line. The served answer
   and every refusal path are byte-identical with the knob on or off
   (gate: make counterfactual_serving).
   Roster slot 0 is RESERVED for the served unit so a saturated roster
   (>= SOUL_CF_MAX_BRANCHES same-shape certified units ahead of it in
   registry order) can never crowd the primary out; the shadow stays silent
   only when the served unit itself is unqualifiable (no recoverable sealed
   exemplar/centroid data). */
#define SOUL_CF_MAX_BRANCHES 32
#define SOUL_CF_CENTROID_DIM 32

/* Grounds one certified live unit as a CCE branch at the given roster slot.
   Returns 1 on success; 0 when the unit cannot be honestly grounded (not
   certified, shape mismatch, or its sealed exemplar contract — which anchors
   the unit's input domain — cannot be recovered). */
static int soul_cf_ground_branch(SoulHost *h, const RegistryEntry *entry,
                                 size_t in_total, int dim,
                                 cce_cascade *cascade, cce_branch *branch) {
    const Contract *c;
    int d;
    if (!entry->btn || !entry->name || !entry->certified || entry->shadow_of)
        return 0;
    if (ports_total(entry->btn->input_ports,
                    entry->btn->input_port_count) != in_total)
        return 0;
    c = soul_contract_lookup(entry->name, h);
    if (!c || c->exemplar_count == 0 || !c->inputs) return 0;
    memset(cascade, 0, sizeof *cascade);
    memset(branch, 0, sizeof *branch);
    cascade->goodness = (float)btn_reliability(entry->btn);
    snprintf(cascade->name, sizeof cascade->name, "%s", entry->name);
    branch->cascade = cascade;
    branch->centroid_dim = dim;
    for (d = 0; d < dim; ++d)
        branch->centroid[d] = (float)c->inputs[d];
    snprintf(branch->name, sizeof branch->name, "%s", entry->name);
    return 1;
}

static void soul_counterfactual_shadow(SoulHost *h, const char *goal_tag,
                                       const RegistryEntry *served,
                                       const double *in, size_t in_total) {
    cce_cascade cascades[SOUL_CF_MAX_BRANCHES];
    cce_branch branches[SOUL_CF_MAX_BRANCHES];
    cce_forest forest;
    cce_router router;
    CounterfactualRoute alts[CCE_ROUTER_MAX_COUNTERFACTUALS];
    CounterfactualRoute primary;
    float input_f[SOUL_CF_CENTROID_DIM];
    float consistency;
    size_t i;
    int dim, k, len, n, primary_idx, count = 0;

    dim = in_total < SOUL_CF_CENTROID_DIM ? (int)in_total
                                          : SOUL_CF_CENTROID_DIM;
    if (dim <= 0) return;
    for (k = 0; k < dim; ++k) input_f[k] = (float)in[k];

    /* the served unit owns slot 0 unconditionally so the roster can never
       saturate before reaching it; if IT cannot be grounded (missing sealed
       exemplar/centroid data) the channel honestly reports nothing */
    if (!soul_cf_ground_branch(h, served, in_total, dim, &cascades[0],
                               &branches[0]))
        return;
    primary_idx = 0;
    n = 1;
    for (i = 0; i < h->reg.count && n < SOUL_CF_MAX_BRANCHES; ++i) {
        const RegistryEntry *entry = &h->reg.entries[i];
        if (entry == served) continue;
        if (soul_cf_ground_branch(h, entry, in_total, dim, &cascades[n],
                                  &branches[n]))
            n++;
    }

    memset(&forest, 0, sizeof forest);
    forest.branches = branches;
    forest.num_branches = n;
    forest.max_branches = n;
    if (cce_router_init(&router, 1.0f, n) != CCE_OK) return;
    if (cce_router_sample_counterfactuals(&router, &forest, input_f, dim,
                                          primary_idx, alts,
                                          CCE_ROUTER_MAX_COUNTERFACTUALS,
                                          &count) != CCE_OK)
        return;

    memset(&primary, 0, sizeof primary);
    primary.branch_index = primary_idx;
    /* the sampler records each alternative's margin AGAINST the primary's
       SSMax score, so the primary score is recovered from the top
       alternative (score + margin); with no alternative the route is
       unchallenged by definition */
    primary.route_score = count > 0
        ? alts[0].route_score + alts[0].contrast_margin
        : 1.0f;
    {
        const char *branch_name = branches[primary_idx].name;
        size_t branch_name_len = strnlen(
            branch_name, sizeof primary.branch_name - 1u);
        memcpy(primary.branch_name, branch_name, branch_name_len);
        primary.branch_name[branch_name_len] = '\0';
    }
    consistency = cce_router_consistency_score(&primary, alts, count);

    len = snprintf(h->counterfactual_last, sizeof h->counterfactual_last,
                   "goal=%s unit=%s consistency=%.6f alternatives=%d",
                   goal_tag, branches[primary_idx].name,
                   (double)consistency, count);
    if (len < 0) {
        h->counterfactual_last[0] = '\0';
        return;
    }
    for (k = 0; k < count && (size_t)len < sizeof h->counterfactual_last;
         ++k) {
        int wrote = snprintf(h->counterfactual_last + len,
                             sizeof h->counterfactual_last - (size_t)len,
                             " alt%d=%s:%.6f", k, alts[k].branch_name,
                             (double)alts[k].route_score);
        if (wrote < 0) break;
        len += wrote;
    }
    fprintf(stderr, "CNET_COUNTERFACTUAL REPORT %s\n", h->counterfactual_last);
}

CNET_API int soul_counterfactual_last(SoulHost *h, char *out, int out_cap) {
    size_t need;
    if (!h || !h->loaded || !out || out_cap <= 0) return -1;
    if (!h->counterfactual_enabled) return -2;
    if (h->counterfactual_last[0] == '\0') return -3;
    need = strlen(h->counterfactual_last) + 1;
    if (need > (size_t)out_cap) {
        out[0] = '\0';
        return -4;
    }
    memcpy(out, h->counterfactual_last, need);
    return 0;
}

CNET_API int soul_route(SoulHost *h, const char *goal_tag,
                        const double *in, int in_cap, double *out, int out_cap) {
    RegistryEntry *entry;
    BinaryTransformNetwork *btn;
    Port input, goal;
    size_t in_total, out_total;
    char name[CNB_NAME_MAX];
    RoutePlan plan;
    int rc;
    double t0, t_plan0, route_ms, total_ms;

    if (!h || !h->loaded || !goal_tag || !in || !out) return -1;
    t0 = soul_mono_ms();
    /* Each routed query starts with no counterfactual evidence; only a
       SERVED answer may attach a report, so a refusal never carries stale
       metadata and refusal semantics stay untouched. */
    if (h->counterfactual_enabled) h->counterfactual_last[0] = '\0';

    /* the unit that owns a goal tag is named "acq_<tag>" (flagship convention);
       take its REAL input/output ports instead of fabricating them. */
    snprintf(name, sizeof name, "acq_%s", goal_tag);
    entry = soul_find_unit(h, name);
    if (!entry) {
        soul_emit_route_log(h, "soul_route", CNET_ROUTE_MECH_NO_PLAN, goal_tag,
                            NULL, -2, SOUL_SOURCE_NONE, 0.0,
                            soul_mono_ms() - t0);
        return -2;
    }
    btn = entry->btn;
    if (btn->input_port_count < 1 || btn->output_port_count < 1) {
        soul_emit_route_log(h, "soul_route", CNET_ROUTE_MECH_NO_PLAN, goal_tag,
                            NULL, -2, SOUL_SOURCE_NONE, 0.0,
                            soul_mono_ms() - t0);
        return -2;
    }
    input = btn->input_ports[0];
    goal  = btn->output_ports[0];
    in_total  = ports_total(&input, 1);
    out_total = ports_total(&goal, 1);

    if ((int)in_total > in_cap || (int)out_total > out_cap) {
        soul_emit_route_log(h, "soul_route", CNET_ROUTE_MECH_NO_PLAN, goal_tag,
                            NULL, -4, SOUL_SOURCE_NONE, 0.0,
                            soul_mono_ms() - t0);
        return -4;
    }

    memset(&plan, 0, sizeof plan);
    t_plan0 = soul_mono_ms();
    if (route_plan(&h->reg, input, goal, &plan) != 0 || plan.length == 0) {
        route_ms = soul_mono_ms() - t_plan0;
        /* Miss: note gap for the learner; try residual Tier C if bound. */
        if (h->gap_inbox[0]) {
            gap_inbox_note_no_plan(h->gap_inbox, input, goal);
            h->gap_notes++;
        }
        if (soul_try_residual_answer(h, input, goal, in, in_total, out,
                                     out_total) == 0) {
            total_ms = soul_mono_ms() - t0;
            soul_emit_route_log(h, "soul_route", CNET_ROUTE_MECH_RESIDUAL,
                                goal_tag, NULL, (int)out_total,
                                SOUL_SOURCE_RESIDUAL, route_ms, total_ms);
            return (int)out_total;
        }
        h->last_source = SOUL_SOURCE_NONE;
        total_ms = soul_mono_ms() - t0;
        soul_emit_route_log(h, "soul_route", CNET_ROUTE_MECH_NO_PLAN, goal_tag,
                            NULL, -3, SOUL_SOURCE_NONE, route_ms, total_ms);
        return -3;
    }
    route_ms = soul_mono_ms() - t_plan0;
    rc = route_execute(&plan, in, in_total, out, out_total);
    /* SHADOW evidence only, strictly after the answer bytes are final: the
       report can never change `out`, the return code, or any refusal. */
    if (rc == 0 && h->counterfactual_enabled)
        soul_counterfactual_shadow(h, goal_tag, entry, in, in_total);
    total_ms = soul_mono_ms() - t0;
    if (rc == 0) {
        h->certified_serves++;
        h->last_source = SOUL_SOURCE_CERTIFIED;
        soul_emit_route_log(h, "soul_route", CNET_ROUTE_MECH_PLANNER, goal_tag,
                            &plan, (int)out_total, SOUL_SOURCE_CERTIFIED,
                            route_ms, total_ms);
        return (int)out_total;
    }
    h->last_source = SOUL_SOURCE_NONE;
    soul_emit_route_log(h, "soul_route", CNET_ROUTE_MECH_PLANNER, goal_tag,
                        &plan, -5, SOUL_SOURCE_NONE, route_ms, total_ms);
    return -5;
}

CNET_API int soul_unit_reliability_milli(SoulHost *h, const char *name) {
    RegistryEntry *entry = soul_find_unit(h, name);
    double rel;
    if (!entry) return -2;
    rel = btn_reliability(entry->btn);
    return (int)(rel * 1000.0 + 0.5);
}

CNET_API int soul_health_tick(SoulHost *h, long long *counts, int counts_cap) {
    SpecialistHealthConfig cfg;
    SpecialistHealthReport rep;
    long long full[SOUL_HEALTH_COUNTS];
    int i, n;
    if (!h || !h->loaded || !counts || counts_cap <= 0) return -1;
    specialist_health_config_defaults(&cfg);
    cfg.contracts = soul_contract_lookup;
    cfg.contracts_ctx = h;
    if (specialist_health_pass(&h->reg, &cfg, &rep) != 0) return -2;
    /* P5: promote residual traces into certified+sealed units when ripe. */
    (void)soul_structure_mine(h);
    full[0] = (long long)rep.entries;
    full[1] = (long long)rep.demoted_by_audit;
    full[2] = (long long)rep.labeled_from_contract;
    full[3] = (long long)rep.labeled_via_teacher;
    full[4] = (long long)rep.heal_attempted;
    full[5] = (long long)rep.healed;
    full[6] = (long long)rep.promoted_provisional;
    full[7] = (long long)rep.shadows_promoted;
    full[8] = (long long)rep.reset_remaining;
    for (i = 0; i < 4; ++i) full[9 + i] = (long long)rep.trust[i];
    n = counts_cap < SOUL_HEALTH_COUNTS ? counts_cap : SOUL_HEALTH_COUNTS;
    for (i = 0; i < n; ++i) counts[i] = full[i];
    return n;
}

CNET_API int soul_unit_axes(SoulHost *h, const char *name,
                            int *trust, int *role) {
    SpecialistTrust t;
    SpecialistRole r;
    if (!h || !h->loaded || !name) return -1;
    if (specialist_axes(&h->reg, name, &t, &r) != 0) return -2;
    if (trust) *trust = (int)t;
    if (role) *role = (int)r;
    return 0;
}

CNET_API int soul_unit_evidence(SoulHost *h, const char *name,
                                char *out, int out_cap) {
    CnetEvidenceBundle b;
    size_t need;
    if (!h || !h->loaded || !name || !out || out_cap <= 0) return -1;
    memset(&b, 0, sizeof b);
    /* Prefer sidecar (carries CF stability + rollback) when present. */
    if (h->evidence_store[0] &&
        cnet_evidence_store_get(h->evidence_store, name, &b) == 0) {
        /* ok */
    } else if (cnet_evidence_bundle_from_base(&h->base, name, NULL, &b) != 0) {
        return -2;
    }
    if (cnet_evidence_bundle_format_json(&b, out, (size_t)out_cap) != 0) {
        out[0] = '\0';
        return -4;
    }
    need = strlen(out) + 1;
    if (need > (size_t)out_cap) {
        out[0] = '\0';
        return -4;
    }
    return 0;
}

CNET_API int soul_unit_health_layers(SoulHost *h, const char *name,
                                     char *out, int out_cap) {
    CnetHealthLayerConfig cfg;
    CnetUnitHealthLayers u;
    if (!h || !h->loaded || !name || !out || out_cap <= 0) return -1;
    cnet_health_layer_config_defaults(&cfg);
    cfg.contracts = soul_contract_lookup;
    cfg.contracts_ctx = h;
    cfg.utility_min_evidence = 0; /* certified units may be freshly sealed */
    if (cnet_health_check_unit(&h->reg, name, &cfg, &u) != 0) return -2;
    if (u.layer[CNET_HEALTH_LAYER_REGISTRY] == CNET_HEALTH_FAIL) return -2;
    if (cnet_health_layers_format_json(&u, out, (size_t)out_cap) != 0) {
        out[0] = '\0';
        return -4;
    }
    return 0;
}

static int request_port(Port *p, int family, int width, int count,
                        const char *tag) {
    memset(p, 0, sizeof *p);
    if (family < PORT_RAW || family > PORT_CONCEPT || width <= 0 || count <= 0)
        return -1;
    p->family = (PortFamily)family;
    p->field_width = (size_t)width;
    p->field_count = (size_t)count;
    if (tag && tag[0] && port_set_tag(p, tag) != 0) return -1;
    return 0;
}

CNET_API int soul_request(SoulHost *h,
                          int in_family, int in_width, int in_count,
                          const char *in_tag,
                          int goal_family, int goal_width, int goal_count,
                          const char *goal_tag,
                          const double *in, int in_len,
                          double *out, int out_cap) {
    Port input, goal;
    RoutePlan plan;
    size_t in_total, out_total;
    double t0, t_plan0, route_ms, total_ms;

    if (!h || !h->loaded) return -1;
    t0 = soul_mono_ms();
    /* a request needs REAL types on both ends: an untagged input port is a
       wildcard, which trivially "already satisfies" any same-shape goal
       (the 0-length identity) — such a request is unservable by
       construction and would mint an unreachable-by-default unit */
    if (!in_tag || !in_tag[0] || !goal_tag || !goal_tag[0]) return -1;
    if (request_port(&input, in_family, in_width, in_count, in_tag) != 0 ||
        request_port(&goal, goal_family, goal_width, goal_count,
                     goal_tag) != 0)
        return -1;
    in_total = input.field_width * input.field_count;
    out_total = goal.field_width * goal.field_count;

    memset(&plan, 0, sizeof plan);
    t_plan0 = soul_mono_ms();
    /* length 0 = the wildcard-tag identity plan ("input already satisfies
       the goal type") — a capability request wants a PRODUCING unit, so it
       counts as no plan, same rule as soul_route. */
    if (route_plan(&h->reg, input, goal, &plan) != 0 || plan.length == 0) {
        route_ms = soul_mono_ms() - t_plan0;
        /* Novel goal: note for the learner; residual may answer immediately. */
        if (h->gap_inbox[0]) {
            gap_inbox_note_no_plan(h->gap_inbox, input, goal);
            h->gap_notes++;
        }
        if (!in) {
            /* Probe: residual cannot claim certified capability. */
            h->last_source = SOUL_SOURCE_NONE;
            total_ms = soul_mono_ms() - t0;
            soul_emit_route_log(h, "soul_request", CNET_ROUTE_MECH_NO_PLAN,
                                goal_tag, NULL, -3, SOUL_SOURCE_NONE,
                                route_ms, total_ms);
            return -3;
        }
        if ((size_t)in_len != in_total || !out ||
            (size_t)out_cap < out_total) {
            soul_emit_route_log(h, "soul_request", CNET_ROUTE_MECH_NO_PLAN,
                                goal_tag, NULL, -4, SOUL_SOURCE_NONE,
                                route_ms, soul_mono_ms() - t0);
            return -4;
        }
        if (soul_try_residual_answer(h, input, goal, in, in_total, out,
                                     out_total) == 0) {
            total_ms = soul_mono_ms() - t0;
            soul_emit_route_log(h, "soul_request", CNET_ROUTE_MECH_RESIDUAL,
                                goal_tag, NULL, (int)out_total,
                                SOUL_SOURCE_RESIDUAL, route_ms, total_ms);
            return (int)out_total;
        }
        h->last_source = SOUL_SOURCE_NONE;
        total_ms = soul_mono_ms() - t0;
        soul_emit_route_log(h, "soul_request", CNET_ROUTE_MECH_NO_PLAN,
                            goal_tag, NULL, -3, SOUL_SOURCE_NONE,
                            route_ms, total_ms);
        return -3;
    }
    route_ms = soul_mono_ms() - t_plan0;
    if (!in) {
        h->last_source = SOUL_SOURCE_PROBE;
        total_ms = soul_mono_ms() - t0;
        soul_emit_route_log(h, "soul_request", CNET_ROUTE_MECH_PROBE, goal_tag,
                            &plan, 0, SOUL_SOURCE_PROBE, route_ms, total_ms);
        return 0;  /* capability probe: plannable, not executed */
    }
    if ((size_t)in_len != in_total || !out ||
        (size_t)out_cap < out_total) {
        soul_emit_route_log(h, "soul_request", CNET_ROUTE_MECH_PLANNER,
                            goal_tag, &plan, -4, SOUL_SOURCE_NONE,
                            route_ms, soul_mono_ms() - t0);
        return -4;
    }
    if (route_execute(&plan, in, in_total, out, out_total) != 0) {
        h->last_source = SOUL_SOURCE_NONE;
        total_ms = soul_mono_ms() - t0;
        soul_emit_route_log(h, "soul_request", CNET_ROUTE_MECH_PLANNER,
                            goal_tag, &plan, -5, SOUL_SOURCE_NONE,
                            route_ms, total_ms);
        return -5;
    }
    h->certified_serves++;
    h->last_source = SOUL_SOURCE_CERTIFIED;
    total_ms = soul_mono_ms() - t0;
    soul_emit_route_log(h, "soul_request", CNET_ROUTE_MECH_PLANNER, goal_tag,
                        &plan, (int)out_total, SOUL_SOURCE_CERTIFIED,
                        route_ms, total_ms);
    return (int)out_total;
}

/* Find a native unit whose recorded provenance points at descriptor
   `oracle_name` and recover its sealed contract into *out (owns_data). The
   oracle taught that unit, so the unit's own sealed exemplars are the evidence
   the remounted adapter must reproduce. Returns 0 with *out filled (caller owns
   it), or -1 if no provenance-linked unit recovers a contract. */
static int soul_recover_provenance_contract(SoulHost *h, const char *oracle_name,
                                            Contract *out) {
    size_t i;
    for (i = 0; i < h->base.unit_count; ++i) {
        if (strcmp(h->base.units[i].provenance, oracle_name) != 0) continue;
        {
            BinaryTransformNetwork tmp;
            memset(&tmp, 0, sizeof tmp);
            if (cnb_get_unit(&h->base, h->base.units[i].name, &tmp, out) == 0) {
                btn_free(&tmp);        /* the native BTN is already live in the registry */
                return 0;
            }
            /* recovery failed on this unit — keep scanning for another teacher */
        }
    }
    return -1;
}

CNET_API int soul_mount_oracles(SoulHost *h, SoulOracleResolver resolver,
                                void *rctx, SoulMountReport *report) {
    SoulMountReport rep;
    size_t i, n;

    memset(&rep, 0, sizeof rep);
    if (!h || !h->loaded) return -1;
    if (!resolver) return -1;   /* descriptor-only projection is soul_open's default */

    n = h->base.oracle_count;
    /* Allocate the stable per-descriptor slots once (never realloc'd, so an
       adapter's borrowed &bound_entries[i] can't dangle). */
    if (n > 0 && h->bound_cap == 0) {
        h->bound_entries = (OracleEntry *)calloc(n, sizeof *h->bound_entries);
        h->bound_adapters =
            (BinaryTransformNetwork *)calloc(n, sizeof *h->bound_adapters);
        h->bound_contracts = (Contract **)calloc(n, sizeof *h->bound_contracts);
        h->bound_mounted = (unsigned char *)calloc(n, sizeof *h->bound_mounted);
        if (!h->bound_entries || !h->bound_adapters || !h->bound_contracts ||
            !h->bound_mounted) {
            free(h->bound_entries);   h->bound_entries = NULL;
            free(h->bound_adapters);  h->bound_adapters = NULL;
            free(h->bound_contracts); h->bound_contracts = NULL;
            free(h->bound_mounted);   h->bound_mounted = NULL;
            return -2;
        }
        h->bound_cap = n;
    }

    for (i = 0; i < n; ++i) {
        const CnbOracleDesc *desc = &h->base.oracles[i];
        SoulOracleBinding b;
        Contract c;
        Contract *hc;
        Specialist s;
        OracleEntry *e;
        uint64_t asserted;
        size_t cost;

        if (h->bound_mounted[i]) continue;   /* idempotent: already live */

        memset(&b, 0, sizeof b);
        if (resolver(desc->name, desc->kind, &b, rctx) != 0 || b.fn == NULL) {
            rep.unbound++;
            continue;
        }
        /* Identity integrity: the sealed descriptor MUST carry a digest and the
           caller MUST assert a matching identity. No digest, no asserted
           identity, or a mismatch is refused — never silent runtime trust. */
        asserted = b.has_identity ? cnet_oracle_identity_digest(&b.identity) : 0;
        if (desc->behavior_digest == 0 || asserted == 0 ||
            asserted != desc->behavior_digest) {
            rep.identity_mismatch++;
            continue;
        }
        /* Provenance evidence: a native unit's OWN sealed contract. */
        memset(&c, 0, sizeof c);
        if (soul_recover_provenance_contract(h, desc->name, &c) != 0) {
            rep.missing_provenance++;
            continue;
        }
        /* Allocate the retained contract before admission. Once the registry
           borrows the adapter there must be no fallible ownership step left. */
        hc = (Contract *)malloc(sizeof *hc);
        if (!hc) {
            contract_free(&c);
            if (report) *report = rep;
            return -2;
        }
        *hc = c;   /* move the owned tables; hc is now the sole owner */
        /* Build the bound entry in its stable slot; the adapter borrows it. */
        e = &h->bound_entries[i];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", desc->name);
        e->input_port = desc->input_port;
        e->output_port = desc->goal_port;
        e->fn = b.fn;
        e->ctx = b.ctx;
        e->identity = desc->identity;             /* authoritative sealed identity */
        e->behavior_digest = desc->behavior_digest;

        cost = e->output_port.field_width * e->output_port.field_count;
        /* Wrap as an ORACLE specialist (adapter over the resolved callback) and
           admit through the one door against the recovered sealed contract. The
           base-owned descriptor name is the borrowed registry name (stable for
           the host's life: registry_free runs before cnb_free in soul_close). */
        if (specialist_wrap_oracle(&s, &h->bound_adapters[i], e,
                                   desc->behavior_digest, cost,
                                   desc->name) != 0) {
            contract_free(hc);
            free(hc);
            rep.cert_failed++;
            continue;
        }
        if (specialist_admit(&h->reg, &s, hc) != 0) {
            btn_free(&h->bound_adapters[i]);      /* releases the adapter context */
            memset(&h->bound_adapters[i], 0, sizeof h->bound_adapters[i]);
            contract_free(hc);
            free(hc);
            rep.cert_failed++;
            continue;
        }
        h->bound_contracts[i] = hc;
        h->bound_mounted[i] = 1;
        h->mounted_count++;
        rep.mounted++;
    }

    if (report) *report = rep;
    return rep.mounted;
}

CNET_API int soul_unit_kind(SoulHost *h, const char *name, int *kind) {
    RegistryEntry *entry;
    if (!h || !h->loaded || !name) return -1;
    entry = soul_find_unit(h, name);
    if (!entry) return -2;
    if (kind) *kind = (int)entry->kind;
    return 0;
}

CNET_API int soul_mounted_oracle_count(SoulHost *h) {
    if (!h || !h->loaded || h->mounted_count > (size_t)INT_MAX) return -1;
    return (int)h->mounted_count;
}

CNET_API void soul_close(SoulHost *h) {
    size_t i;
    char state_dir[1024];
    if (!h) return;
    for (i = 0; i < h->contract_count; ++i) {
        contract_free(h->contracts[i]);
        free(h->contracts[i]);
    }
    free(h->contracts);
    free(h->contract_names);
    free(h->route_log_path);
    h->route_log_path = NULL;
    if (h->loaded) {
        /* Persist learned reliability + runtime sidecars BEFORE teardown so
           the next soul_open restores execution evidence (not the 0.5 prior). */
        if (h->base_path[0] &&
            soul_state_dir(h->base_path, state_dir, sizeof state_dir) == 0) {
            char serve_path[1100];
            (void)registry_persist_runtime_state(&h->reg, state_dir);
            /* Host-level serve counters (session evidence for ops). */
            if ((size_t)snprintf(serve_path, sizeof serve_path,
                                 "%s/soul_serve.stats", state_dir) <
                sizeof serve_path) {
                FILE *sf = fopen(serve_path, "w");
                if (sf) {
                    fprintf(sf,
                            "CNET_SERVE 1\n"
                            "certified_serves %llu\n"
                            "residual_serves %llu\n"
                            "gap_notes %llu\n"
                            "structure_mines %llu\n"
                            "structure_seals %llu\n",
                            (unsigned long long)h->certified_serves,
                            (unsigned long long)h->residual_serves,
                            (unsigned long long)h->gap_notes,
                            (unsigned long long)h->structure_mines,
                            (unsigned long long)h->structure_seals);
                    fclose(sf);
                }
            }
            if (h->evidence_store[0]) {
                size_t ui;
                for (ui = 0; ui < h->reg.count; ++ui) {
                    const char *nm = h->reg.entries[ui].name;
                    if (!nm) continue;
                    CnetEvidenceOpts eopts;
                    memset(&eopts, 0, sizeof eopts);
                    eopts.counterfactual_stability = -1.0f;
                    (void)cnet_evidence_record(&h->base, nm, h->evidence_store,
                                               &eopts);
                }
            }
        }
        /* Drop residual bind before freeing the model it points at. */
        hybrid_ai_free(&h->hybrid);
        if (h->owned_residual) {
            residual_gguf_close(h->owned_residual);
            h->owned_residual = NULL;
        }
        if (h->owned_residual_http) {
            residual_http_close(h->owned_residual_http);
            h->owned_residual_http = NULL;
        }
        /* registry_free first: it drops the registry's borrows of the adapter
           BTNs and their (base-owned) names. Then release each mounted adapter
           (frees its projection context) and the recovered contract it was
           certified against, before cnb_free reclaims the base. */
        registry_free(&h->reg);
        for (i = 0; i < h->bound_cap; ++i) {
            if (h->bound_mounted[i]) btn_free(&h->bound_adapters[i]);
            if (h->bound_contracts[i]) {
                contract_free(h->bound_contracts[i]);
                free(h->bound_contracts[i]);
            }
        }
        free(h->bound_entries);
        free(h->bound_adapters);
        free(h->bound_contracts);
        free(h->bound_mounted);
        cnb_free(&h->base);
    }
    free(h);
}
