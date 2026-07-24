#include "../../include/router.h"
#include "../../include/router/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================
 * Linear route planning + execution (including self-healing)
 * ======================================================================== */

/* Port shape+tag key (FNV-1a) — reject before strcmp in type tables. */
static uint64_t route_port_key(Port p) {
    uint64_t h = 14695981039346656037ULL;
    const unsigned char *t = (const unsigned char *)p.tag;
    h ^= (uint64_t)(unsigned)p.family;
    h *= 1099511628211ULL;
    h ^= (uint64_t)p.field_width;
    h *= 1099511628211ULL;
    h ^= (uint64_t)p.field_count;
    h *= 1099511628211ULL;
    for (; *t; t++) {
        h ^= (uint64_t)*t;
        h *= 1099511628211ULL;
    }
    return h ? h : 1ULL;
}

static int same_port_type(Port a, Port b) {
    return a.family == b.family &&
           a.field_width == b.field_width &&
           a.field_count == b.field_count &&
           strcmp(a.tag, b.tag) == 0;
}

/* Key-first type equality against the planner's type table. */
static int same_port_type_keyed(Port a, uint64_t ka, Port b, uint64_t kb) {
    if (ka != kb) return 0;
    return same_port_type(a, b);
}

extern int entry_usable(const PrimitiveRegistry *reg, size_t i);

int route_plan(
    const PrimitiveRegistry *reg,
    Port input_port,
    Port goal_port,
    RoutePlan *out
) {
    enum { K = ROUTE_MAX_STEPS };
    Port *types;
    uint64_t *type_keys;
    double *dist;
    int *via_prim;
    int *via_type;
    size_t max_types;
    size_t n_types = 0;
    size_t i;
    size_t k;
    size_t t;
    int best_k = -1;
    size_t best_t = 0;
    double best_score = 0.0;

    if (reg == NULL || out == NULL) {
        return -1;
    }

    max_types = reg->count + 1;
    types = malloc(max_types * sizeof(*types));
    type_keys = malloc(max_types * sizeof(*type_keys));
    dist = malloc((K + 1) * max_types * sizeof(*dist));
    via_prim = malloc((K + 1) * max_types * sizeof(*via_prim));
    via_type = malloc((K + 1) * max_types * sizeof(*via_type));
    if (types == NULL || type_keys == NULL || dist == NULL || via_prim == NULL ||
        via_type == NULL) {
        free(types);
        free(type_keys);
        free(dist);
        free(via_prim);
        free(via_type);
        return -1;
    }

    types[0] = input_port;
    type_keys[0] = route_port_key(input_port);
    n_types = 1;
    for (i = 0; i < reg->count; ++i) {
        const BinaryTransformNetwork *p = reg->entries[i].btn;
        Port outp;
        uint64_t ok;
        int seen = 0;

        if (!entry_usable(reg, i)) continue;
        if (p->input_port_count != 1 || p->output_port_count != 1) continue;
        outp = p->output_ports[0];
        ok = route_port_key(outp);
        for (t = 0; t < n_types; ++t) {
            if (same_port_type_keyed(types[t], type_keys[t], outp, ok)) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            types[n_types] = outp;
            type_keys[n_types] = ok;
            n_types++;
        }
    }

    /* Only clear the used (k, type) cells — not the full max_types slab. */
    for (k = 0; k <= K; ++k) {
        for (t = 0; t < n_types; ++t) {
            dist[k * max_types + t] = 0.0;
            via_prim[k * max_types + t] = -1;
            via_type[k * max_types + t] = -1;
        }
    }
    dist[0] = 1.0;

    for (k = 1; k <= K; ++k) {
        for (i = 0; i < reg->count; ++i) {
            BinaryTransformNetwork *p = reg->entries[i].btn;
            Port outp;
            uint64_t ok;
            size_t ti;
            double rel;

            if (!entry_usable(reg, i)) continue;
            if (p->input_port_count != 1 || p->output_port_count != 1) continue;

            outp = p->output_ports[0];
            ok = route_port_key(outp);
            for (ti = 0; ti < n_types; ++ti) {
                if (same_port_type_keyed(types[ti], type_keys[ti], outp, ok))
                    break;
            }
            if (ti >= n_types) continue;
            rel = btn_reliability(p);
            for (t = 0; t < n_types; ++t) {
                double cand;
                if (dist[(k - 1) * max_types + t] <= 0.0 ||
                    !port_compatible(types[t], p->input_ports[0])) continue;
                cand = dist[(k - 1) * max_types + t] * rel;
                if (cand > dist[k * max_types + ti]) {
                    dist[k * max_types + ti] = cand;
                    via_prim[k * max_types + ti] = (int)i;
                    via_type[k * max_types + ti] = (int)t;
                }
            }
        }
    }

    for (k = 0; k <= K; ++k) {
        for (t = 0; t < n_types; ++t) {
            if (dist[k * max_types + t] > best_score &&
                port_compatible(types[t], goal_port)) {
                best_score = dist[k * max_types + t];
                best_k = (int)k;
                best_t = t;
            }
        }
    }

    if (best_k >= 0) {
        out->length = (size_t)best_k;
        out->goal = goal_port;
        out->strict = 0;
        k = (size_t)best_k;
        t = best_t;
        while (k > 0) {
            int pi = via_prim[k * max_types + t];
            out->steps[k - 1] = reg->entries[pi].btn;
            out->names[k - 1] = reg->entries[pi].name;
            t = (size_t)via_type[k * max_types + t];
            --k;
        }
    }

    free(types);
    free(type_keys);
    free(dist);
    free(via_prim);
    free(via_type);
    return best_k >= 0 ? 0 : -1;
}

/* --- execution --------------------------------------------------------- */

/* Live-serving adapter hook; NULL until the opt-in adapter layer installs it.
   Defined here (a core router TU) so route.c and dag_full.c share one symbol. */
CnetLoraServeHook g_cnet_lora_serve_hook = NULL;

int route_execute_ex(
    const RoutePlan *plan,
    const double *input,
    size_t in_len,
    double *output,
    size_t out_cap,
    ExecFault *fault
) {
    double *buf_a;
    double *buf_b;
    double *current;
    double *next;
    size_t max_width = 0;
    size_t cur_len;
    size_t s;
    size_t i;

    if (fault != NULL) {
        fault->primitive = NULL;
        fault->name = NULL;
        fault->step_index = 0;
    }

    if (plan == NULL || input == NULL || output == NULL) {
        return -1;
    }

    if (plan->length == 0) {
        size_t total = plan->goal.field_width * plan->goal.field_count;
        if (in_len != total || out_cap < total ||
            !port_validate(plan->goal, input)) {
            return -1;
        }
        return port_canonicalize(plan->goal, input, output);
    }

    if (in_len != plan->steps[0]->input_count) {
        return -1;
    }

    for (s = 0; s < plan->length; ++s) {
        size_t ic = plan->steps[s]->input_count;
        size_t oc = plan->steps[s]->output_count;
        if (ic > max_width) max_width = ic;
        if (oc > max_width) max_width = oc;
    }

    buf_a = malloc(max_width * sizeof(double));
    buf_b = malloc(max_width * sizeof(double));
    if (buf_a == NULL || buf_b == NULL) {
        free(buf_a);
        free(buf_b);
        return -1;
    }

    current = buf_a;
    next = buf_b;
    for (i = 0; i < in_len; ++i) current[i] = input[i];
    cur_len = in_len;

    for (s = 0; s < plan->length; ++s) {
        BinaryTransformNetwork *p = (BinaryTransformNetwork *)plan->steps[s];
        const double *raw;

        if (p->input_port_count != 1 || p->output_port_count != 1 ||
            !port_validate(p->input_ports[0], current) ||
            port_canonicalize(p->input_ports[0], current, next) != 0) {
            free(buf_a);
            free(buf_b);
            return -1;
        }
        raw = btn_forward(p, next);
        /* Live adapter: add any attached low-rank delta to the raw output before
           validation, so the adapted output is what is validated and served. */
        if (g_cnet_lora_serve_hook)
            g_cnet_lora_serve_hook(p, next, (double *)raw, p->output_count);

        if (port_validate(p->output_ports[0], raw)) {
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
            atomic_fetch_add_explicit(&p->output_successes, 1, memory_order_relaxed);
#else
            p->output_successes++;
#endif
        } else {
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
            atomic_fetch_add_explicit(&p->output_failures, 1, memory_order_relaxed);
#else
            p->output_failures++;
#endif
            if (plan->strict) {
                if (fault != NULL) {
                    fault->primitive = p;
                    fault->name = plan->names[s];
                    fault->step_index = s;
                }
                free(buf_a);
                free(buf_b);
                return -1;
            }
        }
        if (port_canonicalize(p->output_ports[0], raw, current) != 0) {
            free(buf_a);
            free(buf_b);
            return -1;
        }
        cur_len = p->output_count;
    }

    if (out_cap < cur_len) {
        free(buf_a);
        free(buf_b);
        return -1;
    }
    for (i = 0; i < cur_len; ++i) output[i] = current[i];

    free(buf_a);
    free(buf_b);
    return 0;
}

int route_execute(
    const RoutePlan *plan,
    const double *input,
    size_t in_len,
    double *output,
    size_t out_cap
) {
    return route_execute_ex(plan, input, in_len, output, out_cap, NULL);
}

int route_execute_healing(
    PrimitiveRegistry *reg,
    Port input_port,
    Port goal_port,
    const double *input,
    size_t in_len,
    double *output,
    size_t out_cap,
    size_t max_reroutes
) {
    size_t attempt;
    if (reg == NULL) {
        return -1;
    }
    for (attempt = 0; attempt <= max_reroutes; ++attempt) {
        RoutePlan plan;
        ExecFault fault;
        if (route_plan(reg, input_port, goal_port, &plan) != 0) {
            return -1;
        }
        plan.strict = 1;
        if (route_execute_ex(&plan, input, in_len, output, out_cap, &fault) == 0) {
            return 0;
        }
        if (fault.primitive == NULL || fault.name == NULL) {
            return -1;
        }
        registry_set_state(reg, fault.name, PRIM_RESET);
    }
    return -1;
}



