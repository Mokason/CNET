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
    cnet_ood_numerals_clear();
    check(cnet_ood_parse_arith("twelve plus five", &a, &b, &op) != 0,
          "words_not_hardcoded");
    check(cnet_ood_arith_shaped("dozen plus five", &op) == 1 &&
              op == CNET_OOD_OP_ADD,
          "gap_shaped");
    check(cnet_ood_arith_shaped("what is a plus sign", &op) == 0,
          "plus_sign_not_shaped");
    check(cnet_ood_arith_shaped("fix the repo", &op) == 0, "fix_not_shaped");
    check(cnet_ood_try_add("dozen plus five", &r) == 0, "gap_try_rc");
    check(r.kind == CNET_SKILL_LANE_ABSTAIN, "gap_kind");
    check(r.claimed_cert == 0, "gap_no_cert");
    check(r.teacher_calls == 0 && r.residual_calls == 0, "gap_no_teacher");
    check(cnet_ood_handle("dozen plus five", &r) == 0, "gap_handle_owns");
    check(r.kind == CNET_SKILL_LANE_ABSTAIN && r.claimed_cert == 0,
          "gap_handle_not_faq");
    {
        const char *np = "/tmp/cnet_ood_numerals.tsv";
        FILE *nf = fopen(np, "w");
        check(nf != NULL, "numeral_tsv");
        if (nf) {
            fputs("twelve\t12\nfive\t5\neleven\t11\ntwo\t2\ntwenty\t20\nthree\t3\n",
                  nf);
            fclose(nf);
        }
        check(cnet_ood_load_numerals(np) == 6, "numeral_load");
        check(cnet_ood_gold_numeral("dozen", "12", np) == 1, "numeral_gold");
        check(cnet_ood_gold_numeral("twelve plus five", "17", np) == 0,
              "numeral_gold_not_faq_sum");
    }
    check(cnet_ood_parse_arith("twelve plus five", &a, &b, &op) == 0 &&
              a == 12ul && b == 5ul && op == CNET_OOD_OP_ADD,
          "arith_number_words");
    check(cnet_ood_parse_arith("eleven times two", &a, &b, &op) == 0 &&
              a == 11ul && b == 2ul && op == CNET_OOD_OP_MUL,
          "arith_words_times");
    check(cnet_ood_parse_arith("twenty minus three", &a, &b, &op) == 0 &&
              a == 20ul && b == 3ul && op == CNET_OOD_OP_SUB,
          "arith_words_minus");
    check(cnet_ood_try_add("twelve plus five", &r) == 0 &&
              strcmp(r.value, "17") == 0 && r.claimed_cert == 1,
          "add_words_17_not_faq");
    check(cnet_ood_try_add("dozen plus five", &r) == 0 &&
              strcmp(r.value, "17") == 0,
          "add_gold_numeral_dozen");
    check(cnet_ood_try_add("gross plus five", &r) == 0 &&
              r.kind == CNET_SKILL_LANE_ABSTAIN && r.claimed_cert == 0,
          "unknown_numeral_abstain");
    {
        char unk[32];
        check(cnet_ood_gap_unknown("gross plus five", unk, sizeof unk) == 0 &&
                  !strcmp(unk, "gross"),
              "gap_unknown_gross");
        check(cnet_ood_gap_unknown("twelve plus five", unk, sizeof unk) != 0,
              "gap_unknown_none_when_overlay");
        check(cnet_ood_gap_unknown("what is a plus sign", unk, sizeof unk) != 0,
              "gap_unknown_not_shaped");
    }
    {
        const char *mp = "/tmp/cnet_ood_miss.jsonl";
        const char *pp = "/tmp/cnet_ood_propose.tsv";
        FILE *mf = fopen(mp, "w");
        check(mf != NULL, "tick_miss_file");
        if (mf) {
            fputs("{\"query\":\"score plus one\",\"via\":\"numeral_gap\","
                  "\"auto_cert\":false}\n",
                  mf);
            fputs("{\"query\":\"10+11\",\"answer\":\"21\"}\n", mf);
            fclose(mf);
        }
        unlink(pp);
        check(cnet_ood_numeral_tick(mp, pp, "/tmp/cnet_ood_numerals.tsv") == 1,
              "tick_proposes_score");
        check(cnet_ood_numeral_tick(mp, pp, "/tmp/cnet_ood_numerals.tsv") == 0,
              "tick_idempotent");
        {
            FILE *pf = fopen(pp, "r");
            char line[64];
            check(pf != NULL, "tick_propose_exists");
            if (pf) {
                check(fgets(line, sizeof line, pf) != NULL &&
                          strstr(line, "score") != NULL,
                      "tick_propose_word");
                fclose(pf);
            }
        }
    }

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
    check(cnet_ood_parse_arith("20/4", &a, &b, &op) == 0 && a == 20ul &&
              b == 4ul && op == CNET_OOD_OP_DIV,
          "arith_div_slash");
    check(cnet_ood_try_add("20/4", &r) == 0 && strcmp(r.value, "5") == 0 &&
              strcmp(r.skill, CNET_OOD_DIV) == 0 && r.claimed_cert == 1,
          "div_5_not_faq");
    check(cnet_ood_try_add("20 divided by 4", &r) == 0 && strcmp(r.value, "5") == 0,
          "div_words");
    check(cnet_ood_try_add("20/0", &r) == 0 && r.claimed_cert == 0 &&
              r.kind == CNET_SKILL_LANE_ABSTAIN,
          "div_zero_abstain");
    check(cnet_ood_try_add("20 % 6", &r) == 0 && strcmp(r.value, "2") == 0 &&
              strcmp(r.skill, CNET_OOD_MOD) == 0,
          "mod_2");
    check(cnet_ood_try_add("20 mod 6", &r) == 0 && strcmp(r.value, "2") == 0,
          "mod_word");
    check(cnet_ood_try_add("3 < 5", &r) == 0 && strcmp(r.value, "1") == 0 &&
              strcmp(r.skill, CNET_OOD_CMP) == 0,
          "cmp_lt");
    check(cnet_ood_try_add("3 > 5", &r) == 0 && strcmp(r.value, "0") == 0,
          "cmp_gt");
    check(cnet_ood_try_add("3 == 3", &r) == 0 && strcmp(r.value, "1") == 0,
          "cmp_eq");
    check(cnet_ood_try_add("min 3 5", &r) == 0 && strcmp(r.value, "3") == 0 &&
              strcmp(r.skill, CNET_OOD_MIN) == 0,
          "min_3");
    check(cnet_ood_try_add("max 3 5", &r) == 0 && strcmp(r.value, "5") == 0,
          "max_5");
    check(cnet_ood_try_add("clamp 10 0 5", &r) == 0 && strcmp(r.value, "5") == 0 &&
              strcmp(r.skill, CNET_OOD_CLAMP) == 0 && r.claimed_cert == 1,
          "clamp_hi");
    check(cnet_ood_try_add("clamp 3 0 5", &r) == 0 && strcmp(r.value, "3") == 0,
          "clamp_mid");
    check(cnet_ood_try_add("clamp 1 5 0", &r) == 0 && r.claimed_cert == 0,
          "clamp_bad_range_abstain");
    check(cnet_ood_try_add("compose add 3 5 min 4", &r) == 0 &&
              strcmp(r.value, "4") == 0 && strcmp(r.skill, CNET_OOD_COMPOSE) == 0,
          "compose_add_then_min");
    check(cnet_ood_try_add("compose add 3 5 then min 10", &r) == 0 &&
              strcmp(r.value, "8") == 0,
          "compose_then_word");
    check(cnet_ood_try_add("what time is it", &r) == 0 &&
              strcmp(r.skill, CNET_OOD_CLOCK) == 0 && r.claimed_cert == 1 &&
              strchr(r.value, 'T') != NULL,
          "clock_now");
    check(cnet_ood_try_add("load average", &r) == 0 &&
              strcmp(r.skill, CNET_OOD_HOST_LOAD) == 0 && r.claimed_cert == 1,
          "host_load");
    check(cnet_ood_try_add("uptime", &r) == 0 &&
              strcmp(r.skill, CNET_OOD_HOST_UP) == 0,
          "host_uptime");
    check(cnet_ood_try_add("disk free", &r) == 0 &&
              strcmp(r.skill, CNET_OOD_HOST_DISK) == 0 &&
              strstr(r.value, "MiB") != NULL,
          "host_disk");
    check(cnet_ood_try_add("3 and 5", &r) == 0 && strcmp(r.value, "1") == 0 &&
              strcmp(r.skill, CNET_OOD_AND) == 0,
          "and_1");
    check(cnet_ood_try_add("3 xor 1", &r) == 0 && strcmp(r.value, "2") == 0,
          "xor_2");
    check(cnet_ood_try_add("1 << 3", &r) == 0 && strcmp(r.value, "8") == 0,
          "shl_8");
    check(cnet_ood_try_add("compose add 3 5 then min 10 then max 2", &r) == 0 &&
              strcmp(r.value, "8") == 0,
          "compose_3hop");
    check(cnet_ood_try_add("3 plus 5 then min 4", &r) == 0 &&
              strcmp(r.value, "4") == 0 && strcmp(r.skill, CNET_OOD_COMPOSE) == 0,
          "compose_infix_no_word");
    check(cnet_ood_try_add("5 minutes in seconds", &r) == 0 &&
              strcmp(r.value, "300") == 0 &&
              strcmp(r.skill, CNET_OOD_MINUTES) == 0,
          "minutes_to_seconds");
    check(cnet_ood_try_add("crc8 1", &r) == 0 &&
              strcmp(r.skill, CNET_OOD_CRC8) == 0 && r.claimed_cert == 1,
          "crc8_atm_byte");
    check(cnet_ood_try_add("days between 2026-01-01 2026-01-10", &r) == 0 &&
              strcmp(r.value, "9") == 0 && strcmp(r.skill, CNET_OOD_DATE) == 0,
          "date_days_between");
    check(cnet_ood_try_add("day of week 2026-09-03", &r) == 0 &&
              strcmp(r.skill, CNET_OOD_DATE) == 0 && r.claimed_cert == 1,
          "date_dow");
    check(cnet_ood_try_add("how much ram", &r) == 0 &&
              strcmp(r.skill, CNET_OOD_HOST_MEM) == 0,
          "host_mem");
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
