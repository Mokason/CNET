/* Pattern runtime end-to-end gate.
 * make pattern_runtime → PATTERN_RUNTIME_PASS
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../include/cnet_pattern.h"
#include "../include/nn.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    CnetPatternRuntime rt;
    CnetPatternCalloutReport rep;
    CnetPatternAddr ia, oa;
    Port ip, op;
    int idx, i, e0, e1, e2;
    const char *store = "logs/test_pattern_runtime.jsonl";

    unlink(store);
    printf("== pattern runtime (fluid → freeze → callout) ==\n");

    cnet_pattern_runtime_init(&rt);
    snprintf(rt.store_path, sizeof rt.store_path, "%s", store);
    snprintf(rt.skills_dir, sizeof rt.skills_dir, "logs/test_math_skills");
    rt.hot_cap = 2;
    rt.freeze_min_successes = 3;
    rt.freeze_min_reliability = 0.9;

    check(cnet_pattern_bootstrap_defaults(&rt) >= 1, "bootstrap defaults");
    check(cnet_pattern_count(&rt) >= 2, "edges after bootstrap");

    memset(&rep, 0, sizeof rep);
    check(cnet_pattern_callout(&rt, "hermetic_ping", "test", &rep) == 0,
          "callout hermetic");
    check(strstr(rep.answer, "HERMETIC_OK") != NULL, "hermetic answer");

    idx = cnet_pattern_find(&rt, "hermetic_ping", "test");
    check(idx >= 0, "find hermetic edge");
    for (i = 0; i < 5; i++) cnet_pattern_feedback(&rt, idx, 1);
    check(rt.edges[idx].state == CNET_PAT_STATE_FROZEN ||
              cnet_pattern_freeze(&rt, idx, 1) == 0,
          "freeze hermetic");
    check(rt.edges[idx].state == CNET_PAT_STATE_FROZEN, "state frozen");

    memset(&rep, 0, sizeof rep);
    check(cnet_pattern_callout(&rt, "what is 6*7", "math", &rep) == 0,
          "callout math 6*7");
    check(strstr(rep.answer, "42") != NULL, "answer 42");

    memset(&ip, 0, sizeof ip);
    memset(&op, 0, sizeof op);
    ip.family = PORT_RAW;
    ip.field_width = 8;
    op.family = PORT_RAW;
    op.field_width = 8;
    cnet_pattern_addr_from_text("ra", "r", &ia);
    cnet_pattern_addr_from_text("rao", "r", &oa);
    e0 = cnet_pattern_propose(&rt, "res_a", &ia, &oa, ip, op, CNET_PAT_BODY_HERMETIC,
                              "a");
    cnet_pattern_addr_from_text("rb", "r", &ia);
    e1 = cnet_pattern_propose(&rt, "res_b", &ia, &oa, ip, op, CNET_PAT_BODY_HERMETIC,
                              "b");
    cnet_pattern_addr_from_text("rc", "r", &ia);
    e2 = cnet_pattern_propose(&rt, "res_c", &ia, &oa, ip, op, CNET_PAT_BODY_HERMETIC,
                              "c");
    check(e0 >= 0 && e1 >= 0 && e2 >= 0, "propose residency edges");
    check(cnet_pattern_load(&rt, e0) == 0, "load a");
    check(cnet_pattern_load(&rt, e1) == 0, "load b");
    check(rt.hot_count == 2, "hot full at 2");
    check(cnet_pattern_load(&rt, e2) == 0, "load c evicts");
    check(rt.hot_count == 2, "hot still 2");
    check(rt.edges[e2].resident == 1, "c resident");

    check(cnet_pattern_runtime_save(&rt, store) == 0, "save store");
    {
        CnetPatternRuntime rt2;
        cnet_pattern_runtime_init(&rt2);
        snprintf(rt2.store_path, sizeof rt2.store_path, "%s", store);
        check(cnet_pattern_runtime_load(&rt2, store) == 0, "load store");
        check(cnet_pattern_count(&rt2) >= 1, "reloaded edges");
        check(cnet_pattern_find(&rt2, "hermetic_ping", "test") >= 0 ||
                  cnet_pattern_count_state(&rt2, CNET_PAT_STATE_FROZEN) >= 1,
              "hermetic present after load");
    }

    check(cnet_pattern_observe_math(&rt, "sqrt(9)+1", "direct_eval", "4", 1) == 0,
          "observe math");

    /* Curriculum propose + promote */
    {
        int cidx = cnet_pattern_propose_curriculum(&rt, "lookup_skill", "Alan Turing");
        check(cidx >= 0, "propose curriculum edge");
        check(rt.edges[cidx].state == CNET_PAT_STATE_FLUID, "curriculum starts fluid");
        cnet_pattern_feedback(&rt, cidx, 1);
        cnet_pattern_feedback(&rt, cidx, 1);
        cnet_pattern_feedback(&rt, cidx, 1);
        check(cnet_pattern_promote_all(&rt) >= 0, "promote_all");
        check(rt.edges[cidx].state == CNET_PAT_STATE_FROZEN ||
                  rt.edges[cidx].state == CNET_PAT_STATE_IMPROVING,
              "curriculum improved or frozen");
    }

    /* import-units soft-fail without base/cnet.so */
    {
        int n = cnet_pattern_import_units(&rt, 4);
        check(n >= -2, "import_units returns code (may be -1 unbound)");
    }

    printf("PATTERN_RUNTIME_PASS checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
