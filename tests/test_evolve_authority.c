#define main evolve_program_main
#include "../tools/roe_evolve_tick.c"
#undef main

int main(void) {
    int failed = 0;
    const char *reject[] = {
        "{\"source\": \"CORE\"}",
        "{\"source\": \"LOCAL\", \"claimed_cert\": 1}",
        "{\"source\":\"LLM\", \"self_authored\": true}",
        "{\"source\":\"LLM\", \"claimed_cert\":true}",
        "{\"source\":\"LLM\", \"via\": \"kb_recall\"}",
        "{\"source\":\"LLM\",\"source\":\"LOCAL\"}",
        "{\"source\":\"LOCAL\",\"source\":\"LLM\"}",
        "{\"source\":\"LLM\",\"self_authored\":\"false\"}",
        "{\"source\":\"TOOL\",\"verified\":false}",
        "{\"source\":\"LLM\"} garbage", "{}", "not JSON",
        "{\"nested\":{\"source\":\"LLM\"}}",
        "{\"source\":\"LLM\",\"self_\\u0061uthored\":true}",
        "{\"source\":\"LLM\",\"query\":\"a\",\"query\":\"b\"}"
    };
    for (size_t i = 0; i < sizeof reject / sizeof *reject; i++) {
        if (!miss_row_self_authored(reject[i])) {
            fprintf(stderr, "EVOLVE_AUTHORITY_RED provenance_case=%zu\n", i);
            failed++;
        }
    }
    if (miss_row_self_authored("{\"source\": \"LLM\",\"self_authored\": false}")) failed++;
    if (miss_row_self_authored("{\"source\":\"TOOL\",\"verified\":true}")) failed++;
    Paths p = {0};
    snprintf(p.front_door, sizeof p.front_door, "/bin/true");
    if (promote_via_front_door(&p, "test query", "answer", 0)) {
        fprintf(stderr, "EVOLVE_AUTHORITY_RED exit_zero_without_receipt\n"); failed++;
    }
    /* Exercise the entire production tick with a failing front door. */
    char root[] = "/tmp/cnet-evolve-authority-XXXXXX", cwd[2048];
    if (!getcwd(cwd, sizeof cwd) || !mkdtemp(root) || chdir(root)) return 2;
    setenv("CNET_PACKS_ROOT", root, 1);
    setenv("CNET_MINIMAL_ROOT", root, 1);
    setenv("CNET_FRONT_DOOR_BIN", "/bin/false", 1);
    char *args[] = {"evolve", "--seed-demo", "--no-reviewer", NULL};
    if (evolve_program_main(3, args)) failed++;
    FILE *report = fopen("EVOLVE_TICK.json", "r");
    char content[8192] = {0};
    if (report) { size_t n = fread(content, 1, sizeof content - 1, report); content[n] = 0; fclose(report); }
    struct stat cat_stat;
    if (!strstr(content, "front_door_admission_failed") ||
        (stat("pack_personal/catalog.jsonl", &cat_stat) == 0 && cat_stat.st_size != 0)) {
        puts("EVOLVE_AUTHORITY_RED failed_admission_published"); failed++;
    }
    /* Remove only this fixture's gold, then repeated external proposals must
       refuse without independent review, even when configured stable-n=1. */
    char hash[17], gp[128]; roe_q_hash16("roe evolve tick demo query alpha", hash);
    snprintf(gp, sizeof gp, "gold/%s.txt", hash); remove(gp);
    char *again[] = {"evolve", "--no-reviewer", "--stable-n", "1", NULL};
    if (evolve_program_main(4, again)) failed++;
    report = fopen("EVOLVE_TICK.json", "r"); memset(content, 0, sizeof content);
    if (report) { size_t n = fread(content, 1, sizeof content - 1, report); content[n] = 0; fclose(report); }
    if (!strstr(content, "reviewer_unavailable") || !strstr(content, "\"stable_n\": 3") ||
        (stat("pack_personal/catalog.jsonl", &cat_stat) == 0 && cat_stat.st_size != 0)) {
        puts("EVOLVE_AUTHORITY_RED reviewer_disabled_promoted"); failed++;
    }
    remove("EVOLVE_TICK.json"); remove("evolve_state.json"); remove("miss_log.jsonl");
    rmdir("gold"); rmdir("pack_personal");
    if (chdir(cwd)) return 2;
    rmdir(root);
    puts(failed ? "EVOLVE_AUTHORITY_RED" : "EVOLVE_AUTHORITY_PASS");
    return failed ? 1 : 0;
}
