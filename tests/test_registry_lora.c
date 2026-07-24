/* test_registry_lora — wire a cce_lora adapter into a REAL registry unit's
   labeled retrain queue and validate end to end:

   build a base BTN, register it, park faults and supply teacher-corrected
   targets (base + a low-rank correction) into the unit's RetrainQueue, then
   registry_teach_lora from that queue and check that serving with the adapter
   recovers the teacher on held-out inputs while detaching restores the exact
   base. Uses the genuine registry machinery (registry_record_fault /
   registry_supply_label / btn_forward), not a mock. */

#include "../include/nn.h"
#include "../include/router.h"
#include "../include/router/registry_lora.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); g_fail++; } \
                         else printf("ok: %s\n", m); } while (0)

static uint32_t St = 20260724u;
static uint32_t rnd(void) { St = St * 1664525u + 1013904223u; return St; }
static double rd(void) { return ((rnd() >> 8) * (1.0 / 16777216.0)) * 2.0 - 1.0; }

static Port P(PortFamily f, size_t w, size_t c) { Port p; p.family = f; p.field_width = w; p.field_count = c; p.tag[0] = '\0'; return p; }

static double maxabsdiff(const double *a, const double *b, int n) {
    double m = 0.0; for (int i = 0; i < n; i++) { double d = fabs(a[i] - b[i]); if (d > m) m = d; } return m;
}

int main(void) {
    const int in = 16, out = 8, k = 2;
    /* low-rank teacher correction Wc = U V (scaled small) */
    double U[16 * 2], V[2 * 8];
    for (int i = 0; i < in * k; i++) U[i] = rd();
    for (int i = 0; i < k * out; i++) V[i] = rd();

    BinaryTransformNetwork base = {0};
    CHECK(btn_init(&base, in, out, 8, 16, 0.1, 1u) == 0, "btn_init base");
    /* RAW ports: port_canonicalize is identity, so the executor feeds the btn
       (and the adapter) the exact input — keeps the numeric check clean. */
    btn_set_ports(&base, P(PORT_RAW, in, 1), P(PORT_RAW, out, 1));

    PrimitiveRegistry reg;
    registry_init(&reg);
    CHECK(registry_add(&reg, &base, "unit") == 0, "registry_add unit");

    /* correction(x)[o] = 0.15 * sum_i x[i] * sum_j U[i,j]V[j,o] */
    #define MKCORR(x, corr) do { \
        for (int o = 0; o < out; o++) { double s = 0; \
            for (int i = 0; i < in; i++) { double w = 0; \
                for (int j = 0; j < k; j++) w += U[i*k+j]*V[j*out+o]; s += (x)[i]*w; } \
            (corr)[o] = 0.15 * s / in; } } while (0)

    /* populate the labeled queue: park a fault then supply base+correction target */
    const int n = 300;
    double x[16], raw[8], tgt[8];
    int fault_ok = 1, label_ok = 1;
    for (int s = 0; s < n; s++) {
        for (int i = 0; i < in; i++) x[i] = rd();
        const double *b = btn_forward(&base, x);
        for (int o = 0; o < out; o++) raw[o] = b[o];         /* copy: buffer reused */
        double corr[8]; MKCORR(x, corr);
        for (int o = 0; o < out; o++) tgt[o] = raw[o] + corr[o];
        if (registry_record_fault(&reg, "unit", x, raw) != 0) fault_ok = 0;
        if (registry_supply_label(&reg, "unit", x, tgt) != 0) label_ok = 0;
    }
    CHECK(fault_ok, "record_fault parked all inputs");
    CHECK(label_ok, "supply_label attached all teacher targets");

    /* teach the adapter from the real queue */
    registry_lora_opts opt = registry_lora_defaults();
    opt.rank = 4; opt.train.epochs = 800; opt.train.lr = 0.02f;
    registry_lora_stats st;
    int rc = registry_teach_lora(&reg, "unit", &opt, &st);
    CHECK(rc == 0, "registry_teach_lora returns 0");
    printf("   (pairs=%zu in=%d out=%d rank=%d  pre_mse=%.5g post_mse=%.5g  params=%zu vs dense=%zu)\n",
           st.pairs, st.in_dim, st.out_dim, st.rank, st.pre_mse, st.post_mse, st.params, st.dense_params);
    CHECK(st.pairs >= (size_t)n, "queue supplied all labeled pairs");
    CHECK(st.post_mse < st.pre_mse * 0.1, "adapter cuts residual error >10x");
    CHECK(st.params < st.dense_params, "fewer trained params than a dense output update");
    CHECK(registry_has_lora(&reg, "unit"), "adapter attached to the unit");

    /* held-out serving: with adapter ~= teacher; detached == exact base */
    double err_base = 0, err_adp = 0, err_detach = 0;
    const int m = 128;
    for (int t = 0; t < m; t++) {
        for (int i = 0; i < in; i++) x[i] = rd();
        const double *b = btn_forward(&base, x);
        double basecpy[8]; for (int o = 0; o < out; o++) basecpy[o] = b[o];
        double corr[8]; MKCORR(x, corr);
        double teacher[8]; for (int o = 0; o < out; o++) teacher[o] = basecpy[o] + corr[o];

        double served[8];
        registry_forward_with_lora(&reg, "unit", x, served);
        for (int o = 0; o < out; o++) {
            err_base   += fabs(basecpy[o] - teacher[o]);   /* base vs teacher */
            err_adp    += fabs(served[o]  - teacher[o]);   /* adapter vs teacher */
        }
    }
    printf("   (held-out L1: base-vs-teacher=%.4f  adapter-vs-teacher=%.4f)\n",
           err_base / (m * out), err_adp / (m * out));
    CHECK(err_adp < err_base * 0.25, "served adapter recovers teacher on held-out inputs");

    /* ---- certify-before-serve gate: uncertified adapter must NOT be served ---- */
    {
        RoutePlan plan; memset(&plan, 0, sizeof plan);
        plan.steps[0] = &base; plan.names[0] = "unit"; plan.length = 1; plan.strict = 0;
        for (int i = 0; i < in; i++) x[i] = rd();
        const double *bp = btn_forward(&base, x);
        double base_only[8]; for (int o = 0; o < out; o++) base_only[o] = bp[o];

        registry_lora_enable_serving(&reg);
        CHECK(!registry_lora_is_certified(&reg, "unit"), "freshly-taught adapter is uncertified");
        double out_uncert[8];
        route_execute_ex(&plan, x, in, out_uncert, out, NULL);
        CHECK(maxabsdiff(out_uncert, base_only, out) == 0.0,
              "GATE: uncertified adapter is NOT served (executor returns frozen base)");

        /* validation set: target = base + the same planted correction (MSE mode) */
        const size_t nv = 96;
        double *vx = malloc(nv * in * sizeof(double)), *vt = malloc(nv * out * sizeof(double));
        for (size_t s = 0; s < nv; s++) {
            double xx[16]; for (int i = 0; i < in; i++) xx[i] = rd();
            const double *b = btn_forward(&base, xx);
            double corr[8]; MKCORR(xx, corr);
            for (int i = 0; i < in; i++) vx[s * in + i] = xx[i];
            for (int o = 0; o < out; o++) vt[s * out + o] = b[o] + corr[o];
        }
        registry_lora_cert_policy pol = registry_lora_cert_defaults();
        pol.argmax_mode = 0; pol.max_regressions = 0; pol.min_net_gain = 1;
        registry_lora_cert_report rep;
        int pass = registry_certify_lora(&reg, "unit", vx, vt, nv, &pol, &rep);
        printf("   (cert: base_mse=%.4g adapter_mse=%.4g fixes=%d regress=%d -> %s)\n",
               rep.base_mse, rep.adapter_mse, rep.fixes, rep.regressions, pass ? "PASS" : "FAIL");
        CHECK(pass == 1 && registry_lora_is_certified(&reg, "unit"), "certify PASS sets the serve gate");
        double out_cert[8];
        route_execute_ex(&plan, x, in, out_cert, out, NULL);
        CHECK(maxabsdiff(out_cert, base_only, out) > 1e-6,
              "GATE: certified adapter IS served (delta applied)");

        /* a policy the adapter can't meet clears the gate */
        registry_lora_cert_policy strict = pol; strict.min_net_gain = (int)nv + 999;
        int pass2 = registry_certify_lora(&reg, "unit", vx, vt, nv, &strict, NULL);
        CHECK(pass2 == 0 && !registry_lora_is_certified(&reg, "unit"),
              "GATE: a failing certification clears the serve gate");

        registry_certify_lora(&reg, "unit", vx, vt, nv, &pol, NULL);  /* re-arm for the delta tests */
        CHECK(registry_lora_is_certified(&reg, "unit"), "re-certified for the executor delta checks");
        registry_lora_disable_serving(&reg);
        free(vx); free(vt);
    }

    /* ---- executor path: route_execute_ex applies the delta only when serving on ---- */
    {
        RoutePlan plan;
        memset(&plan, 0, sizeof plan);
        plan.steps[0] = &base; plan.names[0] = "unit"; plan.length = 1; plan.strict = 0;
        for (int i = 0; i < in; i++) x[i] = rd();
        const double *bp = btn_forward(&base, x);
        double base_only[8]; for (int o = 0; o < out; o++) base_only[o] = bp[o];
        double ref_on[8];   /* base + delta, via the direct serve path */
        registry_forward_with_lora(&reg, "unit", x, ref_on);

        double out_off[8], out_on[8];
        registry_lora_disable_serving(&reg);
        int rc_off = route_execute_ex(&plan, x, in, out_off, out, NULL);
        registry_lora_enable_serving(&reg);
        CHECK(reg.lora_serving_enabled, "enable_serving sets the flag");
        int rc_on = route_execute_ex(&plan, x, in, out_on, out, NULL);
        registry_lora_disable_serving(&reg);

        CHECK(rc_off == 0 && rc_on == 0, "route_execute_ex runs with serving off and on");
        double d_off = maxabsdiff(out_off, base_only, out);
        double d_on_vs_off = maxabsdiff(out_on, out_off, out);
        double d_on_vs_ref = maxabsdiff(out_on, ref_on, out);
        printf("   (executor: off-vs-base=%.2e  on-vs-off=%.3e  on-vs-(base+delta)=%.2e)\n",
               d_off, d_on_vs_off, d_on_vs_ref);
        CHECK(d_off == 0.0, "serving OFF: executor output is the exact frozen base");
        CHECK(d_on_vs_off > 1e-6, "serving ON: executor output changed (delta applied)");
        CHECK(d_on_vs_ref < 1e-4, "serving ON: executor output == base + adapter delta");
    }

    /* ---- DAG executor path: dag_execute applies the delta only when serving on ---- */
    {
        registry_set_state(&reg, "unit", PRIM_FUZZY);   /* record_fault left it RESET */
        for (int i = 0; i < in; i++) x[i] = rd();
        DagSource src; src.type = P(PORT_RAW, in, 1); src.values = x;
        DagPlan plan; memset(&plan, 0, sizeof plan);
        int planned = dag_plan(&reg, &src, 1, P(PORT_RAW, out, 1), &plan);
        CHECK(planned == 0, "dag_plan built a single-primitive DAG over the unit");

        const double *bp = btn_forward(&base, x);
        double base_only[8]; for (int o = 0; o < out; o++) base_only[o] = bp[o];
        double ref_on[8];
        registry_forward_with_lora(&reg, "unit", x, ref_on);   /* base + delta */

        double out_off[8], out_on[8];
        registry_lora_disable_serving(&reg);
        int rc_off = dag_execute(&plan, &src, 1, out_off, out);
        registry_lora_enable_serving(&reg);
        int rc_on = dag_execute(&plan, &src, 1, out_on, out);
        registry_lora_disable_serving(&reg);
        dag_free(&plan);

        CHECK(rc_off == 0 && rc_on == 0, "dag_execute runs with serving off and on");
        double d_off = maxabsdiff(out_off, base_only, out);
        double d_on_off = maxabsdiff(out_on, out_off, out);
        double d_on_ref = maxabsdiff(out_on, ref_on, out);
        printf("   (DAG: off-vs-base=%.2e  on-vs-off=%.3e  on-vs-(base+delta)=%.2e)\n",
               d_off, d_on_off, d_on_ref);
        CHECK(d_off == 0.0, "DAG serving OFF: output is the exact frozen base");
        CHECK(d_on_off > 1e-6, "DAG serving ON: output changed (delta applied)");
        CHECK(d_on_ref < 1e-4, "DAG serving ON: output == base + adapter delta");
    }

    registry_lora_detach(&reg, "unit");
    CHECK(!registry_has_lora(&reg, "unit"), "detach removes the adapter");
    for (int t = 0; t < m; t++) {
        for (int i = 0; i < in; i++) x[i] = rd();
        const double *b = btn_forward(&base, x);
        double basecpy[8]; for (int o = 0; o < out; o++) basecpy[o] = b[o];
        double served[8];
        registry_forward_with_lora(&reg, "unit", x, served);
        for (int o = 0; o < out; o++) err_detach += fabs(served[o] - basecpy[o]);
    }
    CHECK(err_detach == 0.0, "after detach, serving is the exact frozen base (zero overhead)");

    registry_free(&reg);
    btn_free(&base);
    printf("\n%s (%d failures)\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED", g_fail);
    return g_fail ? 1 : 0;
}
