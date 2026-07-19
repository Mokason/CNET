/* Hermes-style C learn loop gate.
 * make learn_loop → LEARN_LOOP_PASS
 */
#include <stdio.h>
#include "../include/cnet_platform.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/cnet_learn_loop.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    CnetLearnConfig cfg;
    CnetLearnReport rep;
    const char *skills = "logs/test_learn_skills";
    char names[16][128];
    int n;

    cnet_setenv("CNET_SKILLS_DIR", skills, 1);
    /* Isolate MCP fact file if possible — tools may still write cwd facts. */
    printf("== cnet learn loop (Hermes-style in C) ==\n");

    cnet_learn_config_defaults(&cfg);
    snprintf(cfg.skills_dir, sizeof cfg.skills_dir, "%s", skills);
    cfg.use_tools = 1;
    cfg.write_skill = 1;
    cfg.memorize = 1;
    cfg.seed_gap = 0;

    /* Live lookup learn */
    memset(&rep, 0, sizeof rep);
    check(cnet_learn_cycle("Grace Hopper", &cfg, &rep) == 0, "learn Grace Hopper");
    check(rep.source == CNET_LEARN_SRC_WIKI || rep.source == CNET_LEARN_SRC_WEB ||
              rep.source == CNET_LEARN_SRC_MEMORY || rep.source == CNET_LEARN_SRC_SKILL,
          "source is tool/memory/skill");
    check(rep.answer[0] != '\0' && strstr(rep.answer, "LOOKUP_FAILED") == NULL,
          "answer non-empty");
    check(rep.memorized == 1 || rep.skill_reused == 1 || rep.source == CNET_LEARN_SRC_MEMORY,
          "memorized or already known");
    check(rep.skill_written == 1 || rep.skill_reused == 1 || access(rep.skill_path, R_OK) == 0,
          "skill written or present");

    /* Second call should hit skill or memory (no need to re-fetch). */
    memset(&rep, 0, sizeof rep);
    check(cnet_learn_cycle("Grace Hopper", &cfg, &rep) == 0, "reuse Grace Hopper");
    check(rep.source == CNET_LEARN_SRC_SKILL || rep.source == CNET_LEARN_SRC_MEMORY,
          "second call uses skill/memory");
    check(rep.skill_reused == 1 || rep.from_cache == 1 || rep.source == CNET_LEARN_SRC_MEMORY,
          "reused skill or memory cache");

    n = cnet_learn_list_skills(skills, names, 16);
    check(n >= 1, "list_skills finds at least one");
    check(strcmp(cnet_learn_source_name(CNET_LEARN_SRC_WIKI), "wiki") == 0,
          "source_name wiki");

    /* Offline miss path */
    cfg.use_tools = 0;
    memset(&rep, 0, sizeof rep);
    check(cnet_learn_cycle("zzzz_unknown_entity_xyzzy_999", &cfg, &rep) != 0 ||
              rep.source == CNET_LEARN_SRC_NONE || rep.source == CNET_LEARN_SRC_MEMORY,
          "unknown without tools does not invent wiki");

    printf("LEARN_LOOP_PASS checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
