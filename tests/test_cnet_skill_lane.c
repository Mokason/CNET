/* test_cnet_skill_lane — AICIMO HARNESS LANE — EXACT NEVER ESCALATES
 *
 * Ported law, not AICIMO runtime.
 * Exact capsule/lookup bind never calls residual/teacher.
 * Certified hops bind. Leftover returns 1. claimed_cert=0 on miss. No Python.
 */
#include "../include/cnet_skill_lane.h"
#include "../include/cnet_lookup.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void check(int cond, const char *msg) {
    checks++;
    if (cond) {
        printf("  ok   %s\n", msg);
    } else {
        printf("  FAIL %s\n", msg);
        failures++;
    }
}

static void fill_hop(CnetChatLookupTurn *hop, const char *value,
                     unsigned residual) {
    memset(hop, 0, sizeof *hop);
    hop->answered = 1;
    hop->report.bound = 1;
    hop->residual_calls = residual;
    snprintf(hop->report.value, sizeof hop->report.value, "%s", value);
    snprintf(hop->report.host, sizeof hop->report.host, "local-file");
    snprintf(hop->spoken, sizeof hop->spoken, "Marble reports %s from local-file.",
             value);
}

int main(void) {
    CnetSkillLaneResult r;
    CnetChatLookupTurn hop;
    char skill[64];
    const char *teacher = "The capital of France is Paris and the year is 1799.";

    printf("cnet_skill_lane — AICIMO HARNESS LANE / EXACT NEVER ESCALATES\n");

    check(cnet_skill_lane_route("apply increment_mod256 to 41", skill,
                                sizeof skill) == 0,
          "route_increment");
    check(strcmp(skill, "increment_mod256") == 0, "route_increment_name");

    check(cnet_skill_lane_route("crc8_atm byte 23", skill, sizeof skill) == 0,
          "route_crc");
    check(strcmp(skill, "crc8_atm") == 0, "route_crc_name");

    check(cnet_skill_lane_route("lookup https://example.com/n", skill,
                                sizeof skill) == 0,
          "route_lookup_url");
    check(strcmp(skill, CNET_LOOKUP_CONTRACT) == 0, "route_lookup_name");

    check(cnet_skill_lane_route("Tell me how to bake bread.", skill,
                                sizeof skill) == 0,
          "route_recipe_bake");
    check(strcmp(skill, "recipe_inform_v1") == 0, "route_recipe_name");

    check(cnet_skill_lane_route("fix the repo", skill, sizeof skill) == 1,
          "route_ood_fix");
    check(skill[0] == '\0', "route_ood_empty");

    check(cnet_skill_lane_turn("apply increment_mod256 to 41", &r) == 0,
          "inc_turn_rc");
    check(r.kind == CNET_SKILL_LANE_EXACT, "inc_exact");
    check(r.bound == 1, "inc_bound");
    check(strcmp(r.value, "42") == 0, "inc_value_42");
    check(strstr(r.spoken, "42") != NULL, "inc_spoken_has_A");
    check(r.claimed_cert == 1, "inc_claimed_cert");
    check(r.residual_calls == 0, "inc_residual_zero");
    check(r.teacher_calls == 0, "inc_teacher_zero");
    check(strstr(r.spoken, teacher) == NULL, "inc_not_teacher_text");

    check(cnet_skill_lane_turn("increment 255", &r) == 0, "inc_wrap_rc");
    check(strcmp(r.value, "0") == 0, "inc_255_is_0");
    check(r.residual_calls == 0, "inc_255_residual_zero");
    check(r.teacher_calls == 0, "inc_255_teacher_zero");

    check(cnet_skill_lane_bind_fixture("increment_mod256", "42", 1, &r) == 0,
          "inc_fixture_rc");
    check(r.kind == CNET_SKILL_LANE_EXACT, "inc_fixture_exact");
    check(strcmp(r.value, "42") == 0, "inc_fixture_value");
    check(r.residual_calls == 0, "inc_fixture_residual_zero");
    check(r.teacher_calls == 0, "inc_fixture_teacher_zero");
    check(r.claimed_cert == 1, "inc_fixture_cert");

    check(cnet_skill_lane_turn("apply crc8_atm to input byte 1", &r) == 0,
          "crc_turn_rc");
    check(r.kind == CNET_SKILL_LANE_EXACT, "crc_exact");
    check(strcmp(r.value, "7") == 0, "crc_value_1_is_7");
    check(r.claimed_cert == 1, "crc_claimed_cert");
    check(r.residual_calls == 0, "crc_residual_zero");
    check(r.teacher_calls == 0, "crc_teacher_zero");
    check(strstr(r.spoken, "7") != NULL, "crc_spoken_has_A");

    check(cnet_skill_lane_bind_exact("crc8_atm", 0u, &r) == 0, "crc_zero_rc");
    check(strcmp(r.value, "0") == 0, "crc_zero_value");
    check(r.residual_calls == 0, "crc_zero_residual");

    check(cnet_skill_lane_bind_fixture("crc8_atm", "101", 1, &r) == 0,
          "crc_fixture_rc");
    check(strcmp(r.value, "101") == 0, "crc_fixture_value");
    check(r.residual_calls == 0, "crc_fixture_residual");
    check(r.teacher_calls == 0, "crc_fixture_teacher");

    fill_hop(&hop, "13", 0);
    check(cnet_skill_lane_bind_lookup(&hop, &r) == 0, "lookup_bind_rc");
    check(r.kind == CNET_SKILL_LANE_EXACT, "lookup_exact");
    check(strcmp(r.value, "13") == 0, "lookup_value");
    check(strstr(r.spoken, "13") != NULL, "lookup_spoken_has_A");
    check(r.claimed_cert == 1, "lookup_cert");
    check(r.residual_calls == 0, "lookup_residual_zero");
    check(r.teacher_calls == 0, "lookup_teacher_zero");
    check(strstr(r.spoken, teacher) == NULL, "lookup_not_teacher");

    fill_hop(&hop, "13", 1);
    check(cnet_skill_lane_bind_lookup(&hop, &r) == 0, "lookup_residual_rc");
    check(r.kind == CNET_SKILL_LANE_ABSTAIN, "lookup_residual_abstain");
    check(r.bound == 0, "lookup_residual_unbound");
    check(r.claimed_cert == 0, "lookup_residual_no_cert");
    check(strcmp(r.refusal, "residual_mouth") == 0, "lookup_residual_reason");
    check(r.teacher_calls == 0, "lookup_residual_no_teacher");

    check(cnet_skill_lane_bind_lookup(NULL, &r) == 0, "lookup_null_rc");
    check(r.kind == CNET_SKILL_LANE_ABSTAIN, "lookup_null_abstain");
    check(r.claimed_cert == 0, "lookup_null_no_cert");

    check(cnet_skill_lane_turn("Tell me how to bake bread.", &r) == 0,
          "recipe_bake_rc");
    check(r.kind == CNET_SKILL_LANE_EXACT, "recipe_bake_exact");
    check(r.bound == 1, "recipe_bake_bound");
    check(strcmp(r.value, "bread") == 0, "recipe_bake_value");
    check(strstr(r.spoken, "flour") != NULL, "recipe_bake_spoken");
    check(r.claimed_cert == 1, "recipe_bake_cert");
    check(r.residual_calls == 0, "recipe_bake_residual_zero");
    check(r.teacher_calls == 0, "recipe_bake_teacher_zero");
    check(strstr(r.spoken, teacher) == NULL, "recipe_bake_not_teacher");

    check(cnet_skill_lane_cd_ask("bake bread", NULL, &r) == 0, "cd_ask_recipe_rc");
    check(r.kind == CNET_SKILL_LANE_EXACT, "cd_ask_recipe_exact");
    check(r.claimed_cert == 1, "cd_ask_recipe_cert");
    check(r.residual_calls == 0, "cd_ask_recipe_residual");
    check(r.teacher_calls == 0, "cd_ask_recipe_teacher");

    check(cnet_skill_lane_turn("what is 41 plus 1", &r) == 0, "add_plus_rc");
    check(r.kind == CNET_SKILL_LANE_EXACT, "add_plus_exact");
    check(strcmp(r.value, "42") == 0, "add_plus_42");
    check(r.claimed_cert == 1, "add_plus_cert");
    check(r.teacher_calls == 0, "add_plus_no_teacher");

    check(cnet_skill_lane_turn("what year was spacex created", &r) == 0,
          "year_rc");
    check(r.kind == CNET_SKILL_LANE_EXACT, "year_exact");
    check(strcmp(r.value, "2002") == 0, "year_2002");
    check(strstr(r.spoken, "2002") != NULL, "year_spoken");
    check(r.residual_calls == 0, "year_residual");
    check(r.teacher_calls == 0, "year_teacher");

    check(cnet_skill_lane_turn("fix the repo", &r) == 1, "leftover_fix_rc");
    check(r.kind == CNET_SKILL_LANE_ABSTAIN, "leftover_fix_abstain");
    check(r.claimed_cert == 0, "leftover_fix_no_cert");
    check(r.teacher_calls == 0, "leftover_fix_no_teacher");

    check(cnet_skill_lane_turn(
              "looking for moderate italian in the centre please", &r) == 0,
          "rest_rc");
    check(r.kind == CNET_SKILL_LANE_EXACT, "rest_exact");
    check(strcmp(r.value, "Cotto") == 0, "rest_cotto");
    check(strstr(r.spoken, "Cotto") != NULL, "rest_para");
    check(r.teacher_calls == 0, "rest_teacher");

    check(cnet_skill_lane_turn("apply increment_mod256", &r) == 0,
          "inc_no_operand_rc");
    check(r.kind == CNET_SKILL_LANE_ABSTAIN, "inc_no_operand_abstain");
    check(r.claimed_cert == 0, "inc_no_operand_no_cert");
    check(r.residual_calls == 0, "inc_no_operand_residual");
    check(r.teacher_calls == 0, "inc_no_operand_teacher");
    check(strcmp(r.skill, "increment_mod256") == 0, "inc_no_operand_named");

    fill_hop(&hop, "13", 0);
    check(cnet_skill_lane_cd_ask("ignored", &hop, &r) == 0, "cd_ask_hop_rc");
    check(r.kind == CNET_SKILL_LANE_EXACT, "cd_ask_hop_exact");
    check(strcmp(r.value, "13") == 0, "cd_ask_hop_value");
    check(r.residual_calls == 0, "cd_ask_hop_residual");
    check(r.teacher_calls == 0, "cd_ask_hop_teacher");

    check(cnet_skill_lane_bind_fixture("increment_mod256", "42", 0, &r) == 0,
          "unverified_fixture_rc");
    check(r.kind == CNET_SKILL_LANE_ABSTAIN, "unverified_abstain");
    check(r.claimed_cert == 0, "unverified_no_cert");

    check(cnet_skill_lane_cd_ask("increment 41", NULL, &r) == 0, "cd_ask_inc");
    check(strcmp(r.value, "42") == 0, "cd_ask_inc_value");
    check(r.teacher_calls == 0, "cd_ask_inc_teacher");
    check(r.residual_calls == 0, "cd_ask_inc_residual");

    if (failures) {
        printf("CNET_HARNESS_RED failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("CNET_HARNESS_PASS\n");
    printf("checks=%d residual=0 teacher=0 python=0 broader_claims=WITHHELD\n",
           checks);
    return 0;
}
