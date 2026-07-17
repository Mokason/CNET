/* Personal AI gate: local library first, big-AI teacher on call, then local.
 * make personal_ai → PERSONAL_AI_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/personal_ai.h"
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

/* Big-AI stand-in: rot1 */
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

/* Train a rot2 unit for the LOCAL library. */
static int admit_local_rot2(PrimitiveRegistry *reg) {
    BinaryTransformNetwork *btn;
    double in[SYM][SYM], tg[SYM][SYM];
    Contract c;
    Specialist s;
    Port pin = sym_port("pai_in");
    Port pout = sym_port("pai_local");
    int i;

    btn = (BinaryTransformNetwork *)calloc(1, sizeof *btn);
    if (!btn) return -1;
    for (i = 0; i < SYM; i++) {
        onehot(in[i], i);
        onehot(tg[i], (i + 2) % SYM);
    }
    if (btn_init(btn, SYM, SYM, 8, 32, 0.5, 7) != 0) return -1;
    if (btn_set_ports(btn, pin, pout) != 0) return -1;
    (void)btn_train_dynamic(btn, (const double *)in, (const double *)tg, SYM,
                            20000, 200, 1e-5, 1e-7);
    (void)btn_train(btn, (const double *)in, (const double *)tg, SYM, 3000);
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, "pai_local_rot2", btn, (const double *)in,
                               (const double *)tg, SYM) != 0)
        return -1;
    memset(&s, 0, sizeof s);
    if (specialist_wrap_btn(&s, btn, "pai_local_rot2") != 0 ||
        specialist_admit(reg, &s, &c) != 0) {
        contract_free(&c);
        return -1;
    }
    contract_free(&c);
    /* registry borrows btn — leak intentionally for process lifetime of test */
    (void)btn;
    return 0;
}

int main(void) {
    const char *base = "tmp_personal_ai.cnb";
    const char *ledger = "tmp_personal_ai.gaps.txt";
    const char *inbox = "tmp_personal_ai.inbox";
    PersonalAi ai;
    PersonalAiPolicy pol;
    PersonalAiReport rep;
    Port pin = sym_port("pai_in");
    Port plocal = sym_port("pai_local");
    Port phelp = sym_port("pai_help");
    double in[SYM], out[SYM];
    int i;

    remove(base);
    remove(ledger);
    remove(inbox);

    printf("== personal AI: local first, teacher on call ==\n");

    personal_ai_policy_defaults(&pol);
    pol.allow_teacher = 1;
    /* Default product path: teacher helps now; gap_lane tick teaches after.
       (Inline teach is optional and can hit min_evidence on tiny domains.) */
    pol.teach_inline = 0;
    pol.max_inline_teaches = 0;

    check(personal_ai_open(&ai, base, ledger, inbox, &pol) == 0,
          "personal_ai opens");
    /* SYM=4 domain: min_evidence must not exceed usable exemplars. */
    ai.lane.acq.min_evidence = 1;
    ai.lane.acq.evidence_threshold = 0.5;
    check(admit_local_rot2(&ai.lane.reg) == 0, "local rot2 unit admitted");
    check(personal_ai_bind_teacher(&ai, "big_ai_rot1", pin, phelp,
                                   teacher_rot1, NULL) == 0,
          "big-AI teacher bound");

    /* Local hit: no teacher needed. */
    onehot(in, 1);
    memset(out, 0, sizeof out);
    check(personal_ai_serve(&ai, pin, plocal, in, SYM, out, SYM, &rep) == 0 &&
              rep.source == PERSONAL_AI_LOCAL &&
              argmax4(out) == 3,
          "local library serves rot2 without teacher");

    /* Novel goal: teacher helps (rot1). */
    onehot(in, 1);
    memset(out, 0, sizeof out);
    check(personal_ai_serve(&ai, pin, phelp, in, SYM, out, SYM, &rep) == 0 &&
              rep.source == PERSONAL_AI_TEACHER &&
              argmax4(out) == 2,
          "no local plan → teacher on call answers");
    check(rep.gap_noted == 1, "miss noted for learning");

    /* Teach may be async (tick) or inline. Prefer tick path: always works
       under min_evidence once the gap is open and teacher is bound. */
    if (!rep.taught) {
        GapLaneTickReport tr;
        memset(&tr, 0, sizeof tr);
        check(personal_ai_tick(&ai, &tr) == 0, "tick drains open gaps");
        check(tr.drain.closed >= 1 || ai.lane.reg.count >= 2,
              "tick sealed a unit from teacher help");
    } else {
        check(rep.taught == 1, "inline teach sealed a local skill from help");
    }

    /* Same novel goal again: should now be LOCAL if teach sealed. */
    onehot(in, 2);
    memset(out, 0, sizeof out);
    check(personal_ai_serve(&ai, pin, phelp, in, SYM, out, SYM, &rep) == 0 &&
              rep.source == PERSONAL_AI_LOCAL &&
              argmax4(out) == 3,
          "after teach, same signature serves locally");

    /* Local-only mode: abstain without teacher. */
    {
        PersonalAi ai2;
        PersonalAiPolicy p2;
        Port pmiss = sym_port("pai_miss");
        personal_ai_policy_defaults(&p2);
        p2.allow_teacher = 0;
        remove("tmp_personal_ai2.cnb");
        remove("tmp_personal_ai2.gaps.txt");
        check(personal_ai_open(&ai2, "tmp_personal_ai2.cnb",
                               "tmp_personal_ai2.gaps.txt", NULL, &p2) == 0,
              "local-only personal_ai opens");
        onehot(in, 0);
        check(personal_ai_serve(&ai2, pin, pmiss, in, SYM, out, SYM, &rep) != 0 &&
                  rep.source == PERSONAL_AI_ABSTAIN && rep.gap_noted,
              "no plan + no teacher → honest abstain + gap");
        personal_ai_close(&ai2);
        remove("tmp_personal_ai2.cnb");
        remove("tmp_personal_ai2.gaps.txt");
    }

    {
        PersonalAiReport tot;
        personal_ai_totals(&ai, &tot);
        check(tot.local_hits >= 2 && tot.teacher_helps >= 1,
              "cumulative counters track local hits + teacher helps");
    }

    check(strcmp(personal_ai_source_name(PERSONAL_AI_LOCAL), "local") == 0,
          "source name local");

    personal_ai_close(&ai);
    remove(base);
    remove(ledger);
    remove(inbox);
    (void)i;

    if (failures) {
        printf("PERSONAL_AI_FAIL failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("PERSONAL_AI_PASS checks=%d\n", checks);
    return 0;
}
