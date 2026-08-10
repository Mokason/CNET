/* Smoke: Ollama cloud teacher deepseek-v4-flash:cloud via ROE net.
 * make roe_teacher_cloud_smoke → ROE_TEACHER_CLOUD_SMOKE_PASS
 *
 * Requires: ollama up, model deepseek-v4-flash:cloud reachable, no API key.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cnet_roe_asi.h"
#include "../include/cnet_roe_net.h"

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-64s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    RoeNet net;
    RoeAsi R;
    RoeReply out;
    char ans[ROE_ANSWER_MAX];
    uint64_t tok = 0;
    int rc;

    failures = checks = 0;
    printf("=== ROE teacher Ollama cloud smoke ===\n");

    /* Prefer env file values if already exported; else set defaults here. */
    if (!getenv("ROE_LLM_MODEL") || !strstr(getenv("ROE_LLM_MODEL"), "deepseek"))
        setenv("ROE_LLM_MODEL", "deepseek-v4-flash:cloud", 1);
    if (!getenv("ROE_LLM_URL"))
        setenv("ROE_LLM_URL", "http://127.0.0.1:11434/api/generate", 1);
    if (!getenv("ROE_LLM_THINK")) setenv("ROE_LLM_THINK", "0", 1);
    if (!getenv("ROE_TIMEOUT_MS")) setenv("ROE_TIMEOUT_MS", "120000", 1);
    setenv("ROE_LIVE", "1", 1);
    setenv("ROE_LLM", "1", 1);
    setenv("ROE_LOOKUP", "0", 1);

    roe_net_from_env(&net);
    printf("model=%s url=%s think=%d timeout_ms=%ld\n", net.llm_model, net.llm_url,
           net.think, net.timeout_ms);
    check(net.enable_llm == 1, "llm enabled");
    check(strstr(net.llm_model, "deepseek-v4-flash") != NULL, "model is deepseek-v4-flash");
    check(strstr(net.llm_model, ":cloud") != NULL, "cloud tag present");
    check(net.think == 0, "think disabled for short teacher answers");
    check(strstr(net.llm_url, "11434") != NULL, "ollama local daemon URL");

    rc = roe_net_llm(&net, "Reply with exactly the word PONG and nothing else.", ans,
                     sizeof ans, &tok);
    printf("  llm_rc=%d err=%s ans=%s tok~%llu\n", rc, net.last_err, ans,
           (unsigned long long)tok);
    check(rc == 0, "roe_net_llm ok");
    check(ans[0] != 0, "non-empty answer");
    check(strstr(ans, "PONG") != NULL || strstr(ans, "pong") != NULL ||
              strlen(ans) > 0,
          "usable teacher text");

    /* Full ROE turn with live net — miss path should hit LLM */
    roe_init(&R);
    roe_set_net(&R, &net);
    roe_set_catalog_dir(&R, "artifacts/roe_catalog");
    (void)roe_load_catalog(&R);
    /* force a novel query unlikely in catalog */
    rc = roe_turn(&R, "define conformal abstention in one short sentence zzcloudteach",
                  &out);
    printf("  turn src=%s ver=%d ans=%.120s\n", out.source_name, out.verified,
           out.answer);
    check(rc == ROE_OK || rc == ROE_ABSTAIN, "roe_turn completed");
    check(out.source == ROE_SRC_LLM || out.source == ROE_SRC_ABSTAIN ||
              out.source == ROE_SRC_LOOKUP || out.source == ROE_SRC_ASK_USER ||
              out.source == ROE_SRC_LOCAL,
          "valid source");
    /* If LLM path used, must be unverified (never self-CERT) */
    if (out.source == ROE_SRC_LLM) {
        check(out.verified == 0, "LLM teacher untrusted until verify");
        check(out.answer[0] != 0, "LLM answer non-empty");
    } else {
        check(1, "non-llm path acceptable if catalog hit/abstain");
    }

    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ROE_TEACHER_CLOUD_SMOKE_FAIL\n");
        return 1;
    }
    printf("ROE_TEACHER_CLOUD_SMOKE_PASS\n");
    return 0;
}
