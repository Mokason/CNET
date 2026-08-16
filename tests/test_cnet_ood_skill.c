/* test_cnet_ood_skill — OOD HANDLER
 * Leftover hops to compute / fetched extract. No fact table.
 * residual_calls == 0 && teacher_calls == 0. No Python.
 */
#define _POSIX_C_SOURCE 200809L
#include "../include/cnet_ood_skill.h"
#include "../include/cnet_lookup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static int checks;

static int hook_ok(const char *turn, char *out, size_t cap) {
    (void)turn;
    if (out == NULL || cap == 0) return 1;
    snprintf(out, cap, "held-hook");
    return 0;
}

static void check(int cond, const char *msg) {
    checks++;
    if (cond) {
        printf("  ok   %s\n", msg);
    } else {
        printf("  FAIL %s\n", msg);
        failures++;
    }
}

int main(void) {
    CnetSkillLaneResult r;
    char title[CNET_OOD_TITLE];
    char extract[CNET_OOD_EXTRACT];
    unsigned long a, b;
    int op;
    char path[] = "/tmp/cnet-ood-extract-XXXXXX";
    char years[] = "/tmp/cnet-ood-year-XXXXXX";
    char url[640];
    int fd;
    const char *json =
        "{\"type\":\"standard\",\"title\":\"Bird migration\","
        "\"extract\":\"Birds migrate to find food and nesting sites.\"}";
    const char *wiki_year =
        "Use mdy dates from 2026. founded = {{Start date and age|1976|04|01}}\n";
    const char *teacher = "The capital of France is Paris and the year is 1799.";

    unsetenv("CNET_HELD_MODEL_ENDPOINT");
    unsetenv("CNET_HELD_MODEL_PATH");
    cnet_held_model_set_hook(NULL);
    cnet_held_model_set_endpoint(NULL);
    cnet_held_model_set_path(NULL);

    printf("cnet_ood_skill — OOD HANDLER / EXACT NEVER ESCALATES\n");

    check(cnet_ood_subject("why do birds migrate", title, sizeof title) == 0,
          "subject_rc");
    check(strcmp(title, "Birds_Migrate") == 0, "subject_birds_migrate");
    check(cnet_ood_subject("what year was tesla incorporated", title,
                           sizeof title) == 0,
          "subject_tesla_rc");
    check(strcmp(title, "Tesla") == 0, "subject_tesla");
    check(cnet_ood_subject("???", title, sizeof title) == 1, "subject_empty");

    check(cnet_ood_parse_arith("what is 41 plus 1", &a, &b, &op) == 0,
          "arith_plus_rc");
    check(a == 41ul && b == 1ul && op == CNET_OOD_OP_ADD, "arith_plus_vals");
    check(cnet_ood_parse_arith("255 + 1", &a, &b, &op) == 0 && a == 255ul &&
              b == 1ul,
          "arith_plus_sym");
    check(cnet_ood_parse_arith("10 minus 3", &a, &b, &op) == 0 &&
              op == CNET_OOD_OP_SUB && a == 10ul && b == 3ul,
          "arith_minus");
    check(cnet_ood_parse_arith("6 times 7", &a, &b, &op) == 0 &&
              op == CNET_OOD_OP_MUL,
          "arith_times");
    check(cnet_ood_parse_arith("plus 1", &a, &b, &op) == 1, "arith_one_number");

    check(cnet_ood_try_add("what is 41 plus 1", &r) == 0, "add_rc");
    check(r.kind == CNET_SKILL_LANE_EXACT, "add_exact");
    check(strcmp(r.skill, CNET_OOD_ADD) == 0, "add_skill");
    check(strcmp(r.value, "42") == 0, "add_42");
    check(r.claimed_cert == 1, "add_cert");
    check(r.residual_calls == 0 && r.teacher_calls == 0, "add_no_teacher");
    check(strstr(r.spoken, teacher) == NULL, "add_not_teacher_text");

    check(cnet_ood_try_add("255 plus 1", &r) == 0, "add_256_rc");
    check(strcmp(r.value, "256") == 0, "add_256");

    check(cnet_ood_try_add("10 minus 3", &r) == 0 && strcmp(r.value, "7") == 0,
          "sub_7");
    check(cnet_ood_try_add("6 times 7", &r) == 0 && strcmp(r.value, "42") == 0,
          "mul_42");
    check(cnet_ood_try_add("4000000000 plus 4000000000", &r) == 1, "add_overflow");

    check(cnet_ood_json_extract(json, extract, sizeof extract) == 0,
          "extract_rc");
    check(strstr(extract, "Birds migrate") != NULL, "extract_text");
    check(cnet_ood_json_extract("{\"title\":\"x\"}", extract, sizeof extract) ==
              1,
          "extract_missing");

    fd = mkstemp(path);
    check(fd >= 0 && write(fd, json, strlen(json)) == (ssize_t)strlen(json),
          "extract_fixture");
    if (fd >= 0) close(fd);
    snprintf(url, sizeof url, "file://%s", path);
    memset(&r, 0, sizeof r);
    check(cnet_ood_try_wiki_url(url, CNET_LOOKUP_F_ALLOW_FILE, 0, "Birds",
                                &r) == 0,
          "wiki_file_rc");
    check(r.kind == CNET_SKILL_LANE_EXACT, "wiki_file_exact");
    check(strcmp(r.skill, CNET_OOD_WIKI) == 0, "wiki_file_skill");
    check(strstr(r.spoken, "Birds migrate") != NULL, "wiki_file_spoken");
    check(r.claimed_cert == 1, "wiki_file_cert");
    check(r.residual_calls == 0 && r.teacher_calls == 0, "wiki_file_no_teacher");
    unlink(path);

    fd = mkstemp(years);
    check(fd >= 0 &&
              write(fd, wiki_year, strlen(wiki_year)) == (ssize_t)strlen(wiki_year),
          "year_fixture");
    if (fd >= 0) close(fd);
    snprintf(url, sizeof url, "file://%s", years);
    memset(&r, 0, sizeof r);
    check(cnet_ood_try_wiki_url(url, CNET_LOOKUP_F_ALLOW_FILE, 1, "Apple",
                                &r) == 0,
          "wiki_year_rc");
    check(strcmp(r.value, "1976") == 0, "wiki_year_cue");
    check(r.teacher_calls == 0, "wiki_year_no_teacher");
    unlink(years);

    check(cnet_ood_handle("what is 41 plus 1", &r) == 0, "handle_add_rc");
    check(strcmp(r.value, "42") == 0, "handle_add_42");
    check(r.kind == CNET_SKILL_LANE_EXACT, "handle_add_exact");

    check(cnet_ood_handle("fix the repo", &r) == 1, "handle_fix_leftover");
    check(r.claimed_cert == 0, "handle_fix_no_cert");
    check(r.teacher_calls == 0, "handle_fix_no_teacher");

    cnet_held_model_set_hook(hook_ok);
    check(cnet_ood_handle("fix the repo", &r) == 0, "held_hook_rc");
    check(r.kind == CNET_SKILL_LANE_HELD, "held_hook_kind");
    check(strcmp(r.skill, CNET_HELD_CONTRACT) == 0, "held_hook_skill");
    check(strstr(r.spoken, "held-hook") != NULL, "held_hook_spoken");
    check(r.claimed_cert == 0, "held_hook_no_cert");
    check(r.teacher_calls == 0, "held_hook_no_teacher");
    cnet_held_model_set_hook(NULL);

    if (failures) {
        printf("CNET_OOD_RED failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("CNET_OOD_PASS\n");
    printf("checks=%d residual=0 teacher=0 python=0 hardcoded_facts=0 "
           "broader_claims=WITHHELD\n",
           checks);
    return 0;
}
