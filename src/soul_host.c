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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct SoulHost {
    CnetBase base;
    PrimitiveRegistry reg;
    int loaded;
};

/* Sum of a contract's port totals (field_width * field_count), in doubles. */
static size_t ports_total(const Port *ports, size_t n) {
    size_t t = 0, i;
    for (i = 0; i < n; ++i) t += ports[i].field_width * ports[i].field_count;
    return t;
}

CNET_API int soul_open(const char *base_path, const char *model_path,
                       SoulHost **out) {
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
    registry_init(&h->reg);
    { size_t skipped = 0; (void)cnb_load_registry(&h->base, &h->reg, &skipped); }
    h->loaded = 1;
    *out = h;
    return 0;
}

CNET_API int soul_unit_dims(SoulHost *h, const char *name,
                            int *in_total, int *out_total) {
    BinaryTransformNetwork btn; Contract c;
    if (!h || !h->loaded || !name) return -1;
    memset(&btn, 0, sizeof btn); memset(&c, 0, sizeof c);
    if (cnb_get_unit(&h->base, name, &btn, &c) != 0) return -2;
    if (in_total)  *in_total  = (int)ports_total(c.input_ports,  c.input_port_count);
    if (out_total) *out_total = (int)ports_total(c.output_ports, c.output_port_count);
    btn_free(&btn); contract_free(&c);
    return 0;
}

CNET_API int soul_run(SoulHost *h, const char *name,
                      const double *in, double *out, int out_cap) {
    BinaryTransformNetwork btn; Contract c;
    size_t out_total, n;
    const double *result;
    if (!h || !h->loaded || !name || !in || !out || out_cap <= 0) return -1;
    memset(&btn, 0, sizeof btn); memset(&c, 0, sizeof c);
    if (cnb_get_unit(&h->base, name, &btn, &c) != 0) return -2;   /* MISS */

    out_total = ports_total(c.output_ports, c.output_port_count);
    result = btn_forward(&btn, in);
    if (!result) { btn_free(&btn); contract_free(&c); return -3; }

    n = (out_total < (size_t)out_cap) ? out_total : (size_t)out_cap;  /* never overflow */
    memcpy(out, result, n * sizeof(double));
    btn_free(&btn); contract_free(&c);
    return (int)out_total;   /* > out_cap signals truncation to the caller */
}

CNET_API int soul_route(SoulHost *h, const char *goal_tag,
                        const double *in, int in_cap, double *out, int out_cap) {
    BinaryTransformNetwork btn; Contract c;
    Port input, goal;
    size_t in_total, out_total;
    char name[CNB_NAME_MAX];
    RoutePlan plan;
    int rc;

    if (!h || !h->loaded || !goal_tag || !in || !out) return -1;

    /* the unit that owns a goal tag is named "acq_<tag>" (flagship convention);
       take its REAL input/output ports instead of fabricating them. */
    snprintf(name, sizeof name, "acq_%s", goal_tag);
    memset(&btn, 0, sizeof btn); memset(&c, 0, sizeof c);
    if (cnb_get_unit(&h->base, name, &btn, &c) != 0) return -2;
    if (c.input_port_count < 1 || c.output_port_count < 1) {
        btn_free(&btn); contract_free(&c); return -2;
    }
    input = c.input_ports[0];
    goal  = c.output_ports[0];
    in_total  = ports_total(&input, 1);
    out_total = ports_total(&goal, 1);
    btn_free(&btn); contract_free(&c);   /* only needed the ports; route uses the registry */

    if ((int)in_total > in_cap || (int)out_total > out_cap) return -4;

    memset(&plan, 0, sizeof plan);
    if (route_plan(&h->reg, input, goal, &plan) != 0 || plan.length == 0) return -3;
    rc = route_execute(&plan, in, in_total, out, out_total);
    return rc == 0 ? (int)out_total : -5;
}

CNET_API int soul_unit_reliability_milli(SoulHost *h, const char *name) {
    BinaryTransformNetwork btn; Contract c;
    double rel;
    if (!h || !h->loaded || !name) return -1;
    memset(&btn, 0, sizeof btn); memset(&c, 0, sizeof c);
    if (cnb_get_unit(&h->base, name, &btn, &c) != 0) return -2;
    rel = btn_reliability(&btn);
    btn_free(&btn); contract_free(&c);
    return (int)(rel * 1000.0 + 0.5);
}

CNET_API void soul_close(SoulHost *h) {
    if (!h) return;
    if (h->loaded) { registry_free(&h->reg); cnb_free(&h->base); }
    free(h);
}
