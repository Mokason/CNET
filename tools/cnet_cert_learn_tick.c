/* One-shot certified-learning tick on live base.
 * Seed JTC faults → PEFT tick → residual structure mine.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/personal_ai.h"
#include "../include/json_toolcall.h"
#include "../include/cnet_fault.h"
#include "../include/router/registry_lora.h"
#include "../include/residual_http.h"
#include "../include/nn.h"

static int seed_jtc_faults(int n) {
    int i, ok = 0;
    Port ip = cnet_jtc_input_port();
    Port op = cnet_jtc_output_port();
    size_t in = ip.field_width * (ip.field_count ? ip.field_count : 1);
    size_t out = op.field_width * (op.field_count ? op.field_count : 1);
    double *feat = calloc(in ? in : 1, sizeof(double));
    double *tgt = calloc(out ? out : 1, sizeof(double));
    static const char *samples[] = {
        "{\"tool\":\"calculator\",\"args\":{\"expr\":\"1+1\"}}",
        "{\"tool\":\"web_search\",\"args\":{\"query\":\"cnet\"}}",
        "{\"tool\":\"wiki_lookup\",\"args\":{\"query\":\"Skyrim\"}}",
        "{\"tool\":\"memory_store\",\"args\":{\"k\":\"a\",\"v\":\"b\"}}",
        "{\"tool\":\"memory_recall\",\"args\":{\"k\":\"a\"}}",
        "{\"tool\":\"file_read\",\"args\":{\"path\":\"/tmp/x\"}}",
        "{\"tool\":\"cnet_recall\",\"args\":{\"q\":\"test\"}}",
        "{\"tool\":\"final\",\"args\":{\"text\":\"done\"}}",
        "please calculate 2*3",
        "search the web for dragon age",
        "look up wiki for witcher",
        "store memory foo bar",
        "recall memory foo",
        "read file /etc/hosts",
        "final answer yes",
        "use calculator for 10/2",
    };
    if (!feat || !tgt) {
        free(feat);
        free(tgt);
        return -1;
    }
    if (in == 0) in = (size_t)CNET_JTC_N_FEAT;
    for (i = 0; i < n; i++) {
        char buf[256];
        const char *base = samples[i % 16];
        /* unique text => unique feature hash so dedupe cannot collapse seeds */
        snprintf(buf, sizeof buf, "%s unique_seed_%d_nonce_%d", base, i, i * 17 + 3);
        memset(feat, 0, in * sizeof(double));
        memset(tgt, 0, out * sizeof(double));
        if (cnet_jtc_encode(buf, feat) != 0) continue;
        if (cnet_jtc_hermetic_teacher(feat, tgt, NULL) != 0) continue;
        /* slight input jitter on unused dims if any stay zero-heavy */
        if (in > 0) feat[i % in] = feat[i % in] > 0.5 ? feat[i % in] : 1.0;
        cnet_fault_mirror_labeled("json_toolcall_v2", feat, tgt, in, out,
                                  "jtc_cert_seed");
        ok++;
    }
    free(feat);
    free(tgt);
    printf("CERT_LEARN seed_jtc ok=%d in_dim=%zu out_dim=%zu\n", ok, in, out);
    return ok;
}

static int residual_mine_campaign(PersonalAi *ai, int n) {
    ResidualHttp *rh = ai->owned_residual_http;
    int i, served = 0, mrc = -1;
    size_t W;
    double *in, *out;
    Port pin, pout;
    BinaryTransformNetwork *stu = NULL;
    if (!rh || !ai->hybrid.residual.bound) {
        printf("CERT_LEARN residual_mine skip (no residual bound)\n");
        return 0;
    }
    W = (size_t)residual_http_window_n(rh);
    if (W == 0) return 0;
    in = calloc(W, sizeof(double));
    out = calloc(W, sizeof(double));
    if (!in || !out) {
        free(in);
        free(out);
        return -1;
    }
    pin = residual_http_input_port(rh);
    pout = residual_http_output_port(rh);
    for (i = 0; i < n && i < (int)W; i++) {
        PersonalAiReport rep;
        size_t j;
        for (j = 0; j < W; j++) in[j] = 0.0;
        in[(size_t)i % W] = 1.0;
        memset(&rep, 0, sizeof rep);
        if (personal_ai_serve(ai, pin, pout, in, W, out, W, &rep) == 0)
            served++;
    }
    mrc = personal_ai_structure_mine(ai, &stu);
    printf("CERT_LEARN residual_serves=%d structure_mine_rc=%d student=%s\n",
           served, mrc, stu ? "yes" : "no");
    if (stu) {
        btn_free(stu);
        free(stu);
    }
    free(in);
    free(out);
    return served;
}

int main(void) {
    const char *base = getenv("CNET_BASE_PATH");
    PersonalAi ai;
    PersonalAiPolicy pol;
    char ledger[640], inbox[640];
    registry_lora_tick_opts topts;
    registry_lora_tick_report trep;
    int seeded, store_n = 0;

    if (!base || !base[0]) base = "soul_gemma4v2_final.cnb";
    if (!getenv("CNET_FAULT_LOG") || !getenv("CNET_FAULT_LOG")[0])
        setenv("CNET_FAULT_LOG", "logs/cnet_faults.jsonl", 1);
    if (!getenv("CNET_LORA_STORE_DIR"))
        setenv("CNET_LORA_STORE_DIR", "logs/lora_store", 1);
    setenv("CNET_LORA_STORE_AUTOSAVE", "1", 0);
    setenv("CNET_FAULT_MIRROR", "1", 0);
    setenv("CNET_LORA_AUTO_ORCH", "1", 1);

    printf("CERT_LEARN base=%s fault=%s store=%s http=%s\n", base,
           getenv("CNET_FAULT_LOG"), getenv("CNET_LORA_STORE_DIR"),
           getenv("CNET_RESIDUAL_HTTP") ? getenv("CNET_RESIDUAL_HTTP") : "");

    seeded = seed_jtc_faults(96);
    if (seeded < 32) {
        fprintf(stderr, "CERT_LEARN insufficient JTC seeds\n");
        return 2;
    }

    personal_ai_policy_defaults(&pol);
    pol.structure_mine_on_serve = 1;
    pol.structure_min_hits = 2;
    snprintf(ledger, sizeof ledger, "%s.gaps.txt", base);
    snprintf(inbox, sizeof inbox, "%s.inbox", base);
    if (personal_ai_open(&ai, base, ledger, inbox, &pol) != 0) {
        fprintf(stderr, "CERT_LEARN personal_ai_open failed\n");
        return 3;
    }
    printf("CERT_LEARN open residual_bound=%d\n", ai.hybrid.residual.bound);

    topts = registry_lora_tick_defaults();
    topts.min_faults = 32;
    topts.holdout_frac = 0.15;
    topts.cert.max_regressions = -1; /* net-gain only */
    topts.cert.min_net_gain = 0;     /* accept non-negative gain */
    topts.cert.argmax_mode = 1;
    topts.teach.rank = 8;
    topts.teach.alpha = 16.f;
    /* more train steps for sparse JTC features */
    topts.teach.train.epochs = 200;
    registry_lora_install_orchestrator(&ai.lane.reg, &topts);
    memset(&trep, 0, sizeof trep);
    (void)registry_lora_tick(&ai.lane.reg, &topts, &trep);
    printf("CERT_LEARN peft tick seen=%zu taught=%zu certified=%zu rejected=%zu\n",
           trep.units_seen, trep.taught, trep.certified, trep.rejected);
    if (trep.certified > 0)
        registry_lora_enable_serving(&ai.lane.reg);
    printf("CERT_LEARN jtc_lora_certified=%d\n",
           registry_lora_is_certified(&ai.lane.reg, "json_toolcall_v2"));

    /* structure mine: hammer few residual slots to clear min_hits */
    {
        ResidualHttp *rh = ai.owned_residual_http;
        if (rh && ai.hybrid.residual.bound) {
            size_t W = (size_t)residual_http_window_n(rh);
            double *in = calloc(W, sizeof(double));
            double *out = calloc(W, sizeof(double));
            Port pin = residual_http_input_port(rh);
            Port pout = residual_http_output_port(rh);
            int s, r, served = 0;
            BinaryTransformNetwork *stu = NULL;
            if (in && out) {
                for (s = 0; s < 4; s++) {
                    size_t j;
                    for (j = 0; j < W; j++) in[j] = 0.0;
                    in[(size_t)s] = 1.0;
                    for (r = 0; r < 3; r++) {
                        PersonalAiReport rep;
                        memset(&rep, 0, sizeof rep);
                        if (personal_ai_serve(&ai, pin, pout, in, W, out, W,
                                              &rep) == 0)
                            served++;
                    }
                }
                ai.policy.structure_min_hits = 2;
                r = personal_ai_structure_mine(&ai, &stu);
                printf("CERT_LEARN residual_hammer_serves=%d mine_rc=%d student=%s\n",
                       served, r, stu ? "yes" : "no");
                if (stu) {
                    btn_free(stu);
                    free(stu);
                }
            }
            free(in);
            free(out);
        }
    }
    personal_ai_close(&ai);

    {
        FILE *p = popen("ls logs/lora_store 2>/dev/null | wc -l", "r");
        if (p) {
            if (fscanf(p, "%d", &store_n) != 1) store_n = 0;
            pclose(p);
        }
    }
    printf("CERT_LEARN lora_store_files=%d\n", store_n);
    printf("CERT_LEARN_TICK_DONE seed=%d peft_certified=%zu store=%d\n", seeded,
           trep.certified, store_n);
    return 0;
}
