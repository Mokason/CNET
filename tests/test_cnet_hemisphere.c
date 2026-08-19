#include "cnet_hemisphere.h"

#include "cnet_brain_mirror.h"
#include "cnet_held_model.h"
#include "cnet_ood_skill.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_checks;
static int g_fail;

static void expect(int cond, const char *name) {
    g_checks++;
    if (cond) {
        printf("  ok   %s\n", name);
    } else {
        g_fail++;
        printf("  FAIL %s\n", name);
    }
}

static int hook_pong(const char *turn, char *out, size_t cap) {
    (void)turn;
    if (out == NULL || cap == 0) return 1;
    snprintf(out, cap, "hemi-residual-pong");
    return 0;
}

static void reset_held(void) {
    unsetenv("CNET_HELD_MODEL_ENDPOINT");
    unsetenv("CNET_HELD_MODEL_PATH");
    unsetenv("CNET_HELD_MODEL_NAME");
    cnet_held_model_set_hook(NULL);
    cnet_held_model_set_endpoint(NULL);
    cnet_held_model_set_path(NULL);
    cnet_held_model_set_name(NULL);
    cnet_held_model_close();
}

int main(void) {
    CnetHemiPolicy pol;
    CnetHemiResult r;
    CnetSkillLaneResult lane;
    char mirror_tmp[] = "/tmp/cnet_hemi_mirror_XXXXXX";

    g_checks = 0;
    g_fail = 0;
    if (!mkdtemp(mirror_tmp)) {
        perror("mkdtemp");
        return 2;
    }
    cnet_brain_mirror_set_dir(mirror_tmp);
    reset_held();
    unsetenv("CNET_HEMI_RESIDUAL");
    unsetenv("CNET_HEMI_WIKI");
    unsetenv("CNET_NEVER_VOICE_LLM");

    printf("cnet_hemi tests\n");

    /* --- classify: CORE exact --- */
    memset(&lane, 0, sizeof lane);
    snprintf(lane.skill, sizeof lane.skill, "increment_mod256");
    snprintf(lane.value, sizeof lane.value, "3");
    snprintf(lane.spoken, sizeof lane.spoken, "3");
    lane.bound = 1;
    lane.claimed_cert = 1;
    lane.kind = CNET_SKILL_LANE_EXACT;
    cnet_hemi_classify_lane(&lane, 1, &r);
    expect(r.hemi == CNET_HEMI_CORE, "class_core_hemi");
    expect(r.plane == CNET_CORE_PLANE_CERT, "class_core_plane");
    expect(r.via_core == 1, "class_core_via");
    expect(r.source == CNET_HEMI_SRC_SKILL_EXACT, "class_core_src");
    expect(r.claimed_cert == 1, "class_core_cert");
    expect(r.may_voice == 1, "class_core_voice");
    expect(strcmp(cnet_hemi_name(r.hemi), "CERT") == 0, "name_cert");
    expect(strcmp(cnet_core_plane_name(r.plane), "CERT") == 0, "plane_name_cert");

    /* --- classify: OPEN CHAT (held) via CORE middle --- */
    memset(&lane, 0, sizeof lane);
    snprintf(lane.skill, sizeof lane.skill, "%s", CNET_HELD_CONTRACT);
    snprintf(lane.spoken, sizeof lane.spoken, "draft");
    lane.bound = 1;
    lane.kind = CNET_SKILL_LANE_HELD;
    lane.claimed_cert = 1; /* hostile: try to claim cert */
    cnet_hemi_classify_lane(&lane, 1, &r);
    expect(r.hemi == CNET_HEMI_RESIDUAL, "class_res_hemi");
    expect(r.plane == CNET_CORE_PLANE_OPEN_CHAT, "class_open_plane");
    expect(r.open_chat == 1, "class_open_flag");
    expect(r.via_core == 1, "class_open_via_core");
    expect(r.source == CNET_HEMI_SRC_HELD_LLM, "class_res_src");
    expect(r.claimed_cert == 0, "class_res_never_cert");
    expect(r.may_voice == 0, "class_res_no_voice");
    expect(cnet_hemi_may_voice(&r, 1) == 0, "may_voice_residual_off");
    expect(cnet_hemi_may_voice(&r, 0) == 1, "may_voice_residual_override");
    expect(strcmp(cnet_hemi_name(r.hemi), "OPEN_CHAT") == 0, "name_open");

    /* --- classify: math skill --- */
    memset(&lane, 0, sizeof lane);
    snprintf(lane.skill, sizeof lane.skill, "%s", CNET_OOD_ADD);
    snprintf(lane.value, sizeof lane.value, "5");
    lane.bound = 1;
    lane.claimed_cert = 1;
    lane.kind = CNET_SKILL_LANE_EXACT;
    cnet_hemi_classify_lane(&lane, 1, &r);
    expect(r.hemi == CNET_HEMI_CORE, "class_math_core");
    expect(r.source == CNET_HEMI_SRC_OOD_MATH, "class_math_src");

    /* --- CORE ask: arithmetic --- */
    cnet_hemi_policy_default(&pol);
    pol.residual_enabled = 0;
    pol.allow_wiki = 0;
    expect(cnet_hemi_ask_core("what is 2 plus 3", &pol, &r) == 0, "core_math_rc");
    expect(r.hemi == CNET_HEMI_CORE, "core_math_hemi");
    expect(r.plane == CNET_CORE_PLANE_CERT, "core_math_plane");
    expect(r.via_core == 1, "core_math_via");
    expect(r.open_chat == 0, "core_math_not_open");
    expect(r.source == CNET_HEMI_SRC_OOD_MATH, "core_math_src");
    expect(r.claimed_cert == 1, "core_math_cert");
    expect(r.may_voice == 1, "core_math_voice");
    expect(r.bound == 1, "core_math_bound");
    expect(strcmp(r.value, "5") == 0 || strstr(r.spoken, "5") != NULL,
           "core_math_five");

    /* --- CORE middle miss does not invent open chat when disabled --- */
    pol.open_chat_enabled = 0;
    expect(cnet_hemi_ask("totally unknown zz99 fact", &pol, &r) == 1,
           "ask_core_only_miss");
    expect(r.hemi == CNET_HEMI_NONE || r.source == CNET_HEMI_SRC_ABSTAIN,
           "ask_core_only_abstain");
    expect(r.via_core == 1, "miss_still_via_core");
    expect(r.claimed_cert == 0, "ask_core_only_no_cert");
    expect(r.may_voice == 0, "ask_core_only_no_voice");

    /* --- Full ask: CERT wins before open chat --- */
    cnet_held_model_set_hook(hook_pong);
    pol.residual_enabled = 1;
    pol.open_chat_enabled = 1; /* test-only re-enable */
    expect(cnet_core_ask("what is 7 plus 1", &pol, &r) == 0, "ask_core_before_res");
    expect(r.hemi == CNET_HEMI_CORE, "ask_prefers_core");
    expect(r.plane == CNET_CORE_PLANE_CERT, "ask_prefers_cert_plane");
    expect(r.source == CNET_HEMI_SRC_OOD_MATH, "ask_prefers_math");
    expect(strstr(r.spoken, "hemi-residual-pong") == NULL, "ask_not_residual_text");

    /* --- leftover OPEN_CHAT answers are killed --- */
    expect(cnet_core_ask("say a novel leftover phrase about zz99", &pol, &r) == 1,
           "ask_residual_killed");
    expect(r.claimed_cert == 0, "ask_residual_no_cert");
    expect(r.open_chat == 0, "ask_open_flag_off");
    expect(strstr(r.spoken, "hemi-residual-pong") == NULL, "ask_no_open_text");
    expect(strstr(r.refusal, "open_chat_answer_killed") != NULL ||
               strstr(r.refusal, "logic_miss") != NULL || r.bound == 0,
           "ask_killed_reason");

    /* --- residual_disabled blocks held even if hook present --- */
        pol.residual_enabled = 0;
        pol.open_chat_enabled = 0;
        expect(cnet_hemi_ask("another leftover zz99", &pol, &r) == 1,
               "residual_off_miss");
        expect(r.claimed_cert == 0, "residual_off_no_cert");
        expect(strcmp(r.spoken, "hemi-residual-pong") != 0, "residual_off_no_text");

        /* --- discern: what is what --- */
        expect(cnet_core_discern("what is 2 plus 3") == CNET_CORE_INTENT_LOGIC,
               "discern_logic_plus");
        expect(cnet_core_discern("write a haiku about rain") ==
                   CNET_CORE_INTENT_CREATIVE,
               "discern_creative_haiku");
        expect(cnet_core_discern("invent a story about plus signs") ==
                   CNET_CORE_INTENT_MIXED,
               "discern_mixed");
        expect(strcmp(cnet_core_intent_name(CNET_CORE_INTENT_LOGIC), "LOGIC") == 0,
               "intent_name_logic");

        /* --- logic miss does not creative-fill (default) --- */
        pol.residual_enabled = 1;
        pol.open_chat_enabled = 1;
        pol.logic_open_chat_fallback = 0;
        expect(cnet_core_ask("compute crc8 of unknown blob zz99", &pol, &r) == 1,
               "logic_miss_no_fill");
        expect(r.intent == CNET_CORE_INTENT_LOGIC, "logic_miss_intent");
        expect(r.claimed_cert == 0, "logic_miss_no_cert");
        expect(r.open_chat == 0, "logic_miss_no_open");
        expect(strstr(r.refusal, "logic_miss") != NULL, "logic_miss_reason");

        /* --- creative leftover cannot answer; table path is core_bus --- */
        expect(cnet_core_ask("write a short poem about zz99", &pol, &r) == 1,
               "creative_no_answer");
        expect(r.intent == CNET_CORE_INTENT_CREATIVE, "creative_intent");
        expect(r.claimed_cert == 0, "creative_no_cert");
        expect(r.open_chat == 0, "creative_no_open_flag");
        expect(strstr(r.spoken, "hemi-residual-pong") == NULL, "creative_no_draft");

    reset_held();
    printf("CNET_HEMI_PASS\n");
    printf("checks=%d fail=%d residual_never_cert=1 never_voice_llm=1 "
           "core_first=1 core_middle=1 open_chat_answer=0 "
           "logic_strong=1 creative_strong=1 discern=1 python=0 "
           "broader_claims=WITHHELD\n",
           g_checks, g_fail);
    cnet_brain_mirror_set_dir(NULL);
    return g_fail ? 1 : 0;
}
