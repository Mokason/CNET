#include "../include/cnet_moe.h"
#include "../include/cnet_acct.h"

#include <stdio.h>
#include <string.h>

/* Optional: when registry_lora is linked, forward_with_lora is available. */
int registry_forward_with_lora(PrimitiveRegistry *reg, const char *name,
                               const double *input, double *out)
    __attribute__((weak));
int registry_lora_is_certified(const PrimitiveRegistry *reg, const char *name)
    __attribute__((weak));

static int unit_dims_ok(const RegistryEntry *e, Port in, Port goal,
                        size_t in_len, size_t out_cap) {
    size_t need_in, need_out;
    if (!e || !e->btn) return 0;
    need_in = e->btn->input_count;
    need_out = e->btn->output_count;
    if (need_in == 0 || need_out == 0) return 0;
    if (in_len < need_in || out_cap < need_out) return 0;
    (void)in;
    (void)goal;
    return 1;
}

static const RegistryEntry *find_named(const PrimitiveRegistry *reg,
                                       const char *name) {
    size_t i;
    if (!reg || !name || !name[0]) return NULL;
    for (i = 0; i < reg->count; i++) {
        if (reg->entries[i].name && strcmp(reg->entries[i].name, name) == 0)
            return &reg->entries[i];
    }
    return NULL;
}

int cnet_moe_try_hard(PrimitiveRegistry *reg, Port input_port, Port goal_port,
                      const double *input, size_t in_len, double *output,
                      size_t out_cap, CnetMoeHit *hit) {
    const char *tag;
    char name[96];
    const RegistryEntry *e;
    RoutePlan plan;

    if (hit) memset(hit, 0, sizeof *hit);
    if (!reg || !input || !output) return -1;
    tag = goal_port.tag;
    if (!tag || !tag[0]) return 1;

    /* Accept bare tag or acq_<tag> */
    e = find_named(reg, tag);
    if (!e) {
        snprintf(name, sizeof name, "acq_%s", tag);
        e = find_named(reg, name);
        if (e) tag = name;
    }
    if (!e || !unit_dims_ok(e, input_port, goal_port, in_len, out_cap))
        return 1;

    /* Prefer certified LoRA path when linked + certified */
    if (registry_forward_with_lora && registry_lora_is_certified &&
        registry_lora_is_certified(reg, e->name)) {
        if (registry_forward_with_lora(reg, e->name, input, output) == 0) {
            if (hit) {
                hit->hit = 1;
                hit->used_lora = 1;
                hit->steps = 1;
                snprintf(hit->unit, sizeof hit->unit, "%s", e->name);
            }
            return 0;
        }
    }

    memset(&plan, 0, sizeof plan);
    plan.steps[0] = e->btn;
    plan.names[0] = e->name;
    plan.length = 1;
    plan.strict = 0;
    if (route_execute(&plan, input, in_len, output, out_cap) != 0) return -1;
    if (hit) {
        hit->hit = 1;
        hit->used_lora = 0;
        hit->steps = 1;
        snprintf(hit->unit, sizeof hit->unit, "%s", e->name);
    }
    (void)input_port;
    return 0;
}
