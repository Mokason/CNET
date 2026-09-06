/* RED: PERSONAL_AI_REROUTE_RED — trust must follow actual alternate execution. */
#define main personal_fixture_main
#include "test_personal_ai.c"
#undef main
#include "cnet_seal_trust.h"
#include <unistd.h>

static void mature(CnetSealTrust *st, const char *name) {
    cnet_seal_trust_seed_prior(st, name, 80, 80);
    for (int i = 0; i < 8; i++) {
        cnet_seal_trust_note_seal(st, name, 0);
        cnet_seal_trust_note_outcome(st, name, 1);
    }
}

int main(void) {
    PersonalAi ai;
    PersonalAiPolicy pol;
    PersonalAiReport rep;
    double input[SYM], output[SYM], inputs[SYM][SYM], targets[SYM][SYM];
    BinaryTransformNetwork alt;
    Contract ct;
    Specialist spec;
    char root[] = "/tmp/cnet-reroute-XXXXXX", base[256], ledger[256], inbox[256];
    if (!mkdtemp(root)) return 2;
    snprintf(base, sizeof base, "%s/base.cnb", root);
    snprintf(ledger, sizeof ledger, "%s/gaps", root);
    snprintf(inbox, sizeof inbox, "%s/inbox", root);
    setenv("CNET_SEAL_TRUST", "1", 1);
    setenv("CNET_SEAL_TRUST_LEDGER", "", 1);
    setenv("CNET_SEAL_TRUST_JOURNAL", "", 1);
    setenv("CNET_COVERAGE_ABSTAIN", "1", 1);
    cnet_seal_trust_global_shutdown();
    CnetSealTrust *st = cnet_seal_trust_global();
    if (!st) return 2;
    st->cfg.min_lcb = .90;
    st->cfg.min_live_outcomes = 5;
    st->cfg.explore_rate = st->cfg.explore_warmup_rate = 0;
    personal_ai_policy_defaults(&pol);
    pol.allow_teacher = pol.allow_residual = pol.allow_soft = pol.allow_medium = 0;
    if (personal_ai_open(&ai, base, ledger, inbox, &pol) != 0 ||
        admit_local_rot2(&ai.lane.reg) != 0) return 2;
    mature(st, "ledger_only");
    setenv("CNET_DISTRUST_REROUTE_DOMAIN", "ledger_only", 1);
    onehot(input, 1);
    personal_ai_serve(&ai, sym_port("pai_in"), sym_port("pai_local"), input, SYM, output, SYM, &rep);
    check(rep.source != PERSONAL_AI_LOCAL, "ledger-only target cannot certify source output");
    for (int i = 0; i < SYM; i++) {
        onehot(inputs[i], i);
        onehot(targets[i], (i + 1) % SYM);
    }
    memset(&alt, 0, sizeof alt);
    memset(&ct, 0, sizeof ct);
    if (btn_init(&alt, SYM, SYM, 8, 32, .5, 17) != 0 ||
        btn_set_ports(&alt, sym_port("pai_in"), sym_port("pai_local")) != 0) return 2;
    btn_train_dynamic(&alt, inputs[0], targets[0], SYM, 20000, 200, 1e-5, 1e-7);
    btn_train(&alt, inputs[0], targets[0], SYM, 3000);
    if (contract_init_borrowed(&ct, "alt_rot1", &alt, inputs[0], targets[0], SYM) != 0 ||
        specialist_wrap_btn(&spec, &alt, "alt_rot1") != 0 ||
        specialist_admit(&ai.lane.reg, &spec, &ct) != 0) return 2;
    contract_free(&ct);
    mature(st, "alt_rot1");
    setenv("CNET_DISTRUST_REROUTE_DOMAIN", "alt_rot1", 1);
    int rc = personal_ai_serve(&ai, sym_port("pai_in"), sym_port("pai_local"), input, SYM, output, SYM, &rep);
    check(rc == 0 && rep.source == PERSONAL_AI_LOCAL && argmax4(output) == 2,
          "compatible alternate executes its own rot1 output");
    check(hybrid_coverage_record(&ai.hybrid, sym_port("pai_in"), sym_port("pai_local"),
          "alt_rot1", inputs[0], targets[0], 1, SYM, SYM) == 0, "install alternate restricted coverage");
    personal_ai_serve(&ai, sym_port("pai_in"), sym_port("pai_local"), input, SYM, output, SYM, &rep);
    check(rep.source != PERSONAL_AI_LOCAL, "alternate outside coverage refuses");
    hybrid_coverage_forget_unit(&ai.hybrid, "alt_rot1");
    alt.output_ports[0] = sym_port("incompatible");
    personal_ai_serve(&ai, sym_port("pai_in"), sym_port("pai_local"), input, SYM, output, SYM, &rep);
    check(rep.source != PERSONAL_AI_LOCAL, "incompatible or mutated alternate refuses");
    BinaryTransformNetwork *owned = ai.lane.reg.entries[0].btn;
    personal_ai_close(&ai);
    btn_free(owned); free(owned);
    btn_free(&alt);
    cnet_seal_trust_global_shutdown();
    remove(base); remove(ledger); remove(inbox); rmdir(root);
    printf("PERSONAL_AI_REROUTE_%s checks=%d failures=%d\n", failures ? "RED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
