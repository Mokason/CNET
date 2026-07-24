/* registry_lora — train and serve a rank-r cce_lora adapter from a real unit's
   labeled retrain queue. See include/router/registry_lora.h.

   The adapter is an additive low-rank correction on the unit's output:
       serve(x) = btn_forward(base)(x) + (alpha/rank)*(xA)B
   trained on the residual (teacher_target - base) already sitting in the unit's
   RetrainQueue.labeled_* set — the same port-validated pairs registry_heal uses.
   The base BTN is never modified. */

#include "../../include/router/registry_lora.h"
#include "../../include/nn.h"          /* btn_forward, BinaryTransformNetwork */
#include "../../include/cce/cce_tensor.h"
#include "../../include/cnet_fault.h"
#include "../../include/cnet_promote.h"
#include "../../include/router/registry_lora_store.h"
#include "../../include/cce/cce_adapter_bank.h"
#include "../../include/cnet_acct.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

registry_lora_opts registry_lora_defaults(void) {
    registry_lora_opts o;
    o.rank = 8;
    o.alpha = 16.0f;
    o.train = cce_lora_train_defaults();
    o.train.epochs = 600;
    o.train.lr = 0.02f;
    return o;
}

/* first strcmp match wins, mirroring registry_set_state */
static RegistryEntry *find_entry(PrimitiveRegistry *reg, const char *name) {
    if (!reg || !name) return NULL;
    for (size_t i = 0; i < reg->count; i++)
        if (reg->entries[i].name && strcmp(reg->entries[i].name, name) == 0)
            return &reg->entries[i];
    return NULL;
}

/* Train + attach a rank-r adapter to `e` from explicit (input, target) double
   pairs (residual = target - btn_forward(base)). Attaches as an uncertified
   candidate. Shared by registry_teach_lora (whole queue) and registry_lora_tick
   (a train split). */
static int teach_pairs(RegistryEntry *e, const double *inputs, const double *targets,
                       size_t n, const registry_lora_opts *o, registry_lora_stats *stats) {
    const int in = (int)e->btn->input_count, out = (int)e->btn->output_count;
    if (n == 0 || in <= 0 || out <= 0) return -1;

    float *X = malloc(n * (size_t)in * sizeof(float));
    float *R = malloc(n * (size_t)out * sizeof(float));
    if (!X || !R) { free(X); free(R); return -1; }

    double pre = 0.0;
    for (size_t s = 0; s < n; s++) {
        const double *xin = inputs + s * (size_t)in;
        const double *tgt = targets + s * (size_t)out;
        const double *base = btn_forward(e->btn, xin);
        if (!base) { free(X); free(R); return -1; }
        for (int i = 0; i < in; i++) X[s * in + i] = (float)xin[i];
        for (int oo = 0; oo < out; oo++) {
            double resid = tgt[oo] - base[oo];
            R[s * out + oo] = (float)resid;
            pre += resid * resid;
        }
    }
    pre /= (double)(n * (size_t)out);

    cce_lora *adp = malloc(sizeof(*adp));
    if (!adp) { free(X); free(R); return -1; }
    if (cce_lora_init(adp, in, out, o->rank, o->alpha, 0x51A17u + (uint32_t)o->rank) != CCE_OK) {
        free(adp); free(X); free(R); return -1;
    }
    /* VeRA-lite: freeze random A, train B only */
    if (getenv("CNET_LORA_VERA") && getenv("CNET_LORA_VERA")[0] == '1')
        cce_lora_set_train_A(adp, 0);
    double post = cce_lora_train(adp, X, R, n, &o->train);
    cnet_acct_add_peft_train();
    if (post < 0.0) { cce_lora_free(adp); free(adp); free(X); free(R); return -1; }

    if (e->lora) { cce_lora_free(e->lora); free(e->lora); }
    e->lora = adp;
    e->lora_certified = 0;

    if (stats) {
        stats->pairs = n; stats->pre_mse = pre; stats->post_mse = post;
        stats->in_dim = in; stats->out_dim = out; stats->rank = o->rank;
        stats->params = cce_lora_param_count(adp);
        stats->dense_params = cce_lora_dense_param_count(adp);
    }
    free(X); free(R);
    return 0;
}

int registry_teach_lora(PrimitiveRegistry *reg, const char *name,
                        const registry_lora_opts *opt, registry_lora_stats *stats) {
    RegistryEntry *e = find_entry(reg, name);
    if (!e || !e->btn || !e->queue) return -1;
    RetrainQueue *q = e->queue;
    registry_lora_opts o = opt ? *opt : registry_lora_defaults();
    return teach_pairs(e, q->labeled_inputs, q->labeled_targets, q->labeled_count, &o, stats);
}

/* registry_teach_lora_OLD removed (Tier3 cleanup) */

int registry_forward_with_lora(PrimitiveRegistry *reg, const char *name,
                               const double *input, double *out) {
    RegistryEntry *e = find_entry(reg, name);
    if (!e || !e->btn || !input || !out) return -1;
    const int outc = (int)e->btn->output_count;
    const int inc = (int)e->btn->input_count;
    const double *base = btn_forward(e->btn, input);
    if (!base) return -1;
    for (int o = 0; o < outc; o++) out[o] = base[o];
    if (!e->lora) return 0;                       /* base unchanged, zero overhead */

    /* out += delta via the float adapter */
    cce_tensor x, y; int xs[1] = { inc }, ys[1] = { outc };
    if (cce_tensor_alloc(&x, xs, 1) != CCE_OK) return -1;
    if (cce_tensor_alloc(&y, ys, 1) != CCE_OK) { cce_tensor_free(&x); return -1; }
    for (int i = 0; i < inc; i++) x.data[i] = (float)input[i];
    for (int o = 0; o < outc; o++) y.data[o] = 0.0f;
    cce_result rc = cce_lora_apply((const cce_lora *)e->lora, &x, &y);
    if (rc == CCE_OK)
        for (int o = 0; o < outc; o++) out[o] += (double)y.data[o];
    cce_tensor_free(&x); cce_tensor_free(&y);
    return rc == CCE_OK ? 0 : -1;
}

int registry_has_lora(const PrimitiveRegistry *reg, const char *name) {
    RegistryEntry *e = find_entry((PrimitiveRegistry *)reg, name);
    return (e && e->lora) ? 1 : 0;
}

void registry_lora_detach(PrimitiveRegistry *reg, const char *name) {
    RegistryEntry *e = find_entry(reg, name);
    if (e && e->lora) { cce_lora_free(e->lora); free(e->lora); e->lora = NULL; e->lora_certified = 0; }
}

/* ---- certify-before-serve ------------------------------------------------- */
registry_lora_cert_policy registry_lora_cert_defaults(void) {
    registry_lora_cert_policy p;
    p.argmax_mode = 1;       /* classifiers are the common case */
    p.max_regressions = 0;   /* set >0 to tolerate a few right->wrong flips */
    p.min_net_gain = 1;      /* must fix at least one net */
    return p;
}

static int argmax_d(const double *v, int n) {
    int mi = 0; for (int i = 1; i < n; i++) if (v[i] > v[mi]) mi = i; return mi;
}

int registry_certify_lora(PrimitiveRegistry *reg, const char *name,
                          const double *inputs, const double *targets, size_t n,
                          const registry_lora_cert_policy *policy,
                          registry_lora_cert_report *report) {
    RegistryEntry *e = find_entry(reg, name);
    if (!e || !e->btn || !e->lora || !inputs || !targets || n == 0) return -1;
    const cce_lora *lo = (const cce_lora *)e->lora;
    const int in = lo->in_dim, out = lo->out_dim;
    if (out > 64) return -1;   /* per-sample scratch cap (prototype) */
    registry_lora_cert_policy pol = policy ? *policy : registry_lora_cert_defaults();

    cce_tensor x, y; int xs[1] = { in }, ys[1] = { out };
    if (cce_tensor_alloc(&x, xs, 1) != CCE_OK) return -1;
    if (cce_tensor_alloc(&y, ys, 1) != CCE_OK) { cce_tensor_free(&x); return -1; }

    int base_correct = 0, adp_correct = 0, fixes = 0, regress = 0;
    double base_se = 0.0, adp_se = 0.0;
    for (size_t s = 0; s < n; s++) {
        const double *xin = inputs + s * (size_t)in;
        const double *tgt = targets + s * (size_t)out;
        const double *base = btn_forward(e->btn, xin);      /* borrowed buffer */
        if (!base) { cce_tensor_free(&x); cce_tensor_free(&y); return -1; }
        for (int j = 0; j < in; j++) x.data[j] = (float)xin[j];
        for (int o = 0; o < out; o++) y.data[o] = 0.0f;
        cce_lora_apply(lo, &x, &y);                          /* delta */

        double be = 0.0, ae = 0.0;
        double adapted[64]; double basecpy[64];
        for (int o = 0; o < out; o++) {
            basecpy[o] = base[o];
            adapted[o] = base[o] + (double)y.data[o];
            double db = base[o] - tgt[o], da = adapted[o] - tgt[o];
            be += db * db; ae += da * da;
        }
        base_se += be; adp_se += ae;

        if (pol.argmax_mode) {
            int tt = argmax_d(tgt, out);
            int bc = (argmax_d(basecpy, out) == tt);
            int ac = (argmax_d(adapted, out) == tt);
            base_correct += bc; adp_correct += ac;
            if (!bc && ac) fixes++;
            if (bc && !ac) regress++;
        } else {
            if (ae < be - 1e-12) fixes++;
            else if (ae > be + 1e-12) regress++;
        }
    }
    cce_tensor_free(&x); cce_tensor_free(&y);

    int passed = 1;
    if (pol.max_regressions >= 0 && regress > pol.max_regressions) passed = 0;
    if ((fixes - regress) < pol.min_net_gain) passed = 0;
    e->lora_certified = passed;

    if (report) {
        report->passed = passed;
        report->n = n;
        report->base_correct = base_correct;
        report->adapter_correct = adp_correct;
        report->fixes = fixes;
        report->regressions = regress;
        report->base_mse = base_se / (double)(n * (size_t)out);
        report->adapter_mse = adp_se / (double)(n * (size_t)out);
    }
    return passed;
}

int registry_lora_is_certified(const PrimitiveRegistry *reg, const char *name) {
    RegistryEntry *e = find_entry((PrimitiveRegistry *)reg, name);
    return (e && e->lora && e->lora_certified) ? 1 : 0;
}

/* ---- governed tick action (orchestrator) ---------------------------------- */
static cce_adapter_bank g_peft_bank;
static int g_peft_bank_inited = 0;
static void peft_bank_ensure(void) {
    if (!g_peft_bank_inited) {
        cce_adapter_bank_init(&g_peft_bank);
        g_peft_bank_inited = 1;
    }
}

registry_lora_tick_opts registry_lora_tick_defaults(void) {
    registry_lora_tick_opts o;
    o.min_faults = 64;         /* enough pairs for a meaningful train+holdout split */
    o.holdout_frac = 0.25;
    o.teach = registry_lora_defaults();
    o.cert = registry_lora_cert_defaults();
    o.cert.max_regressions = -1;   /* set by policy; -1 => rate-agnostic, rely on net gain */
    o.validate = NULL;
    o.validate_ctx = NULL;
    o.validate_cap = 256;
    return o;
}

int registry_lora_ingest_fault_bus(PrimitiveRegistry *reg, const char *path,
                                   const char *unit_filter) {
    const char *p;
    size_t total = 0;
    size_t i;
    if (!reg) return -1;
    p = path;
    if (!p || !p[0]) p = getenv("CNET_FAULT_LOG");
    if (!p || !p[0]) return 0;
    for (i = 0; i < reg->count; i++) {
        RegistryEntry *e = &reg->entries[i];
        int in, out;
        size_t cap = 4096, n, k;
        double *ins = NULL, *tgts = NULL;
        if (!e->btn || !e->name) continue;
        if (unit_filter && unit_filter[0] && strcmp(e->name, unit_filter) != 0)
            continue;
        in = (int)e->btn->input_count;
        out = (int)e->btn->output_count;
        if (in <= 0 || out <= 0) continue;
        ins = malloc(cap * (size_t)in * sizeof(double));
        tgts = malloc(cap * (size_t)out * sizeof(double));
        if (!ins || !tgts) {
            free(ins);
            free(tgts);
            return -1;
        }
        n = cnet_fault_load_vectors(p, e->name, in, out, ins, tgts, cap);
        for (k = 0; k < n; k++) {
            if (registry_add_labeled_pair(reg, e->name,
                                          ins + k * (size_t)in,
                                          tgts + k * (size_t)out) == 0)
                total++;
        }
        free(ins);
        free(tgts);
    }
    return (int)total;
}

int registry_lora_tick(PrimitiveRegistry *reg, const registry_lora_tick_opts *opt,
                       registry_lora_tick_report *report) {
    if (!reg) return -1;
    registry_lora_tick_opts o = opt ? *opt : registry_lora_tick_defaults();
    size_t seen = 0, taught = 0, certd = 0, rej = 0;
    /* Pull cross-process labeled pairs before teaching (Ghost/JTC/MCP bus). */
    if (getenv("CNET_FAULT_LOG") && getenv("CNET_FAULT_LOG")[0])
        (void)registry_lora_ingest_fault_bus(reg, NULL, NULL);
    for (size_t i = 0; i < reg->count; i++) {
        RegistryEntry *e = &reg->entries[i];
        if (!e->btn || !e->queue || !e->name) continue;
        RetrainQueue *q = e->queue;
        const size_t n = q->labeled_count;
        if (n < o.min_faults) continue;
        const int in = (int)q->input_count, out = (int)q->output_count;
        if (in <= 0 || out <= 0) continue;
        seen++;

        int pass = 0;
        if (o.validate) {
            size_t cap = o.validate_cap ? o.validate_cap : 256;
            /* Train on faults UNION a representative sample, so the adapter learns
               delta≈0 on already-correct cases (safe on a good base) while fixing
               the faults — mirrors registry_heal's "exemplars ∪ labeled faults".
               Certify on a fresh representative sample (regression-aware). */
            double *ai = malloc(cap * (size_t)in * sizeof(double));
            double *at = malloc(cap * (size_t)out * sizeof(double));
            size_t na = (ai && at) ? o.validate(e->name, in, out, ai, at, cap, o.validate_ctx) : 0;
            size_t ntot = n + na;
            double *ti = malloc(ntot * (size_t)in * sizeof(double));
            double *tt = malloc(ntot * (size_t)out * sizeof(double));
            int taught_ok = 0;
            if (ti && tt) {
                memcpy(ti, q->labeled_inputs, n * (size_t)in * sizeof(double));
                memcpy(tt, q->labeled_targets, n * (size_t)out * sizeof(double));
                if (na) {
                    memcpy(ti + n * (size_t)in, ai, na * (size_t)in * sizeof(double));
                    memcpy(tt + n * (size_t)out, at, na * (size_t)out * sizeof(double));
                }
                taught_ok = (teach_pairs(e, ti, tt, ntot, &o.teach, NULL) == 0);
            }
            free(ai); free(at); free(ti); free(tt);
            if (!taught_ok) continue;
            taught++;
            double *vi = malloc(cap * (size_t)in * sizeof(double));
            double *vt = malloc(cap * (size_t)out * sizeof(double));
            if (vi && vt) {
                size_t nv = o.validate(e->name, in, out, vi, vt, cap, o.validate_ctx);
                if (nv > 0)
                    pass = registry_certify_lora(reg, e->name, vi, vt, nv, &o.cert, NULL);
            }
            free(vi); free(vt);
        } else {
            /* Fault-queue holdout (fixes only, no regression signal). */
            size_t nh = (size_t)((double)n * o.holdout_frac);
            if (nh < 1) nh = 1;
            if (nh >= n) nh = n / 2;
            size_t nt = n - nh;
            if (nt < 1) { seen--; continue; }
            if (teach_pairs(e, q->labeled_inputs, q->labeled_targets, nt, &o.teach, NULL) != 0)
                continue;
            taught++;
            pass = registry_certify_lora(reg, e->name,
                       q->labeled_inputs + nt * (size_t)in,
                       q->labeled_targets + nt * (size_t)out, nh, &o.cert, NULL);
        }
        if (pass == 1) {
            /* Optional promote gate when CNET_PROMOTE=1 (optionally needs eval delta) */
            if (getenv("CNET_PROMOTE") && getenv("CNET_PROMOTE")[0] == '1') {
                CnetPromoteInput pin;
                CnetPromoteDecision dec;
                const char *edp;
                cnet_promote_defaults(&pin);
                pin.fixes = 1;
                pin.regressions = 0;
                pin.min_net_gain = o.cert.min_net_gain > 0 ? o.cert.min_net_gain : 1;
                edp = getenv("CNET_PROMOTE_EVAL_DELTA");
                if (edp && edp[0]) {
                    double dlt = 0;
                    if (cnet_promote_read_eval_delta(edp, &dlt) == 0) {
                        pin.require_eval_delta = 1;
                        pin.eval_delta = dlt;
                        pin.min_eval_delta = 0.0;
                    } else {
                        pass = 0; /* required file missing/unreadable */
                    }
                }
                if (pass == 1) {
                    dec = cnet_promote_decide(&pin);
                    if (!dec.allowed) pass = 0;
                }
            }
        }
        if (pass == 1) {
            certd++;
            /* handled by the adapter — clear PRIM_RESET so a dense-heal pass
               (specialist_health, gated on PRIM_RESET) skips this unit; dense
               heal remains the fallback for units left RESET below. */
            if (e->state == PRIM_RESET) registry_set_state(reg, e->name, PRIM_PROVISIONAL);
            /* Persist certified adapter when CNET_LORA_STORE_AUTOSAVE=1 */
            {
                const char *as = getenv("CNET_LORA_STORE_AUTOSAVE");
                const char *dir = registry_lora_store_dir_env();
                if (as && as[0] == '1' && dir)
                    (void)registry_lora_store_save(reg, e->name, dir);
            }
            /* Hard-routed multi-adapter bank (CNET_ADAPTER_BANK=1) */
            if (getenv("CNET_ADAPTER_BANK") && getenv("CNET_ADAPTER_BANK")[0] == '1' && e->lora) {
                peft_bank_ensure();
                (void)cce_adapter_bank_put(&g_peft_bank, e->name, e->lora, /*lora*/1, 1);
                (void)cce_adapter_bank_select(&g_peft_bank, e->name);
            }
            cnet_acct_add_adapter_pass();
        } else { registry_lora_detach(reg, e->name); rej++; cnet_acct_add_adapter_reject(); }
    }
    if (report) { report->units_seen = seen; report->taught = taught;
                  report->certified = certd; report->rejected = rej; }
    return 0;
}

/* ---- orchestrator install (arms the serve + tick hooks) ------------------- */
static registry_lora_tick_opts g_tick_opts;
static int g_tick_opts_set = 0;

static void lora_tick_impl(PrimitiveRegistry *reg) {
    registry_lora_tick(reg, g_tick_opts_set ? &g_tick_opts : NULL, NULL);
}

void registry_lora_install_orchestrator(PrimitiveRegistry *reg, const registry_lora_tick_opts *opt) {
    if (!reg) return;
    g_tick_opts = opt ? *opt : registry_lora_tick_defaults();
    g_tick_opts_set = 1;
    registry_lora_enable_serving(reg);   /* serve hook + g_lora_reg */
    g_cnet_lora_tick_hook = lora_tick_impl;
    /* Reload durable certified adapters if requested */
    {
        const char *al = getenv("CNET_LORA_STORE_AUTOLOAD");
        const char *dir = registry_lora_store_dir_env();
        if (al && al[0] == '1' && dir)
            (void)registry_lora_store_load_all(reg, dir, /*mark_certified=*/1);
    }
}

void registry_lora_uninstall_orchestrator(PrimitiveRegistry *reg) {
    g_cnet_lora_tick_hook = NULL;
    g_tick_opts_set = 0;
    registry_lora_disable_serving(reg);
}

/* ---- live-serving hook (executors call this after btn_forward) ------------- */
static const PrimitiveRegistry *g_lora_reg = NULL;

static void lora_serve_impl(const BinaryTransformNetwork *btn,
                            const double *input, double *raw, size_t out_len) {
    const PrimitiveRegistry *reg = g_lora_reg;
    if (!reg || !reg->lora_serving_enabled || !btn || !input || !raw) return;
    for (size_t i = 0; i < reg->count; i++) {
        const RegistryEntry *e = &reg->entries[i];
        if (e->btn != btn || !e->lora || !e->lora_certified) continue;  /* certify-before-serve */
        const cce_lora *lo = (const cce_lora *)e->lora;
        int inc = lo->in_dim, outc = lo->out_dim;
        if ((size_t)outc > out_len) outc = (int)out_len;   /* never overrun caller's buffer */
        cce_tensor x, y; int xs[1] = { inc }, ys[1] = { lo->out_dim };
        if (cce_tensor_alloc(&x, xs, 1) != CCE_OK) return;
        if (cce_tensor_alloc(&y, ys, 1) != CCE_OK) { cce_tensor_free(&x); return; }
        for (int j = 0; j < inc; j++) x.data[j] = (float)input[j];
        for (int o = 0; o < lo->out_dim; o++) y.data[o] = 0.0f;
        if (cce_lora_apply(lo, &x, &y) == CCE_OK)
            for (int o = 0; o < outc; o++) raw[o] += (double)y.data[o];
        cce_tensor_free(&x); cce_tensor_free(&y);
        return;                                            /* one entry per btn */
    }
}

void registry_lora_enable_serving(PrimitiveRegistry *reg) {
    if (!reg) return;
    g_lora_reg = reg;
    reg->lora_serving_enabled = 1;
    g_cnet_lora_serve_hook = lora_serve_impl;
}

void registry_lora_disable_serving(PrimitiveRegistry *reg) {
    if (reg) reg->lora_serving_enabled = 0;
    g_lora_reg = NULL;
    g_cnet_lora_serve_hook = NULL;
}
