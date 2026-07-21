/*
 * test_cnet_route_log.c — hermetic gate for route decision logging.
 * make route_log → ROUTE_LOG_PASS
 */
#include "../include/cnet_route_log.h"
#include "../include/cnet_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    char buf[1024];
    char path[] = "tmp_route_log_XXXXXX";
    int fd;
    CnetRouteLogEvent ev;
    FILE *f;
    char line[1200];

    printf("== cnet_route_log ==\n");

    check(cnet_route_log_cost(0, 0) == 1.0, "cost decision only = 1");
    check(cnet_route_log_cost(10, 5) == 16.0, "cost = 1 + tokens");
    check(strcmp(cnet_route_log_profile_name(CNET_HARNESS_SAMPLING_FOCUSED),
                 "focused") == 0,
          "profile focused");
    check(strcmp(cnet_route_log_outcome_name(CNET_HARNESS_OK), "ok") == 0,
          "outcome ok");
    check(strcmp(cnet_route_log_outcome_name(CNET_HARNESS_ERR_BACKEND),
                 "err_backend") == 0,
          "outcome err_backend");

    memset(&ev, 0, sizeof ev);
    ev.event = "generate";
    ev.mechanism = CNET_ROUTE_MECH_AGENT_ROLE;
    ev.role = "coder";
    ev.role_canonical = "coder";
    ev.selected_expert = 2;
    ev.expert_profile = "focused";
    ev.entropy = 0.25f;
    ev.outcome = "ok";
    ev.outcome_code = 0;
    ev.route_latency_ms = 0.5;
    ev.total_latency_ms = 12.5;
    ev.cost = cnet_route_log_cost(100, 40);
    ev.prompt_tokens = 100;
    ev.generated_tokens = 40;
    ev.aicimo_override = 0;
    ev.model_id = "demo";

    check(cnet_route_log_format_json(&ev, buf, sizeof buf) == 0, "format ok");
    check(strstr(buf, "\"mechanism\":\"aicimo_agent_role\"") != NULL,
          "json mechanism");
    check(strstr(buf, "\"selected_expert\":2") != NULL, "json selected_expert");
    check(strstr(buf, "\"selected_unit\":\"\"") != NULL ||
              strstr(buf, "\"selected_unit\":") != NULL,
          "json selected_unit present");
    check(strstr(buf, "\"plan_length\":0") != NULL, "json plan_length");
    check(strstr(buf, "\"entropy\":0.250000") != NULL, "json entropy");
    /* Core CNET planner shape */
    {
        CnetRouteLogEvent pev;
        char pbuf[1024];
        memset(&pev, 0, sizeof pev);
        pev.event = "soul_route";
        pev.mechanism = CNET_ROUTE_MECH_PLANNER;
        pev.role = "unified_goal";
        pev.role_canonical = "unified_goal";
        pev.selected_expert = 1;
        pev.selected_unit = "acq_unified_goal";
        pev.plan_length = 1;
        pev.expert_profile = "certified";
        pev.entropy = 0.1f;
        pev.outcome = "ok";
        pev.outcome_code = 2;
        pev.route_latency_ms = 0.2;
        pev.total_latency_ms = 0.8;
        pev.cost = 5.0;
        check(cnet_route_log_format_json(&pev, pbuf, sizeof pbuf) == 0,
              "format planner event");
        check(strstr(pbuf, "\"mechanism\":\"planner\"") != NULL,
              "planner mechanism");
        check(strstr(pbuf, "\"selected_unit\":\"acq_unified_goal\"") != NULL,
              "planner selected_unit");
        check(strstr(pbuf, "\"plan_length\":1") != NULL, "planner plan_length");
    }
    check(strstr(buf, "\"outcome\":\"ok\"") != NULL, "json outcome");
    check(strstr(buf, "\"route_latency_ms\":0.500") != NULL, "json route_latency");
    check(strstr(buf, "\"total_latency_ms\":12.500") != NULL, "json total_latency");
    check(strstr(buf, "\"cost\":141.000") != NULL, "json cost");
    check(strstr(buf, "\"role\":\"coder\"") != NULL, "json role");

    /* Escape quotes in role */
    ev.role = "weird\"role";
    check(cnet_route_log_format_json(&ev, buf, sizeof buf) == 0,
          "format escaped role");
    check(strstr(buf, "weird\\\"role") != NULL, "escaped quote present");

    fd = mkstemp(path);
    check(fd >= 0, "mkstemp");
    if (fd >= 0) close(fd);
    unlink(path); /* append creates */

    ev.role = "coder";
    check(cnet_route_log_append(path, &ev) == 0, "append 1");
    check(cnet_route_log_append(path, &ev) == 0, "append 2");

    f = fopen(path, "r");
    check(f != NULL, "open log");
    if (f) {
        int lines = 0;
        while (fgets(line, sizeof line, f)) {
            lines++;
            check(strstr(line, "\"ts\":") != NULL, "line has ts");
            check(strstr(line, "selected_expert") != NULL, "line has expert");
            check(strstr(line, "entropy") != NULL, "line has entropy");
            check(strstr(line, "outcome") != NULL, "line has outcome");
            check(strstr(line, "latency") != NULL, "line has latency");
            check(strstr(line, "\"cost\":") != NULL, "line has cost");
        }
        fclose(f);
        check(lines == 2, "two JSONL lines");
    }

    check(cnet_route_log_append(NULL, &ev) != 0, "append NULL path fails");
    check(cnet_route_log_append(path, NULL) != 0, "append NULL event fails");
    check(cnet_route_log_format_json(NULL, buf, sizeof buf) != 0,
          "format NULL fails");

    unlink(path);

    if (failures) {
        printf("ROUTE_LOG_FAIL failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("ROUTE_LOG_PASS checks=%d\n", checks);
    return 0;
}
