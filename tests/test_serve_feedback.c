/* P1 Serve feedback loop — hermetic.
 * Teach → seal → SoulHost serve N times → reliability leaves 0.5 prior,
 * certified_serves increases, optional CNET_ROUTE_LOG gets JSONL lines.
 * make serve_feedback → SERVE_FEEDBACK_PASS
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/personal_ai.h"
#include "../include/soul_host.h"
#include "../include/nn.h"
#include "../include/router.h"

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

static int teacher_rot1(const double *in, double *out, void *ctx) {
    int i, hot = 0;
    (void)ctx;
    for (i = 1; i < SYM; i++) if (in[i] > in[hot]) hot = i;
    for (i = 0; i < SYM; i++) out[i] = 0.0;
    out[(hot + 1) % SYM] = 1.0;
    return 0;
}

static int count_lines(const char *path) {
    FILE *f = fopen(path, "r");
    int n = 0;
    int c;
    if (!f) return -1;
    while ((c = fgetc(f)) != EOF) if (c == '\n') n++;
    fclose(f);
    return n;
}

int main(void) {
    const char *base = "tmp_serve_feedback.cnb";
    const char *ledger = "tmp_serve_feedback.gaps.txt";
    const char *inbox = "tmp_serve_feedback.inbox";
    const char *rlog = "tmp_serve_feedback.route.jsonl";
    PersonalAi ai;
    PersonalAiPolicy pol;
    PersonalAiReport prep;
    GapLaneTickReport tr;
    Port pin = sym_port("sf_in");
    Port pgoal = sym_port("sf_goal");
    double in[SYM], out[SYM];
    SoulHost *host = NULL;
    SoulServeStats st0, st1;
    int sealed = 0, i, rel0 = -1, rel1 = -1;
    char uname[128];
    int unit_idx = 0;

    remove(base);
    remove(ledger);
    remove(inbox);
    remove(rlog);
    unsetenv("CNET_RESIDUAL_GGUF");
    setenv("CNET_GAP_INBOX", inbox, 1);
    setenv("CNET_ROUTE_LOG", rlog, 1);

    printf("== serve_feedback (P1) ==\n");

    personal_ai_policy_defaults(&pol);
    pol.allow_teacher = 1;
    pol.teach_inline = 0;
    pol.allow_residual = 0;
    pol.allow_soft = 0;
    pol.allow_medium = 0;

    check(personal_ai_open(&ai, base, ledger, inbox, &pol) == 0, "open personal_ai");
    ai.lane.acq.min_evidence = 1;
    ai.lane.acq.evidence_threshold = 0.5;
    check(personal_ai_bind_teacher(&ai, "sf_teacher", pin, pgoal, teacher_rot1, NULL) == 0,
          "bind teacher");

    onehot(in, 0);
    memset(out, 0, sizeof out);
    check(personal_ai_serve(&ai, pin, pgoal, in, SYM, out, SYM, &prep) == 0 &&
              prep.source == PERSONAL_AI_TEACHER,
          "pre-seal teacher serve");

    for (i = 0; i < 8 && !sealed; i++) {
        memset(&tr, 0, sizeof tr);
        if (personal_ai_tick(&ai, &tr) != 0) break;
        if (tr.drain.closed >= 1 || ai.lane.reg.count >= 1) sealed = 1;
    }
    check(sealed, "sealed unit from teacher");
    personal_ai_close(&ai);

    check(soul_open(base, NULL, &host) == 0 && host, "SoulHost open sealed base");
    check(soul_unit_count(host) >= 1, "unit count >= 1");
    check(soul_unit_name(host, unit_idx, uname, (int)sizeof uname) == 0, "unit name");

    rel0 = soul_unit_reliability_milli(host, uname);
    check(rel0 == 500, "fresh unit reliability is Laplace prior 0.500");
    check(soul_serve_stats(host, &st0) == 0, "stats before");
    check(st0.certified_serves == 0, "certified_serves starts at 0");

    for (i = 0; i < 16; i++) {
        onehot(in, i % SYM);
        memset(out, 0, sizeof out);
        check(soul_request(host, PORT_ONEHOT, SYM, 1, "sf_in",
                           PORT_ONEHOT, SYM, 1, "sf_goal",
                           in, SYM, out, SYM) == SYM,
              i == 0 ? "soul_request certified serve" : "soul_request repeat");
        if (soul_last_source(host) != SOUL_SOURCE_CERTIFIED) {
            check(0, "source remains CERTIFIED");
            break;
        }
    }

    check(soul_serve_stats(host, &st1) == 0, "stats after");
    check(st1.certified_serves >= 16, "certified_serves incremented by serves");
    rel1 = soul_unit_reliability_milli(host, uname);
    check(rel1 > 500, "reliability rose above prior after successful serves");
    {
        int nlines = count_lines(rlog);
        check(nlines >= 16, "CNET_ROUTE_LOG recorded one line per serve");
    }

    soul_close(host);
    unsetenv("CNET_ROUTE_LOG");
    unsetenv("CNET_GAP_INBOX");
    remove(base);
    remove(ledger);
    remove(inbox);
    remove(rlog);
    remove("tmp_serve_feedback.cnb.tmp");

    if (failures) {
        printf("SERVE_FEEDBACK_FAIL failures=%d checks=%d rel0=%d rel1=%d serves=%llu\n",
               failures, checks, rel0, rel1,
               (unsigned long long)st1.certified_serves);
        return 1;
    }
    printf("SERVE_FEEDBACK_PASS checks=%d rel0=%d rel1=%d certified_serves=%llu\n",
           checks, rel0, rel1, (unsigned long long)st1.certified_serves);
    return 0;
}
