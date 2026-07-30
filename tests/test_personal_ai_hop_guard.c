/* test_personal_ai_hop_guard — ordinary serving must guard every hop.
 *
 * WHY THIS EXISTS. `knowledge_composition_bench` proves the DAG API *can*
 * enforce coverage at every hop, because the benchmark injects a guard. The
 * ordinary product path did not: `personal_ai_serve` checked the ORIGINAL
 * REQUEST against each mined unit in the plan and then called `route_execute`,
 * which takes no guard at all. For a chain that is the wrong question twice
 * over — hop 2 does not receive the request, it receives hop 1's OUTPUT, and
 * the request's ports are not hop 2's ports — so a composed answer could be
 * built from a hop operating outside its certified domain and still be reported
 * as Tier-A certified.
 *
 * The fixture is the smallest thing that shows it: two mined units chained
 * A -> B -> C, where hop 1 is certified over the whole input domain and hop 2 is
 * certified over only PART of the intermediate domain. One request is inside
 * hop 1's coverage, is itself a member of hop 2's recorded rows (so the
 * plan-level name check passes), and produces an intermediate hop 2 was never
 * certified on.
 *
 * Everything is written under a mkdtemp root. No real base, no runtime state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/base.h"
#include "../include/contract/contract.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/personal_ai.h"

#define SYM 4

static int failures;
static int checks;

static void check(int condition, const char *message) {
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static char dir_template[] = "/tmp/cnet-hopguard-XXXXXX";
static char *scratch;

static Port make_port(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = SYM;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static void one_hot(double *row, int hot) {
    int i;
    for (i = 0; i < SYM; i++) row[i] = (i == hot) ? 1.0 : 0.0;
}

/* Build a mined unit implementing `map` over the full one-hot domain, seal it
   into `base`, and record coverage for the first `cov_rows` inputs only. */
static int build_hop(CnetBase *base, HybridAi *cov, const char *name,
                     Port in_port, Port out_port, const int *map,
                     size_t cov_rows) {
    BinaryTransformNetwork btn;
    Contract contract;
    double in[SYM][SYM], target[SYM][SYM];
    size_t i;
    int rc = -1;

    for (i = 0; i < SYM; i++) {
        one_hot(in[i], (int)i);
        one_hot(target[i], map[i]);
    }
    memset(&btn, 0, sizeof btn);
    memset(&contract, 0, sizeof contract);
    if (btn_init(&btn, SYM, SYM, 16, 64, 0.5, 20260730u) != 0) return -1;
    btn_set_ports(&btn, in_port, out_port);
    btn_train_dynamic(&btn, (const double *)in, (const double *)target, SYM,
                      12000, 200, 1e-6, 1e-8);
    btn_train(&btn, (const double *)in, (const double *)target, SYM, 3000);
    if (contract_init_borrowed(&contract, name, &btn, (const double *)in,
                               (const double *)target, SYM) != 0) {
        btn_free(&btn);
        return -1;
    }
    if (cnb_add_unit(base, &btn, &contract, NULL) == 0) {
        rc = 0;
        if (cov && cov_rows)
            (void)hybrid_coverage_record(cov, in_port, out_port, name,
                                         (const double *)in,
                                         (const double *)target, cov_rows, SYM,
                                         SYM);
    }
    contract_free(&contract);
    btn_free(&btn);
    return rc;
}

int main(void) {
    CnetBase base;
    HybridAi cov;
    PersonalAi ai;
    PersonalAiPolicy policy;
    PersonalAiReport rep;
    char base_path[600], cover_path[700], ledger_path[600];
    Port pa = make_port("hopguard_alpha");
    Port pb = make_port("hopguard_bravo");
    Port pc = make_port("hopguard_gamma");
    /* hop1: identity on the whole domain, so every request maps to itself.
       hop2: identity as well -- what matters is not the mapping but WHICH
       intermediate values hop2 was certified on. */
    const int identity[SYM] = {0, 1, 2, 3};
    double input[SYM], output[SYM];
    const char *unit_a = HYBRID_MINED_UNIT_PREFIX "hopalpha";
    const char *unit_b = HYBRID_MINED_UNIT_PREFIX "hopbravo";
    int rc;

    setvbuf(stdout, NULL, _IONBF, 0);
    scratch = mkdtemp(dir_template);
    if (!scratch) {
        fprintf(stderr, "FAIL: cannot create scratch directory\n");
        return 1;
    }
    snprintf(base_path, sizeof base_path, "%s/hop.cnb", scratch);
    snprintf(cover_path, sizeof cover_path, "%s/hop.cnb.coverage", scratch);
    snprintf(ledger_path, sizeof ledger_path, "%s/hop.gaps.txt", scratch);

    cnb_init(&base);
    hybrid_ai_init(&cov);

    /* hop1 is certified over the WHOLE input domain. */
    check(build_hop(&base, &cov, unit_a, pa, pb, identity, SYM) == 0,
          "hop 1 is built and certified over its whole domain");
    /* hop2 is certified over only the first two intermediate values. Row 0 is
       among them, which is what makes the plan-level name check pass for a
       request of one-hot(0) while hop 2 is NOT certified on one-hot(3). */
    check(build_hop(&base, &cov, unit_b, pb, pc, identity, 2) == 0,
          "hop 2 is built and certified over only part of its domain");

    check(cnb_save(&base, base_path) == 0, "the fixture base saves");
    check(hybrid_coverage_save(&cov, cover_path) == 0,
          "the fixture sidecar saves");
    hybrid_ai_free(&cov);
    cnb_free(&base);

    personal_ai_policy_defaults(&policy);
    policy.allow_teacher = 0;
    policy.allow_soft = 0;
    policy.allow_residual = 0;
    policy.allow_medium = 0;
    policy.structure_mine_on_serve = 0;
    memset(&ai, 0, sizeof ai);
    if (personal_ai_open(&ai, base_path, ledger_path, NULL, &policy) != 0) {
        fprintf(stderr, "FAIL: personal_ai_open on the fixture base\n");
        return 1;
    }

    /* --- control: a request whose whole chain stays inside coverage -------
       one-hot(0) -> hop1 -> one-hot(0) -> hop2, and hop2 IS certified on
       one-hot(0). This must still be served Tier-A certified, or the guard
       would be refusing everything and proving nothing. */
    one_hot(input, 0);
    memset(&rep, 0, sizeof rep);
    memset(output, 0, sizeof output);
    rc = personal_ai_serve(&ai, pa, pc, input, SYM, output, SYM, &rep);
    check(rc == 0, "control: a fully covered chain serves");
    check(rep.source == PERSONAL_AI_LOCAL,
          "control: a fully covered chain is served Tier-A certified");
    check(rep.coverage_abstains == 0,
          "control: a fully covered chain does not abstain");

    /* --- the defect: hop 1 covered, hop 2 handed an uncovered intermediate --
       one-hot(3) is inside hop1's coverage. hop1 maps it to one-hot(3), which
       hop2 was never certified on. The plan-level check cannot see that: it
       tests the REQUEST, and the request is one of hop2's recorded rows only by
       coincidence of dimension. Only an execution-time guard sees the value
       hop2 is actually about to consume. */
    one_hot(input, 3);
    memset(&rep, 0, sizeof rep);
    memset(output, 0, sizeof output);
    rc = personal_ai_serve(&ai, pa, pc, input, SYM, output, SYM, &rep);
    check(rep.source != PERSONAL_AI_LOCAL,
          "an uncovered INTERMEDIATE must not be served as Tier-A certified");
    check(rep.coverage_abstains == 1,
          "the refusal is recorded as a coverage abstention");
    check(rep.trust != HYBRID_TRUST_CERTIFIED || rep.source != PERSONAL_AI_LOCAL,
          "no certified authority is claimed for the refused chain");
    /* Every other tier is disabled by policy, so the only honest outcome left
       is an abstention -- never a partial chain presented as an answer. */
    check(rc != 0 || rep.source == PERSONAL_AI_ABSTAIN,
          "the refused chain abstains rather than serving a partial result");
    printf("HOPGUARD_REFUSAL source=%d trust=%d coverage_abstains=%zu rc=%d\n",
           (int)rep.source, (int)rep.trust, rep.coverage_abstains, rc);

    /* --- the control again, after the refusal -----------------------------
       A refusal must not poison the lane: the covered request still serves. */
    one_hot(input, 0);
    memset(&rep, 0, sizeof rep);
    rc = personal_ai_serve(&ai, pa, pc, input, SYM, output, SYM, &rep);
    check(rc == 0 && rep.source == PERSONAL_AI_LOCAL,
          "a covered chain still serves after a refused one");

    /* --- the ROOT hop is guarded too --------------------------------------
       An input outside hop1's own coverage must be refused at the first hop,
       before anything downstream runs. hop1 covers the whole domain here, so
       this is exercised by forgetting its record: the mined unit then has no
       bound coverage at all and fail-closed applies. */
    check(hybrid_coverage_forget_unit(&ai.hybrid, unit_a) == 1,
          "hop 1's coverage record is removed");
    hybrid_coverage_arm_fail_closed(&ai.hybrid, 1);
    one_hot(input, 0);
    memset(&rep, 0, sizeof rep);
    rc = personal_ai_serve(&ai, pa, pc, input, SYM, output, SYM, &rep);
    check(rep.source != PERSONAL_AI_LOCAL,
          "a root hop with no bound coverage is refused too");
    check(rep.coverage_abstains == 1,
          "the root refusal is also recorded as a coverage abstention");

    personal_ai_close(&ai);

    if (failures) {
        printf("PERSONAL_AI_HOP_GUARD_FAIL checks=%d failures=%d\n", checks,
               failures);
        return 1;
    }
    printf("PERSONAL_AI_HOP_GUARD_PASS checks=%d hops_guarded=every "
           "refusals=root+intermediate\n", checks);
    return 0;
}
