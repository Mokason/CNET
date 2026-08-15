/* test_cnet_capsule_loop — CAPSULE LOOP — BOUNDED CALL LOG
 *
 * Steal DeepSeek loop shape, not their runtime.
 * Exact never escalates. OOD abstains. Not open chat, not F11.
 * residual_calls == 0 && teacher_calls == 0. No Python.
 */
#include "../include/cnet_capsule_loop.h"
#include "../include/cnet_lookup.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void check(int cond, const char *msg)
{
    checks++;
    if (cond) {
        printf("  ok   %s\n", msg);
    } else {
        printf("  FAIL %s\n", msg);
        failures++;
    }
}

static void fill_hop(CnetChatLookupTurn *hop, const char *value,
                     unsigned residual)
{
    memset(hop, 0, sizeof *hop);
    hop->answered = 1;
    hop->report.bound = 1;
    hop->residual_calls = residual;
    snprintf(hop->report.value, sizeof hop->report.value, "%s", value);
    snprintf(hop->report.host, sizeof hop->report.host, "local-file");
    snprintf(hop->spoken, sizeof hop->spoken, "Marble reports %s from local-file.",
             value);
}

int main(void)
{
    CnetCapsuleLoopResult r;
    CnetChatLookupTurn hop;
    CnetCapsulePerm p;
    const char *teacher = "The capital of France is Paris and the year is 1799.";

    printf("cnet_capsule_loop — BOUNDED CALL LOG / EXACT NEVER ESCALATES\n");

    check(CNET_CAPSULE_LOOP_MAX_STEPS == 2, "max_steps_is_2");

    check(cnet_capsule_loop_count_subjects("increment 41") == 1,
          "count_one_increment");
    check(cnet_capsule_loop_count_subjects("increment 41 then crc8") == 2,
          "count_two_inc_crc");
    check(cnet_capsule_loop_count_subjects(
              "increment 41 then crc8 then lookup") == 3,
          "count_three_skills");
    check(cnet_capsule_loop_count_subjects("bake bread") == 0, "count_ood_zero");
    check(cnet_capsule_loop_count_subjects("fix the repo") == 0,
          "count_fix_repo_zero");
    check(cnet_capsule_loop_count_subjects("what is 41 plus 1") == 0,
          "count_plus_zero");

    check(cnet_capsule_loop_run("increment 41", NULL, &r) == 0, "inc_rc");
    check(r.kind == CNET_CAPSULE_CALL_EXACT, "inc_exact");
    check(r.bound == 1, "inc_bound");
    check(strcmp(r.value, "42") == 0, "inc_value_42");
    check(strstr(r.spoken, "42") != NULL, "inc_spoken_has_A");
    check(r.claimed_cert == 1, "inc_claimed_cert");
    check(r.residual_calls == 0, "inc_residual_zero");
    check(r.teacher_calls == 0, "inc_teacher_zero");
    check(r.n_exact == 1, "inc_one_exact");
    check(r.n_calls == 1, "inc_one_call");
    check(strstr(r.spoken, teacher) == NULL, "inc_not_teacher_text");

    check(cnet_capsule_loop_run("apply crc8_atm to input byte 1", NULL, &r) == 0,
          "crc_rc");
    check(r.kind == CNET_CAPSULE_CALL_EXACT, "crc_exact");
    check(strcmp(r.value, "7") == 0, "crc_value_1_is_7");
    check(r.claimed_cert == 1, "crc_claimed_cert");
    check(r.residual_calls == 0, "crc_residual_zero");
    check(r.teacher_calls == 0, "crc_teacher_zero");
    check(strstr(r.spoken, "7") != NULL, "crc_spoken_has_A");

    check(cnet_capsule_loop_run("Tell me how to bake bread.", NULL, &r) == 0,
          "ood_bake_rc");
    check(r.kind == CNET_CAPSULE_CALL_ABSTAIN, "ood_bake_abstain");
    check(r.bound == 0, "ood_bake_unbound");
    check(r.claimed_cert == 0, "ood_bake_no_cert");
    check(r.residual_calls == 0, "ood_bake_residual_zero");
    check(r.teacher_calls == 0, "ood_bake_teacher_zero");
    check(strcmp(r.refusal, "ood_no_skill") == 0, "ood_bake_reason");
    check(r.n_exact == 0, "ood_bake_no_exact");
    check(strstr(r.spoken, teacher) == NULL, "ood_bake_not_teacher");

    check(cnet_capsule_loop_run("fix the repo", NULL, &r) == 0, "ood_fix_rc");
    check(r.kind == CNET_CAPSULE_CALL_ABSTAIN, "ood_fix_abstain");
    check(r.claimed_cert == 0, "ood_fix_no_cert");
    check(r.teacher_calls == 0, "ood_fix_teacher_zero");
    check(r.residual_calls == 0, "ood_fix_residual_zero");
    check(strcmp(r.refusal, "ood_no_skill") == 0, "ood_fix_reason");
    check(r.n_exact == 0, "ood_fix_no_exact");

    check(cnet_capsule_loop_run("what is 41 plus 1", NULL, &r) == 0,
          "no_subject_rc");
    check(r.kind == CNET_CAPSULE_CALL_ABSTAIN, "no_subject_abstain");
    check(strcmp(r.refusal, "ood_no_skill") == 0, "no_subject_ood");
    check(r.claimed_cert == 0, "no_subject_no_cert");
    check(r.teacher_calls == 0, "no_subject_no_teacher");

    fill_hop(&hop, "13", 1);
    check(cnet_capsule_loop_run("lookup https://example.com/n", &hop, &r) == 0,
          "residual_hop_rc");
    check(r.kind != CNET_CAPSULE_CALL_EXACT, "residual_hop_not_exact");
    check(r.bound == 0, "residual_hop_unbound");
    check(r.claimed_cert == 0, "residual_hop_no_cert");
    check(strcmp(r.refusal, "residual_mouth") == 0, "residual_hop_reason");
    check(r.teacher_calls == 0, "residual_hop_no_teacher");
    check(r.residual_calls == 0, "residual_hop_residual_zero");
    check(r.n_exact == 0, "residual_hop_no_exact_step");

    p = CNET_CAPSULE_PERM_ALLOW;
    p = cnet_capsule_loop_pre_execute(p, "not_a_capsule", NULL, 1);
    check(p == CNET_CAPSULE_PERM_DENY, "unknown_name_deny");
    p = cnet_capsule_loop_pre_execute(p, "increment_mod256", NULL, 1);
    check(p == CNET_CAPSULE_PERM_DENY, "deny_monotonic_stays_deny");
    p = cnet_capsule_loop_pre_execute(CNET_CAPSULE_PERM_DENY, "crc8_atm", NULL,
                                      1);
    check(p == CNET_CAPSULE_PERM_DENY, "deny_cannot_become_allow");

    p = cnet_capsule_loop_pre_execute(CNET_CAPSULE_PERM_ALLOW, "increment_mod256",
                                      NULL, 1);
    check(p == CNET_CAPSULE_PERM_ALLOW, "known_name_allow");

    fill_hop(&hop, "13", 1);
    p = cnet_capsule_loop_pre_execute(CNET_CAPSULE_PERM_ALLOW, "web_lookup_v1",
                                      &hop, 1);
    check(p == CNET_CAPSULE_PERM_DENY, "residual_pre_execute_deny");
    p = cnet_capsule_loop_pre_execute(p, "increment_mod256", NULL, 1);
    check(p == CNET_CAPSULE_PERM_DENY, "residual_deny_stays");

    p = cnet_capsule_loop_pre_execute(CNET_CAPSULE_PERM_ALLOW, "increment_mod256",
                                      NULL, 0);
    check(p == CNET_CAPSULE_PERM_ABSTAIN, "unverified_pre_abstain");
    p = cnet_capsule_loop_pre_execute(p, "increment_mod256", NULL, 1);
    check(p == CNET_CAPSULE_PERM_ABSTAIN, "abstain_cannot_become_allow");

    check(cnet_capsule_loop_bind_fixture("increment_mod256", "42", 0, &r) == 0,
          "unverified_fixture_rc");
    check(r.kind == CNET_CAPSULE_CALL_ABSTAIN, "unverified_abstain");
    check(r.claimed_cert == 0, "unverified_no_cert");
    check(r.teacher_calls == 0, "unverified_no_teacher");
    check(r.n_exact == 0, "unverified_no_exact");

    check(cnet_capsule_loop_bind_fixture("increment_mod256", "42", 1, &r) == 0,
          "verified_fixture_rc");
    check(r.kind == CNET_CAPSULE_CALL_EXACT, "verified_fixture_exact");
    check(strcmp(r.value, "42") == 0, "verified_fixture_value");
    check(r.claimed_cert == 1, "verified_fixture_cert");
    check(r.residual_calls == 0, "verified_fixture_residual");
    check(r.teacher_calls == 0, "verified_fixture_teacher");

    check(cnet_capsule_loop_run("increment 41 then crc8", NULL, &r) == 0,
          "two_step_rc");
    check(r.n_calls == 2, "two_step_two_calls");
    check(r.n_exact == 2, "two_step_two_exact");
    check(r.calls[0].kind == CNET_CAPSULE_CALL_EXACT, "two_step_0_exact");
    check(r.calls[1].kind == CNET_CAPSULE_CALL_EXACT, "two_step_1_exact");
    check(strcmp(r.calls[0].name, "increment_mod256") == 0, "two_step_0_inc");
    check(strcmp(r.calls[0].value, "42") == 0, "two_step_0_value_42");
    check(strcmp(r.calls[1].name, "crc8_atm") == 0, "two_step_1_crc");
    check(strcmp(r.calls[1].value, "223") == 0, "two_step_1_crc41");
    check(r.kind == CNET_CAPSULE_CALL_EXACT, "two_step_kind_exact");
    check(r.bound == 1, "two_step_bound");
    check(r.claimed_cert == 1, "two_step_cert");
    check(r.residual_calls == 0, "two_step_residual_zero");
    check(r.teacher_calls == 0, "two_step_teacher_zero");
    check(strstr(r.spoken, "223") != NULL, "two_step_spoken_last_A");
    check(strstr(r.spoken, teacher) == NULL, "two_step_not_teacher");
    check(r.calls[0].residual_calls == 0, "two_step_0_residual");
    check(r.calls[0].teacher_calls == 0, "two_step_0_teacher");
    check(r.calls[1].residual_calls == 0, "two_step_1_residual");
    check(r.calls[1].teacher_calls == 0, "two_step_1_teacher");

    check(cnet_capsule_loop_cd_ask("increment 41 then crc8", NULL, &r) == 0,
          "cd_ask_two_rc");
    check(r.n_exact == 2, "cd_ask_two_exact");
    check(r.teacher_calls == 0, "cd_ask_two_teacher");
    check(r.residual_calls == 0, "cd_ask_two_residual");

    check(cnet_capsule_loop_run("increment 41 then crc8 then lookup", NULL,
                                &r) == 0,
          "three_skill_rc");
    check(r.n_calls == 2, "three_skill_stops_at_2");
    check(r.n_exact == 2, "three_skill_two_exact");
    check(r.n_calls <= (unsigned)CNET_CAPSULE_LOOP_MAX_STEPS,
          "three_skill_max_steps");
    check(r.teacher_calls == 0, "three_skill_no_teacher");
    check(r.residual_calls == 0, "three_skill_no_residual");
    check(r.kind == CNET_CAPSULE_CALL_EXACT, "three_skill_still_exact");
    check(strcmp(r.calls[0].name, "increment_mod256") == 0, "three_skill_0");
    check(strcmp(r.calls[1].name, "crc8_atm") == 0, "three_skill_1");

    check(cnet_capsule_loop_run("apply increment_mod256", NULL, &r) == 0,
          "inc_no_operand_rc");
    check(r.kind == CNET_CAPSULE_CALL_ABSTAIN, "inc_no_operand_abstain");
    check(r.claimed_cert == 0, "inc_no_operand_no_cert");
    check(r.teacher_calls == 0, "inc_no_operand_teacher");
    check(r.residual_calls == 0, "inc_no_operand_residual");
    check(strcmp(r.skill, "increment_mod256") == 0, "inc_no_operand_named");

    fill_hop(&hop, "13", 0);
    check(cnet_capsule_loop_cd_ask("ignored", &hop, &r) == 0, "cd_ask_hop_rc");
    check(r.kind == CNET_CAPSULE_CALL_EXACT, "cd_ask_hop_exact");
    check(strcmp(r.value, "13") == 0, "cd_ask_hop_value");
    check(r.residual_calls == 0, "cd_ask_hop_residual");
    check(r.teacher_calls == 0, "cd_ask_hop_teacher");

    check(cnet_capsule_loop_cd_ask("bake bread", NULL, &r) == 0, "cd_ask_ood_rc");
    check(r.kind == CNET_CAPSULE_CALL_ABSTAIN, "cd_ask_ood_abstain");
    check(r.claimed_cert == 0, "cd_ask_ood_no_cert");
    check(r.teacher_calls == 0, "cd_ask_ood_teacher");

    check(cnet_capsule_loop_run(NULL, NULL, &r) == 0, "null_turn_rc");
    check(r.kind == CNET_CAPSULE_CALL_ABSTAIN, "null_turn_abstain");
    check(r.teacher_calls == 0, "null_turn_no_teacher");

    check(cnet_capsule_loop_run("increment 41 then crc8", NULL, NULL) == -1,
          "null_out_err");

    if (failures) {
        printf("CNET_CAPSULE_LOOP_RED failures=%d checks=%d\n", failures,
               checks);
        return 1;
    }
    printf("CNET_CAPSULE_LOOP_PASS\n");
    printf("checks=%d residual=0 teacher=0 python=0 broader_claims=WITHHELD\n",
           checks);
    printf("steps<=2 exact_never_escalates=1 ood_abstains=1 "
           "claimed_cert=1_iff_bound propose_neq_authority=1\n");
    return 0;
}
