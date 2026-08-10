/* Seed + train ROE-ASI on basic coding. make roe_asi_coding → ROE_ASI_CODING_PASS */
#include <stdio.h>
#include <string.h>

#include "../include/cnet_roe_asi.h"

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-64s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void seed_coding(RoeAsi *R) {
    /* Day-0 local certified basics (single-line so catalog.jsonl stays valid) */
    roe_add_skill(R, "hello_world", "hello_world", "hello world",
                  "print(\"Hello, world\")  # Python", 0, 1);
    roe_add_skill(R, "var_assign", "var_assign", "variable assign",
                  "name = value  # bind a name to a value", 0, 1);
    roe_add_skill(R, "if_else", "if_else", "if else",
                  "if cond: ... else: ...", 0, 1);

    roe_add_lookup(R, "compiler vs interpreter",
                   "Compiler translates ahead of time; interpreter executes source.");
    roe_add_lookup(R, "big o",
                   "Big-O describes how runtime/memory grow with input size.");

    roe_add_teach(R, "for loop", "for_loop",
                  "for x in items: process(x)  # iterate a sequence");
    roe_add_teach(R, "while loop", "while_loop",
                  "while cond: ...  # repeat until condition is false");
    roe_add_teach(R, "function def", "function_def",
                  "def name(args): return result  # reusable block");
    roe_add_teach(R, "list append", "list_append",
                  "xs.append(item)  # add to end of a Python list");
    roe_add_teach(R, "dict get", "dict_get",
                  "d.get(key, default)  # safe dict lookup");
    roe_add_teach(R, "string split", "str_split",
                  "s.split(sep)  # break string into a list of parts");
    roe_add_teach(R, "read file", "read_file",
                  "with open(path) as f: data = f.read()");
    roe_add_teach(R, "write file", "write_file",
                  "with open(path, \"w\") as f: f.write(text)");
    roe_add_teach(R, "try except", "try_except",
                  "try: risky()  except Error as e: handle(e)");
    roe_add_teach(R, "class init", "class_init",
                  "class C: def __init__(self, x): self.x = x");
    roe_add_teach(R, "recursion base", "recursion",
                  "def f(n): return n if n<=1 else f(n-1)+f(n-2)");
    roe_add_teach(R, "boolean and or", "boolean",
                  "and / or / not combine true/false conditions");
}

int main(void) {
    RoeAsi R;
    RoeTrainReport tr;
    RoeReply rep;
    char stats[700];
    const char *cat = "artifacts/roe_coding_catalog";
    const char *batch[] = {
        /* local seeds */
        "python hello world example",
        "how does variable assign work",
        "show if else structure",
        /* teach → promote */
        "python for loop example",
        "python while loop example",
        "function def syntax",
        "list append method",
        "dict get with default",
        "string split by comma",
        "how to read file in python",
        "how to write file in python",
        "try except error handling",
        "class init constructor",
        "recursion base case fibonacci",
        "boolean and or not",
        /* second pass (votes / local) */
        "python for loop example",
        "python while loop example",
        "function def syntax",
        "list append method",
        "dict get with default",
        "string split by comma",
        "how to read file in python",
        "how to write file in python",
        "try except error handling",
        "class init constructor",
        "recursion base case fibonacci",
        "boolean and or not",
        /* third pass — should be mostly local */
        "python for loop example",
        "function def syntax",
        "list append method",
        "how to read file in python",
        "try except error handling",
        "python hello world example",
        "show if else structure",
        /* lookup */
        "compiler vs interpreter difference",
        "what is big o notation",
        /* ood */
        "quantum kernel assembly teleport",
    };
    int n = (int)(sizeof batch / sizeof batch[0]);
    int e;
    size_t skills0;

    failures = checks = 0;
    printf("=== ROE-ASI basic coding curriculum ===\n");
    roe_init(&R);
    roe_set_catalog_dir(&R, cat);
    seed_coding(&R);
    skills0 = R.n_skills;
    check(skills0 >= 3, "seeded local coding skills");

    for (e = 1; e <= 3; e++) {
        roe_reset_stats(&R);
        roe_train_epoch(&R, batch, n, &tr);
        printf("epoch %d: hit=%.1f%% save=%.1f%% prom=%d tok=%llu/%llu skills=%zu\n",
               e, 100.0 * tr.local_hit_rate, 100.0 * tr.token_save_ratio, tr.promotes,
               (unsigned long long)tr.tokens_used,
               (unsigned long long)tr.tokens_baseline, R.n_skills);
    }

    check(R.n_skills > skills0, "learned new coding skills");
    check(tr.local_hit_rate >= 0.55, "coding hit rate >= 55%");
    check(tr.token_save_ratio >= 0.50, "coding token save >= 50%");
    check(roe_save_catalog(&R) >= 3, "saved coding catalog");

    /* Spot checks — must be LOCAL after train */
    check(roe_turn(&R, "python for loop example", &rep) == ROE_OK &&
              rep.source == ROE_SRC_LOCAL,
          "for loop local");
    check(roe_turn(&R, "function def syntax", &rep) == ROE_OK &&
              rep.source == ROE_SRC_LOCAL,
          "function def local");
    check(roe_turn(&R, "list append method", &rep) == ROE_OK &&
              rep.source == ROE_SRC_LOCAL,
          "list append local");
    check(roe_turn(&R, "try except error handling", &rep) == ROE_OK &&
              rep.source == ROE_SRC_LOCAL,
          "try/except local");
    check(roe_turn(&R, "python hello world example", &rep) == ROE_OK &&
              rep.source == ROE_SRC_LOCAL && rep.tokens_est == 0,
          "hello world free tokens");

    /* OOD still abstain */
    check(roe_turn(&R, "quantum kernel assembly teleport", &rep) == ROE_ABSTAIN,
          "ood coding abstain");

    /* Fresh load from disk */
    {
        RoeAsi R2;
        RoeReply r2;
        roe_init(&R2);
        roe_set_catalog_dir(&R2, cat);
        check(roe_load_catalog(&R2) >= 5, "reload coding catalog");
        check(roe_turn(&R2, "function def syntax", &r2) == ROE_OK &&
                  r2.source == ROE_SRC_LOCAL,
              "persisted function def serves");
        printf("  reloaded skills=%zu\n", R2.n_skills);
    }

    roe_dump_stats(&R, stats, sizeof stats);
    printf("\n  %s\n", stats);
    printf("  catalog → %s\n", cat);

    /* Print a few skills */
    {
        size_t i;
        int shown = 0;
        printf("  sample skills:\n");
        for (i = 0; i < R.n_skills && shown < 8; i++) {
            if (!R.skills[i].certified) continue;
            printf("    - %s :: %s\n", R.skills[i].id, R.skills[i].pattern);
            shown++;
        }
    }

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ROE_ASI_CODING_FAIL\n");
        return 1;
    }
    printf("ROE_ASI_CODING_PASS\n");
    return 0;
}
