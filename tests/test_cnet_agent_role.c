/*
 * test_cnet_agent_role.c — hermetic gate for explicit agent roles.
 *
 * make agent_role → AGENT_ROLE_PASS
 */
#include "../include/cnet_agent_role.h"
#include "../include/cnet_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    CnetAgentRole role = (CnetAgentRole)99;
    CnetAgentRolePolicy pol;
    char *sys = NULL;

    printf("== cnet_agent_role ==\n");

    check(cnet_agent_role_parse(NULL, &role) != 0, "parse NULL fails");
    check(cnet_agent_role_parse("", &role) != 0, "parse empty fails");
    check(cnet_agent_role_parse("planner", &role) != 0, "parse unknown fails");
    check(cnet_agent_role_is_known("planner") == 0, "unknown not known");

    check(cnet_agent_role_parse("auditor", &role) == 0 &&
              role == CNET_AGENT_ROLE_AUDITOR,
          "parse auditor");
    check(cnet_agent_role_parse("AUDITOR", &role) == 0 &&
              role == CNET_AGENT_ROLE_AUDITOR,
          "parse AUDITOR case-insensitive");
    check(cnet_agent_role_parse("researcher", &role) == 0 &&
              role == CNET_AGENT_ROLE_RESEARCHER,
          "parse researcher");
    check(cnet_agent_role_parse("coder", &role) == 0 &&
              role == CNET_AGENT_ROLE_CODER,
          "parse coder");
    check(cnet_agent_role_parse("critic", &role) == 0 &&
              role == CNET_AGENT_ROLE_CRITIC,
          "parse critic");
    check(cnet_agent_role_parse("memory-witness", &role) == 0 &&
              role == CNET_AGENT_ROLE_MEMORY_WITNESS,
          "parse memory-witness");
    check(cnet_agent_role_parse("memory_witness", &role) == 0 &&
              role == CNET_AGENT_ROLE_MEMORY_WITNESS,
          "alias memory_witness");
    check(cnet_agent_role_parse("witness", &role) == 0 &&
              role == CNET_AGENT_ROLE_MEMORY_WITNESS,
          "alias witness");

    check(strcmp(cnet_agent_role_name(CNET_AGENT_ROLE_AUDITOR), "auditor") == 0,
          "name auditor");
    check(strcmp(cnet_agent_role_name(CNET_AGENT_ROLE_MEMORY_WITNESS),
                 "memory-witness") == 0,
          "name memory-witness");
    check(strcmp(cnet_agent_role_name((CnetAgentRole)99), "unknown") == 0,
          "name unknown");

    check(cnet_agent_role_resolve("Critic", &pol) == 0 &&
              pol.role == CNET_AGENT_ROLE_CRITIC &&
              strcmp(pol.name, "critic") == 0,
          "resolve Critic");
    check(pol.preferred_sampling == CNET_AGENT_SAMPLE_FOCUSED,
          "critic preferred FOCUSED");
    check((pol.capabilities & CNET_AGENT_CAP_SEAL) == 0,
          "critic has no SEAL");
    check(pol.system_fragment && strstr(pol.system_fragment, "critic"),
          "critic system fragment");

    check(cnet_agent_role_resolve("auditor", &pol) == 0 &&
              pol.preferred_sampling == CNET_AGENT_SAMPLE_DETERMINISTIC,
          "auditor DETERMINISTIC");
    check((pol.capabilities & CNET_AGENT_CAP_CERTIFY) != 0,
          "auditor CERTIFY");
    check((pol.capabilities & CNET_AGENT_CAP_SEAL) == 0,
          "auditor no SEAL");

    check(cnet_agent_role_resolve("researcher", &pol) == 0 &&
              pol.preferred_sampling == CNET_AGENT_SAMPLE_BALANCED,
          "researcher BALANCED");
    check((pol.capabilities & CNET_AGENT_CAP_SEARCH) != 0,
          "researcher SEARCH");

    check(cnet_agent_role_resolve("coder", &pol) == 0 &&
              (pol.capabilities & CNET_AGENT_CAP_CODE) != 0 &&
              (pol.capabilities & CNET_AGENT_CAP_WRITE) != 0,
          "coder CODE|WRITE");

    check(cnet_agent_role_resolve("memory-witness", &pol) == 0 &&
              pol.preferred_sampling == CNET_AGENT_SAMPLE_DETERMINISTIC &&
              (pol.capabilities & CNET_AGENT_CAP_RECALL) != 0 &&
              (pol.capabilities & CNET_AGENT_CAP_WRITE) == 0,
          "memory-witness recall-only stance");

    /* Sampling enum alignment with harness */
    check((int)CNET_AGENT_SAMPLE_DETERMINISTIC ==
              (int)CNET_HARNESS_SAMPLING_DETERMINISTIC,
          "sample DET aligns harness");
    check((int)CNET_AGENT_SAMPLE_FOCUSED ==
              (int)CNET_HARNESS_SAMPLING_FOCUSED,
          "sample FOC aligns harness");
    check((int)CNET_AGENT_SAMPLE_BALANCED ==
              (int)CNET_HARNESS_SAMPLING_BALANCED,
          "sample BAL aligns harness");
    check((int)CNET_AGENT_SAMPLE_EXPLORATORY ==
              (int)CNET_HARNESS_SAMPLING_EXPLORATORY,
          "sample EXP aligns harness");

    check(cnet_agent_role_policy(CNET_AGENT_ROLE_AUDITOR, &pol) == 0,
          "policy by enum");
    check(cnet_agent_role_policy((CnetAgentRole)99, &pol) != 0,
          "policy bad enum fails");
    check(cnet_agent_role_policy(CNET_AGENT_ROLE_AUDITOR, NULL) != 0,
          "policy NULL fails");

    check(cnet_agent_role_caps_valid(CNET_AGENT_CAP_INSPECT |
                                     CNET_AGENT_CAP_RECALL),
          "caps valid subset");
    check(!cnet_agent_role_caps_valid(1u << 31), "caps invalid high bit");

    /* No role defaults to SEAL */
    {
        int i;
        int any_seal = 0;
        for (i = 0; i < CNET_AGENT_ROLE_COUNT; ++i) {
            check(cnet_agent_role_policy((CnetAgentRole)i, &pol) == 0,
                  "policy all roles");
            if (pol.capabilities & CNET_AGENT_CAP_SEAL) any_seal = 1;
            check(pol.name && pol.system_fragment,
                  "policy non-null fields");
            check(cnet_agent_role_caps_valid(pol.capabilities),
                  "policy caps valid");
        }
        check(!any_seal, "no default role has SEAL");
    }

    check(cnet_agent_role_resolve("auditor", &pol) == 0, "compose setup");
    sys = cnet_agent_role_compose_system(&pol, NULL);
    check(sys && strstr(sys, "auditor") && !strstr(sys, "extra"),
          "compose fragment only");
    free(sys);
    sys = cnet_agent_role_compose_system(&pol, "extra instruction");
    check(sys && strstr(sys, "auditor") && strstr(sys, "extra instruction"),
          "compose fragment + caller");
    free(sys);
    check(cnet_agent_role_compose_system(NULL, "x") == NULL,
          "compose NULL policy fails");

    if (failures) {
        printf("AGENT_ROLE_FAIL failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("AGENT_ROLE_PASS checks=%d\n", checks);
    return 0;
}
