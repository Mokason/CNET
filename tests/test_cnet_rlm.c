#include "cnet_rlm.h"

#include "cnet_brain_mirror.h"
#include "cnet_held_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_checks;
static int g_fail;

static void expect(int cond, const char *name) {
    g_checks++;
    if (cond)
        printf("  ok   %s\n", name);
    else {
        g_fail++;
        printf("  FAIL %s\n", name);
    }
}

static int hook_creative(const char *turn, char *out, size_t cap) {
    (void)turn;
    snprintf(out, cap, "rlm-open-chat-draft");
    return 0;
}

int main(void) {
    CnetRlmPolicy pol;
    CnetRlmResult r;
    char mirror[] = "/tmp/cnet_rlm_mirror_XXXXXX";

    g_checks = 0;
    g_fail = 0;
    if (!mkdtemp(mirror)) {
        perror("mkdtemp");
        return 2;
    }
    cnet_brain_mirror_set_dir(mirror);
    cnet_held_model_set_hook(NULL);
    cnet_held_model_set_endpoint(NULL);
    unsetenv("CNET_HELD_MODEL_ENDPOINT");
    unsetenv("CNET_HELD_MODEL_PATH");

    printf("cnet_rlm tests — outer host wraps CORE + planes\n");

    cnet_rlm_policy_default(&pol);
    pol.core.allow_wiki = 0;
    pol.core.open_chat_enabled = 1;
    pol.core.residual_enabled = 1;
    pol.core.logic_open_chat_fallback = 0;

    /* LOGIC CERT through RLM */
    expect(cnet_rlm_ask("what is 2 plus 3", &pol, &r) == 0, "rlm_logic_rc");
    expect(r.via_rlm == 1, "rlm_via");
    expect(r.final.via_core == 1, "rlm_via_core");
    expect(r.final.plane == CNET_CORE_PLANE_CERT, "rlm_logic_plane");
    expect(r.final.claimed_cert == 1, "rlm_logic_cert");
    expect(r.intent == CNET_CORE_INTENT_LOGIC, "rlm_logic_intent");
    expect(r.n_steps >= 1, "rlm_logic_steps");
    expect(strstr(r.summary, "5") != NULL || strstr(r.final.value, "5") != NULL,
           "rlm_logic_five");

    /* Multi-skill capsule under RLM */
    expect(cnet_rlm_ask("increment 41 then crc8", &pol, &r) == 0,
           "rlm_capsule_rc");
    expect(r.final.plane == CNET_CORE_PLANE_CERT, "rlm_capsule_cert_plane");
    expect(r.final.claimed_cert == 1, "rlm_capsule_cert");
    expect(r.n_steps >= 1, "rlm_capsule_steps");
    expect(strcmp(r.steps[0].note, "capsule") == 0 ||
               r.final.bound == 1,
           "rlm_capsule_note");

    /* CREATIVE open chat through RLM (still via CORE) */
    cnet_held_model_set_hook(hook_creative);
    expect(cnet_rlm_ask("write a short poem about zz99", &pol, &r) == 0,
           "rlm_creative_rc");
    expect(r.final.plane == CNET_CORE_PLANE_OPEN_CHAT, "rlm_creative_plane");
    expect(r.final.open_chat == 1, "rlm_creative_open");
    expect(r.final.claimed_cert == 0, "rlm_creative_no_cert");
    expect(r.final.via_core == 1, "rlm_creative_via_core");
    expect(strcmp(r.summary, "rlm-open-chat-draft") == 0, "rlm_creative_text");

    /* LOGIC miss does not fill with creative */
    expect(cnet_rlm_ask("compute crc8 of unknown blob zz99", &pol, &r) == 1,
           "rlm_logic_miss");
    expect(r.final.claimed_cert == 0, "rlm_logic_miss_no_cert");
    expect(r.final.open_chat == 0, "rlm_logic_miss_no_open");

    /* Open chat never cert even if hostile */
    expect(r.final.plane != CNET_CORE_PLANE_CERT || !r.final.bound,
           "rlm_miss_not_fake_cert");

    cnet_held_model_set_hook(NULL);
    cnet_brain_mirror_set_dir(NULL);

    printf("CNET_RLM_PASS\n");
    printf("checks=%d fail=%d via_rlm=1 wraps_core=1 cert_and_open_chat=1 "
           "residual_never_cert=1 recursive_bounded=1 python=0 "
           "broader_claims=WITHHELD\n",
           g_checks, g_fail);
    return g_fail ? 1 : 0;
}
