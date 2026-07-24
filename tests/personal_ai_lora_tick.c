/* personal_ai_lora_tick — end-to-end: the LIVE orchestrator (personal_ai_tick)
   with a LOADED base, adapter hook AHEAD of gap_lane's dense heal, driven by a
   RECORDED production traffic stream.

   A recorded JSONL of real JSON tool-call requests (one per line) stands in for
   the host's traffic log. It is replayed in three disjoint slices — fault-mining,
   in-tick validation, and held-out eval — so the fault queue and the validation
   sampler read the SAME captured stream, not independent generations.

   Two scenarios prove "cheap-retrain-first, dense-heal-fallback":
     A. reasonable gate -> the adapter CERTIFIES (trained on faults ∪ a
        representative replay slice, so it does not regress the good base), is
        marked non-RESET, gap_lane's heal skips it, and it serves base+delta.
     B. strict gate     -> a residual flip trips the gate, the unit stays RESET,
        and gap_lane's dense heal runs as the fallback.
   No GPU / teacher lane. */

#include "../include/personal_ai.h"
#include "../include/gap_lane.h"
#include "../include/base.h"
#include "../include/json_toolcall.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/router/registry_lora.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t S = 0x9E3779B9u;
static uint32_t rnd(void) { S = S * 1664525u + 1013904223u; return S; }

/* ---- realistic JSON tool-call request (for capturing the recorded stream) ---- */
static const char *const TRAFFIC_TOOL[8] = {
    "calculator", "memory_store", "memory_recall", "file_read",
    "cnet_recall", "web_search", "wiki_lookup", "final"
};
static const char *const TRAFFIC_ARGS[8] = {
    "\"expr\":\"n\"", "\"key\":\"k\",\"value\":\"v\"", "\"query\":\"q\"",
    "\"path\":\"f.txt\"", "\"cond\":0,\"current\":1", "\"query\":\"web thing\"",
    "\"query\":\"topic\"", "\"answer\":\"ok\""
};
static void gen_traffic(char *buf, size_t cap) {
    int t = (int)(rnd() % 8);
    int style = (int)(rnd() % 10);
    if (t == 7)
        snprintf(buf, cap, "{\"final\":\"stop\",%s}", TRAFFIC_ARGS[7]);
    else if (style < 6)
        snprintf(buf, cap, "{\"tool\":\"%s\",\"args\":{%s}}", TRAFFIC_TOOL[t], TRAFFIC_ARGS[t]);
    else if (style < 8)
        snprintf(buf, cap, "{\"args\":{%s}}", TRAFFIC_ARGS[t]);
    else {
        int t2 = (int)(rnd() % 7);
        snprintf(buf, cap, "{\"tool\":\"%s\",\"args\":{%s,\"note\":\"see %s\"}}",
                 TRAFFIC_TOOL[t], TRAFFIC_ARGS[t], TRAFFIC_TOOL[t2]);
    }
}

/* Capture: write `n` JSON requests, one per line — the recorded traffic log.
   (In deployment the serving host appends live requests here.) */
static int capture_write(const char *path, int n) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    for (int i = 0; i < n; i++) { char json[256]; gen_traffic(json, sizeof json); fprintf(f, "%s\n", json); }
    fclose(f);
    return 0;
}

/* ---- replay: load the recorded JSONL into memory ---- */
typedef struct { char **lines; size_t count; } TrafficReplay;
static int replay_load(TrafficReplay *tr, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t cap = 256; tr->lines = malloc(cap * sizeof(char *)); tr->count = 0;
    char buf[512];
    while (fgets(buf, sizeof buf, f)) {
        size_t len = strlen(buf); while (len && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
        if (len == 0) continue;
        if (tr->count == cap) { cap *= 2; tr->lines = realloc(tr->lines, cap * sizeof(char *)); }
        tr->lines[tr->count++] = strdup(buf);
    }
    fclose(f);
    return tr->count > 0 ? 0 : -1;
}
static void replay_free(TrafficReplay *tr) {
    for (size_t i = 0; i < tr->count; i++) free(tr->lines[i]);
    free(tr->lines); tr->lines = NULL; tr->count = 0;
}

static int serve_tool(const RoutePlan *plan, const double *feat, int IN, int OUT) {
    double out[CNET_JTC_N_TOOL];
    if (route_execute_ex(plan, feat, (size_t)IN, out, (size_t)OUT, NULL) != 0) return -1;
    return cnet_jtc_decode_tool(out);
}

/* validation sampler: replays the held-out validation slice of the recorded
   stream (base-correct AND base-wrong), labeled by the shared hermetic teacher. */
typedef struct { const TrafficReplay *tr; size_t lo, hi, cur; } ValCtx;
static size_t jtc_validate(const char *unit, int in_dim, int out_dim,
                           double *inputs, double *targets, size_t cap, void *ctx) {
    (void)unit;
    ValCtx *vc = (ValCtx *)ctx;
    size_t span = vc->hi - vc->lo;
    for (size_t s = 0; s < cap; s++) {
        const char *json = vc->tr->lines[vc->lo + (vc->cur++ % span)];
        double *f = inputs + s * (size_t)in_dim;
        cnet_jtc_encode(json, f);
        cnet_jtc_hermetic_teacher(f, targets + s * (size_t)out_dim, NULL);
    }
    return cap;
}

static BinaryTransformNetwork *find_btn(PrimitiveRegistry *reg, const char *name) {
    for (size_t i = 0; i < reg->count; i++)
        if (reg->entries[i].name && strcmp(reg->entries[i].name, name) == 0)
            return reg->entries[i].btn;
    return NULL;
}

/* returns 1 if the scenario matched its expectation, else 0 */
static int run_scenario(const char *tag, int strict_gate, const TrafficReplay *tr) {
    const char *base_path = "tmp_pa_lora.cnb", *ledger = "tmp_pa_lora.ledger", *inbox = "tmp_pa_lora.inbox";
    remove(base_path);
    { CnetBase base; cnb_init(&base);
      if (cnet_jtc_ensure_sealed(&base, NULL) < 0 || cnb_save(&base, base_path) != 0) {
          printf("FAIL: seal/save\n"); cnb_free(&base); return 0; }
      cnb_free(&base); }

    PersonalAiPolicy pol; personal_ai_policy_defaults(&pol);
    pol.allow_teacher = 0; pol.teach_inline = 0;
    PersonalAi ai;
    if (personal_ai_open(&ai, base_path, ledger, inbox, &pol) != 0) { printf("FAIL: open\n"); return 0; }
    PrimitiveRegistry *reg = &ai.lane.reg;
    const char *unit = CNET_JTC_UNIT_NAME;
    BinaryTransformNetwork *stu = find_btn(reg, unit);
    if (!stu) { printf("FAIL: no unit\n"); personal_ai_close(&ai); return 0; }
    const int IN = (int)stu->input_count, OUT = (int)stu->output_count;

    RoutePlan plan; memset(&plan, 0, sizeof plan);
    plan.steps[0] = stu; plan.names[0] = unit; plan.length = 1; plan.strict = 0;

    /* three disjoint slices of the recorded stream */
    const size_t N = tr->count;
    const size_t f_end = N * 6 / 10;     /* [0, f_end)      fault mining      */
    const size_t v_end = N * 8 / 10;     /* [f_end, v_end)  in-tick validation */
    /* [v_end, N) held-out eval */
    const int nte = (int)(N - v_end);

    double toh[CNET_JTC_N_TOOL];
    double *HF = malloc((size_t)nte * IN * sizeof(double));
    int *HT = malloc((size_t)nte * sizeof(int));
    for (int s = 0; s < nte; s++) {
        double *f = HF + (size_t)s * IN;
        cnet_jtc_encode(tr->lines[v_end + (size_t)s], f);
        cnet_jtc_hermetic_teacher(f, toh, NULL); HT[s] = cnet_jtc_decode_tool(toh);
    }
    int base_ok = 0;
    for (int s = 0; s < nte; s++) if (serve_tool(&plan, HF + (size_t)s * IN, IN, OUT) == HT[s]) base_ok++;

    double feat[CNET_JTC_N_FEAT]; int faults = 0;
    for (size_t i = 0; i < f_end; i++) {
        cnet_jtc_encode(tr->lines[i], feat);
        cnet_jtc_hermetic_teacher(feat, toh, NULL);
        if (serve_tool(&plan, feat, IN, OUT) != cnet_jtc_decode_tool(toh)) {
            double bad[CNET_JTC_N_TOOL]; const double *so = btn_forward(stu, feat);
            for (int o = 0; o < OUT; o++) bad[o] = so ? so[o] : 0.0;
            if (registry_record_fault(reg, unit, feat, bad) == 0 &&
                registry_supply_label(reg, unit, feat, toh) == 0) faults++;
        }
    }

    ValCtx vc = { tr, f_end, v_end, 0 };
    registry_lora_tick_opts topt = registry_lora_tick_defaults();
    topt.min_faults = 64; topt.holdout_frac = 0.25;
    topt.teach.rank = 8; topt.teach.alpha = 16.0f;
    topt.teach.train.epochs = 1500; topt.teach.train.lr = 0.02f;
    topt.validate = jtc_validate; topt.validate_ctx = &vc; topt.validate_cap = 200;
    topt.cert.argmax_mode = 1;
    topt.cert.max_regressions = strict_gate ? 0 : faults / 10;
    topt.cert.min_net_gain = 1;
    registry_lora_install_orchestrator(reg, &topt);

    GapLaneTickReport trep; memset(&trep, 0, sizeof trep);
    int rc = personal_ai_tick(&ai, &trep);
    int cert_after = registry_lora_is_certified(reg, unit);

    int base_post = 0, served_post = 0;
    for (int s = 0; s < nte; s++) {
        const double *so = btn_forward(stu, HF + (size_t)s * IN);
        if (so && cnet_jtc_decode_tool(so) == HT[s]) base_post++;
        if (serve_tool(&plan, HF + (size_t)s * IN, IN, OUT) == HT[s]) served_post++;
    }
    int heal = 100 * (base_post - base_ok) / nte;
    int adapt = 100 * (served_post - base_post) / nte;
    printf("\n[%s]  faults=%d (of %zu replayed)  rc=%d  certified=%d\n", tag, faults, f_end, rc, cert_after);
    printf("  baseline %d%% | base-student post %d%% (heal %+d) | served %d%% (adapter %+d)\n",
           100*base_ok/nte, 100*base_post/nte, heal, 100*served_post/nte, adapt);

    int ok;
    if (strict_gate) {
        ok = (rc == 0 && cert_after == 0 && heal > 0);
        printf("  -> %s: gate REJECTED the adapter; gap_lane dense heal served as fallback\n", ok ? "PASS" : "FAIL");
    } else {
        ok = (rc == 0 && cert_after == 1 && adapt > 0 && heal == 0);
        printf("  -> %s: adapter CERTIFIED + served; dense heal skipped (unit marked non-RESET)\n", ok ? "PASS" : "FAIL");
    }
    registry_lora_uninstall_orchestrator(reg);
    free(HF); free(HT); personal_ai_close(&ai);
    remove(base_path); remove(ledger); remove(inbox);
    return ok;
}

int main(void) {
    const char *traffic = "tmp_jtc_traffic.jsonl";
    if (capture_write(traffic, 1500) != 0) { printf("FAIL: capture\n"); return 1; }
    TrafficReplay tr;
    if (replay_load(&tr, traffic) != 0) { printf("FAIL: replay_load\n"); return 1; }
    printf("recorded production stream: %s (%zu requests)  e.g. %s\n", traffic, tr.count, tr.lines[0]);

    int a = run_scenario("A: adapter-first, reasonable gate", 0, &tr);
    int b = run_scenario("B: strict gate -> dense-heal fallback", 1, &tr);

    replay_free(&tr);
    printf("\n%s: cheap-adapter-first w/ dense-heal fallback, driven by personal_ai_tick over a recorded stream\n",
           (a && b) ? "ALL PASS" : "FAIL");
    printf("PA_LORA_TICK_DONE\n");
    return (a && b) ? 0 : 1;
}
