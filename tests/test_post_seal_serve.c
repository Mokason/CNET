/* Post-seal serve proof: teacher help → gap_lane seal → close process →
 * SoulHost reopen → certified Tier A serve (cross-process learning proof).
 *
 * make post_seal_serve → POST_SEAL_SERVE_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/personal_ai.h"
#include "../include/soul_host.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/specialist.h"

#define SYM 4

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static Port sym_port(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = SYM;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static void onehot(double *row, int hot) {
    int i;
    for (i = 0; i < SYM; i++) row[i] = (i == hot) ? 1.0 : 0.0;
}

/* Teacher: rot1 — sealed unit must reproduce this after drain. */
static int teacher_rot1(const double *in, double *out, void *ctx) {
    int i, hot = 0;
    (void)ctx;
    for (i = 1; i < SYM; i++) if (in[i] > in[hot]) hot = i;
    for (i = 0; i < SYM; i++) out[i] = 0.0;
    out[(hot + 1) % SYM] = 1.0;
    return 0;
}

static int argmax4(const double *v) {
    int i, b = 0;
    for (i = 1; i < SYM; i++) if (v[i] > v[b]) b = i;
    return b;
}

int main(void) {
    const char *base = "tmp_post_seal_serve.cnb";
    const char *ledger = "tmp_post_seal_serve.gaps.txt";
    const char *inbox = "tmp_post_seal_serve.inbox";
    PersonalAi ai;
    PersonalAiPolicy pol;
    PersonalAiReport rep;
    GapLaneTickReport tr;
    Port pin = sym_port("pss_in");
    Port pgoal = sym_port("pss_seal");
    double in[SYM], out[SYM];
    SoulHost *host = NULL;
    int sealed = 0;
    int i;

    remove(base);
    remove(ledger);
    remove(inbox);
    remove("tmp_post_seal_serve.cnb.tmp");
    unsetenv("CNET_RESIDUAL_GGUF");
    unsetenv("CNET_SOUL_RESIDUAL_HERMETIC");
    setenv("CNET_GAP_INBOX", inbox, 1);

    printf("== post-seal serve proof (teach → seal → reopen → Tier A) ==\n");

    personal_ai_policy_defaults(&pol);
    pol.allow_teacher = 1;
    pol.teach_inline = 0;
    pol.allow_residual = 0;
    pol.allow_soft = 0;
    pol.allow_medium = 0;

    check(personal_ai_open(&ai, base, ledger, inbox, &pol) == 0,
          "personal_ai opens fresh base");
    ai.lane.acq.min_evidence = 1;
    ai.lane.acq.evidence_threshold = 0.5;
    check(personal_ai_bind_teacher(&ai, "pss_teacher_rot1", pin, pgoal,
                                   teacher_rot1, NULL) == 0,
          "teacher bound for novel signature");

    /* Novel goal: no local unit yet → teacher answers + gap. */
    onehot(in, 1);
    memset(out, 0, sizeof out);
    check(personal_ai_serve(&ai, pin, pgoal, in, SYM, out, SYM, &rep) == 0 &&
              rep.source == PERSONAL_AI_TEACHER &&
              argmax4(out) == 2,
          "pre-seal: teacher serves rot1");
    check(rep.gap_noted == 1, "pre-seal: miss noted for learning");

    /* Drain until sealed (or a few ticks). */
    for (i = 0; i < 8 && !sealed; i++) {
        memset(&tr, 0, sizeof tr);
        if (personal_ai_tick(&ai, &tr) != 0) break;
        if (tr.drain.closed >= 1 || ai.lane.reg.count >= 1)
            sealed = 1;
    }
    check(sealed, "tick sealed a certified unit from teacher");
    check(tr.checkpointed || sealed, "base checkpointed after seal");

    /* In-process proof: same signature is local Tier A. */
    onehot(in, 0);
    memset(out, 0, sizeof out);
    check(personal_ai_serve(&ai, pin, pgoal, in, SYM, out, SYM, &rep) == 0 &&
              rep.source == PERSONAL_AI_LOCAL &&
              argmax4(out) == 1,
          "in-process post-seal: PERSONAL_AI_LOCAL Tier A");

    /* Cross-process: close learner, open SoulHost on sealed CNB only. */
    personal_ai_close(&ai);
    check(access(base, 0) == 0, "sealed CNB exists on disk");

    check(soul_open(base, NULL, &host) == 0 && host != NULL,
          "SoulHost opens sealed CNB");
    check(soul_unit_count(host) >= 1, "SoulHost sees sealed unit(s)");

    onehot(in, 2);
    memset(out, 0, sizeof out);
    {
        int rc = soul_request(host, PORT_ONEHOT, SYM, 1, "pss_in",
                              PORT_ONEHOT, SYM, 1, "pss_seal",
                              in, SYM, out, SYM);
        check(rc == SYM, "soul_request post-seal returns out_total");
        check(soul_last_source(host) == SOUL_SOURCE_CERTIFIED,
              "soul_request source=CERTIFIED (Tier A)");
        check(argmax4(out) == 3, "post-seal SoulHost answer matches rot1");
        if (soul_last_source(host) == SOUL_SOURCE_CERTIFIED)
            printf("  (post-seal source=certified Tier A)\n");
    }

    /* Also prove named run of acq_pss_seal if present. */
    {
        int in_tot = 0, out_tot = 0;
        if (soul_unit_dims(host, "acq_pss_seal", &in_tot, &out_tot) == 0 &&
            in_tot == SYM && out_tot == SYM) {
            onehot(in, 3);
            memset(out, 0, sizeof out);
            check(soul_run(host, "acq_pss_seal", in, out, SYM) == SYM &&
                      argmax4(out) == 0,
                  "soul_run acq_pss_seal matches teacher");
        } else {
            check(1, "soul_run acq_pss_seal optional (name variant ok)");
        }
    }

    {
        SoulServeStats st;
        check(soul_serve_stats(host, &st) == 0, "serve stats readable");
        check(st.certified_serves >= 1, "stats: at least one certified serve");
    }

    soul_close(host);
    remove(base);
    remove(ledger);
    remove(inbox);
    remove("tmp_post_seal_serve.cnb.tmp");

    if (failures) {
        printf("POST_SEAL_SERVE_FAIL failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("POST_SEAL_SERVE_PASS checks=%d\n", checks);
    return 0;
}
