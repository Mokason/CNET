/* ROE-ASI concept gate + train bench. Marker: ROE_ASI_PASS */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cnet_roe_asi.h"

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-64s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void seed_world(RoeAsi *R) {
    roe_init(R);
    /* Certified local skills (day-0) */
    check(roe_add_skill(R, "greet", "greet", "hello",
                        "Hello. I am ROE-ASI (local skill).", 0, 1) == 0,
          "seed greet");
    check(roe_add_skill(R, "identity", "identity", "who are you",
                        "ROE-ASI: regulated open-ended specialized assistant.", 0, 1) ==
              0,
          "seed identity");
    check(roe_add_skill(R, "add2", "math_add", "what is 2+2", "4", 0, 1) == 0,
          "seed math");

    /* Cheap lookup corpus (not CERT) */
    check(roe_add_lookup(R, "cnet capsule",
                         "CNET capsule = portable CERT unit with coverage gate.") == 0,
          "lookup cnet");
    check(roe_add_lookup(R, "fail closed",
                         "Fail-closed means abstain outside certified coverage.") == 0,
          "lookup fail-closed");

    /* External teacher curriculum (mock LLM) — untrusted until verify+promote */
    check(roe_add_teach(R, "what is roe-asi", "roe_def",
                        "ROE-ASI is a shell-regulated specialist assistant that "
                        "serves local skills and escalates misses.") == 0,
          "teach roe");
    check(roe_add_teach(R, "token cost", "token_cost",
                        "Local hits cost ~0 tokens; LLM miss path is expensive.") == 0,
          "teach tokens");
    check(roe_add_teach(R, "promote skill", "promote",
                        "Misses become local skills only after verify votes.") == 0,
          "teach promote");
}

int main(void) {
  /* HEAP, NOT STACK: this struct exceeds the 2 MB MinGW stack reserve
   * (RoeAsi 3.01 MB, RoeDebug 4.15 MB, RoeOcr 6.15 MB since ROE_ANSWER_MAX
   * went 512 -> 4096 in 85c433e and is embedded 640x). A stack instance
   * dies inside ___chkstk_ms in the prologue, before any statement runs. */
    RoeAsi *R = (RoeAsi *)calloc(1, sizeof *R);
    RoeReply rep;
    RoeTrainReport tr;
    char stats[512];
    const char *epoch1[] = {
        "hello there",
        "who are you",
        "what is 2+2",
        "what is a cnet capsule",
        "what is roe-asi please",
        "how does token cost work",
        "explain promote skill path",
        "what is roe-asi please", /* 2nd vote */
        "how does token cost work",
        "explain promote skill path",
        "hello again",
        "what is roe-asi please", /* should be local after promote */
        "totally unknown xyzzy quest",
    };
    const char *epoch2[] = {
        "hello",
        "who are you",
        "what is 2+2",
        "what is roe-asi please",
        "how does token cost work",
        "explain promote skill path",
        "what is a cnet capsule",
        "fail closed meaning",
        "hello",
        "what is roe-asi please",
        "how does token cost work",
        "explain promote skill path",
        "what is 2+2",
        "who are you",
        "hello",
        "what is roe-asi please",
        "how does token cost work",
        "explain promote skill path",
        "what is a cnet capsule",
        "fail closed meaning",
    };
    int n1 = (int)(sizeof epoch1 / sizeof epoch1[0]);
    int n2 = (int)(sizeof epoch2 / sizeof epoch2[0]);
    size_t skills_before, skills_after;

    failures = checks = 0;
    printf("=== ROE-ASI concept build + train ===\n");
    seed_world(R);
    skills_before = R->n_skills;

    /* Unit: local hit */
    check(roe_turn(R, "hello", &rep) == ROE_OK, "turn hello ok");
    check(rep.source == ROE_SRC_LOCAL && rep.verified == 1, "hello local verified");
    check(rep.tokens_est == 0, "local tokens 0");

    /* Lookup untrusted */
    check(roe_turn(R, "tell me about cnet capsule design", &rep) == ROE_OK,
          "lookup turn");
    check(rep.source == ROE_SRC_LOOKUP && rep.verified == 0, "lookup untrusted");

    /* LLM miss */
    check(roe_turn(R, "what is roe-asi please", &rep) == ROE_OK, "llm turn");
    check(rep.source == ROE_SRC_LLM && rep.verified == 0, "llm untrusted");
    check(R->n_llm_call >= 1, "llm counted");

    /* Verify once — not enough votes */
    check(roe_feedback_verify(R, "what is roe-asi please", NULL, 0) == 0,
          "first verify no promote yet");
    check(roe_turn(R, "what is roe-asi please", &rep) == ROE_OK, "llm again");
    check(roe_feedback_verify(R, "what is roe-asi please", NULL, 0) == 1,
          "second verify promotes");
    check(R->n_promote >= 1, "promote stat");
    check(roe_turn(R, "what is roe-asi please", &rep) == ROE_OK, "after promote");
    check(rep.source == ROE_SRC_LOCAL, "promoted skill now local");

    /* True OOD abstain */
    check(roe_turn(R, "xyzzy unknown foobar", &rep) == ROE_ABSTAIN, "ood abstain");
    check(rep.source == ROE_SRC_ASK_USER, "ask user source");

    /* Full train epochs */
    printf("\n-- train epoch 1 (bootstrap) --\n");
    roe_reset_stats(R);
    roe_train_epoch(R, epoch1, n1, &tr);
    printf("  turns=%d promotes=%d hit=%.3f save=%.3f tok=%llu base=%llu\n",
           tr.turns, tr.promotes, tr.local_hit_rate, tr.token_save_ratio,
           (unsigned long long)tr.tokens_used,
           (unsigned long long)tr.tokens_baseline);

    printf("\n-- train epoch 2 (after learning) --\n");
    roe_reset_stats(R);
    roe_train_epoch(R, epoch2, n2, &tr);
    printf("  turns=%d promotes=%d hit=%.3f save=%.3f tok=%llu base=%llu\n", tr.turns,
           tr.promotes, tr.local_hit_rate, tr.token_save_ratio,
           (unsigned long long)tr.tokens_used, (unsigned long long)tr.tokens_baseline);
    check(tr.local_hit_rate >= 0.40, "epoch2 local hit >= 40%");
    check(tr.token_save_ratio >= 0.30, "epoch2 token save >= 30%");
    check(tr.tokens_used < tr.tokens_baseline, "used < baseline LLM");

    skills_after = R->n_skills;
    check(skills_after >= skills_before, "skills grew or held");

    roe_dump_stats(R, stats, sizeof stats);
    printf("\n  final: %s\n", stats);

    /* Shell law: LLM answer never auto-CERT without votes — fresh instance */
    {
        RoeAsi *R2 = (RoeAsi *)calloc(1, sizeof *R2);
        RoeReply r2;
        seed_world(R2);
        (void)roe_turn(R2, "what is roe-asi please", &r2);
        check(r2.source == ROE_SRC_LLM, "fresh still llm");
        {
            size_t i;
            int cert_roe = 0;
            for (i = 0; i < R2->n_skills; i++)
                if (strstr(R2->skills[i].id, "roe") && R2->skills[i].certified)
                    cert_roe = 1;
            check(!cert_roe, "no silent CERT from one llm turn");
        }
    }

    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ROE_ASI_FAIL\n");
        return 1;
    }
    printf("ROE_ASI_PASS\n");
    return 0;
}
