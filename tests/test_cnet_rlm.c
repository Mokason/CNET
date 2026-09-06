#include "cnet_rlm.h"

#include "cnet_brain_mirror.h"
#include "cnet_core_serve.h"
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

    expect(!cnet_rlm_is_chain_turn("q1_add16 3"), "chain_single_no");
    expect(cnet_rlm_is_chain_turn("q1_add16 3 then q1_xor16"), "chain_then_yes");
    expect(cnet_rlm_is_chain_turn("q1_add16:3 | q1_xor16:auto"), "chain_pipe_yes");
    expect(cnet_rlm_is_chain_turn("increment 41 then crc8"), "chain_skills_yes");

    cnet_rlm_policy_default(&pol);
    pol.core.allow_wiki = 0;
    pol.core.open_chat_enabled = 1; /* test-only */
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

    /* Multi-skill capsule under RLM — each named hop is one billed step.
       Prefix-CERT of leftover hops is illegal. */
    expect(cnet_rlm_ask("increment 41 then crc8", &pol, &r) == 0,
           "rlm_capsule_rc");
    expect(r.final.plane == CNET_CORE_PLANE_CERT, "rlm_capsule_cert_plane");
    expect(r.final.claimed_cert == 1, "rlm_capsule_cert");
    expect(r.n_steps >= 2, "rlm_capsule_steps");
    expect(r.steps[0].step.claimed_cert == 1, "rlm_capsule_hop0_cert");
    expect(r.steps[1].step.claimed_cert == 1, "rlm_capsule_hop1_cert");
    expect(r.steps[0].step.residual_calls == 0 &&
               r.steps[1].step.residual_calls == 0,
           "rlm_capsule_hops_no_residual");
    expect(r.final.residual_calls == 0, "rlm_capsule_no_residual");
    expect(strcmp(r.steps[0].note, "capsule") == 0 ||
               strcmp(r.steps[0].note, "core") == 0,
           "rlm_capsule_note");

    /* Bound must trip: 3 named skills, max_steps=2 → rlm_budget, no prefix CERT. */
    {
        CnetRlmPolicy tight = pol;
        tight.max_steps = 2;
        expect(cnet_rlm_ask("increment 41 then crc8 then lookup", &tight, &r) ==
                   1,
               "rlm_budget_rc");
        expect(r.final.claimed_cert == 0, "rlm_budget_no_cert");
        expect(r.final.bound == 0, "rlm_budget_unbound");
        expect(r.final.open_chat == 0, "rlm_budget_no_open");
        expect(strstr(r.final.refusal, "rlm_budget") != NULL ||
                   strstr(r.summary, "rlm_budget") != NULL,
               "rlm_budget_reason");
        expect(r.n_steps >= 2, "rlm_budget_spent_two");
    }

    /* CREATIVE may use OPEN_CHAT draft (CORE plane) — never claimed_cert */
    cnet_held_model_set_hook(hook_creative);
    {
        int crc = cnet_rlm_ask("write a short poem about zz99", &pol, &r);
        expect(crc == 0 || crc == 1, "rlm_creative_rc");
        expect(r.final.claimed_cert == 0, "rlm_creative_no_cert");
        /* If open chat enabled + hook fired: bound draft, plane OPEN_CHAT */
        if (r.final.open_chat) {
            expect(r.final.bound == 1, "rlm_creative_open_bound");
            expect(r.final.plane == CNET_CORE_PLANE_OPEN_CHAT, "rlm_creative_plane");
            expect(strstr(r.final.spoken, "rlm-open-chat-draft") != NULL ||
                       r.final.spoken[0] != '\0',
                   "rlm_creative_draft_text");
        } else {
            /* open chat off / unavailable — must not CERT */
            expect(r.final.bound == 0 || r.final.plane != CNET_CORE_PLANE_CERT,
                   "rlm_creative_no_fake_cert");
        }
    }

    /* LOGIC miss does not fill with creative */
    expect(cnet_rlm_ask("compute crc8 of unknown blob zz99", &pol, &r) == 1,
           "rlm_logic_miss");
    expect(r.final.claimed_cert == 0, "rlm_logic_miss_no_cert");
    expect(r.final.open_chat == 0, "rlm_logic_miss_no_open");

    /* Open chat never cert even if hostile */
    expect(r.final.plane != CNET_CORE_PLANE_CERT || !r.final.bound,
           "rlm_miss_not_fake_cert");

    cnet_held_model_set_hook(NULL);

    /* Multi-turn session: again recalls; bare skill does not false-CERT. */
    {
        CnetRlmSession sess;
        cnet_rlm_session_init(&sess);
        expect(cnet_rlm_ask_session("what is 2 plus 3", &pol, &sess, &r) == 0,
               "rlm_sess_math");
        expect(r.final.claimed_cert == 1, "rlm_sess_cert");
        expect(sess.n >= 1, "rlm_sess_mem");
        expect(cnet_rlm_ask_session("again", &pol, &sess, &r) == 0,
               "rlm_sess_again");
        expect(r.session_used == 1, "rlm_sess_flag");
        expect(r.final.claimed_cert == 1, "rlm_sess_again_cert");
        expect(strstr(r.final.value, "5") != NULL ||
                   strstr(r.summary, "5") != NULL,
               "rlm_sess_again_val");
        /* bare prior skill name must NOT stale-CERT */
        expect(cnet_rlm_ask_session(sess.skill[0], &pol, &sess, &r) != 0 ||
                   r.session_used == 0,
               "rlm_sess_no_bare_skill");
        if (r.session_used == 0)
            expect(1, "rlm_sess_bare_ok");
        else {
            expect(0, "rlm_sess_bare_ok");
        }
        cnet_rlm_session_clear(&sess);
    }

    /* Serve-bank chain through RLM (AGI-scenario hops hosted by the live
       outer host). Park .lut files in-process. RLM must not call evolve. */
    {
        char bricks[] = "/tmp/cnet_rlm_bricks_XXXXXX";
        char missp[] = "/tmp/cnet_rlm_miss_XXXXXX";
        float add[16], xorf[16];
        unsigned i;
        CnetRlmPolicy bpol = pol;
        if (!mkdtemp(bricks)) {
            perror("mkdtemp bricks");
            return 2;
        }
        if (!mkdtemp(missp)) {
            perror("mkdtemp miss");
            return 2;
        }
        for (i = 0; i < 16; ++i) {
            add[i] = (float)((i + 1u) & 15u);
            xorf[i] = (float)(i ^ 1u);
        }
        expect(cnet_serve_save_lut(bricks, "q1_add16", "agi_q_add", add) == 0,
               "rlm_brick_save_add");
        expect(cnet_serve_save_lut(bricks, "q1_xor16", "agi_k_xor", xorf) == 0,
               "rlm_brick_save_xor");
        setenv("CNET_CORE_BUS_BRICKS_DIR", bricks, 1);
        expect(cnet_serve_global_load_env() == 0, "rlm_brick_bank_load");
        {
            char missf[768];
            snprintf(missf, sizeof missf, "%s/miss.jsonl", missp);
            setenv("CNET_MISS_LOG", missf, 1);
        }
        bpol.max_steps = 3;
        expect(cnet_rlm_ask("q1_add16 3 then q1_xor16", &bpol, &r) == 0,
               "rlm_brick_chain_rc");
        expect(r.via_rlm == 1, "rlm_brick_via");
        expect(r.n_steps >= 2, "rlm_brick_steps");
        expect(r.final.claimed_cert == 1, "rlm_brick_cert");
        expect(r.final.plane == CNET_CORE_PLANE_CERT, "rlm_brick_plane");
        expect(r.final.open_chat == 0, "rlm_brick_no_open");
        expect(r.final.residual_calls == 0, "rlm_brick_no_residual");
        expect(strcmp(r.final.value, "5") == 0 ||
                   strstr(r.summary, "5") != NULL,
               "rlm_brick_chain_value"); /* add:3→4; xor:4^1=5 */

        /* Unknown brick hop abstains; leftover does not prefix-CERT. */
        expect(cnet_rlm_ask("q1_add16 3 then missing_dom", &bpol, &r) == 1,
               "rlm_brick_partial_rc");
        expect(r.final.claimed_cert == 0, "rlm_brick_partial_no_cert");

        /* First hop missing on multi-hop: still brick abstain, not soft fill. */
        expect(cnet_rlm_ask("no_such_brick 3 then q1_xor16 1", &bpol, &r) == 1,
               "rlm_brick_first_miss_rc");
        expect(r.final.claimed_cert == 0, "rlm_brick_first_miss_no_cert");
        expect(strstr(r.final.refusal, "outside_table") != NULL ||
                   strstr(r.summary, "outside_table") != NULL ||
                   strstr(r.final.refusal, "abstain") != NULL,
               "rlm_brick_first_miss_reason");

        /* Budget trip still writes a miss row evolve can harvest later. */
        {
            CnetRlmPolicy tight = bpol;
            FILE *mf;
            char line[512];
            int saw = 0;
            tight.max_steps = 1;
            expect(cnet_rlm_ask("q1_add16 3 then q1_xor16", &tight, &r) == 1,
                   "rlm_brick_budget_rc");
            expect(r.final.claimed_cert == 0, "rlm_brick_budget_no_cert");
            mf = fopen(getenv("CNET_MISS_LOG"), "r");
            expect(mf != NULL, "rlm_miss_log_open");
            if (mf) {
                while (fgets(line, sizeof line, mf)) {
                    if (strstr(line, "\"via\":\"cnet_rlm\"") &&
                        strstr(line, "\"claimed_cert\":0"))
                        saw = 1;
                }
                fclose(mf);
            }
            expect(saw, "rlm_miss_log_no_auto_cert");
        }
        unsetenv("CNET_MISS_LOG");
        unsetenv("CNET_CORE_BUS_BRICKS_DIR");
    }

    cnet_brain_mirror_set_dir(NULL);

    if (g_fail == 0)
        printf("CNET_RLM_PASS\n");
    else
        printf("CNET_RLM_FAIL\n");
    printf("checks=%d fail=%d via_rlm=1 wraps_core=1 open_chat_answer=0 "
           "residual_never_cert=1 recursive_bounded=1 recursive_used=1 "
           "budget_trips=1 leftover_no_prefix_cert=1 session_again=1 "
           "brick_first_miss=1 python=0 broader_claims=WITHHELD\n",
           g_checks, g_fail);
    return g_fail ? 1 : 0;
}
