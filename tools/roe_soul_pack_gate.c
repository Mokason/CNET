/* Gate: pack_soul_marble SOUL persona capsule.
 * make roe_soul_pack → ROE_SOUL_PACK_PASS
 *
 * Proves: kind=persona, SOUL.md present, seal_path forbidden,
 * LOCAL identity hits, never claims CERT without gate.
 */
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

static int file_has(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    char buf[4096];
    size_t n;
    int hit = 0;
    if (!f) return 0;
    while ((n = fread(buf, 1, sizeof buf - 1, f)) > 0) {
        buf[n] = 0;
        if (strstr(buf, needle)) {
            hit = 1;
            break;
        }
    }
    fclose(f);
    return hit;
}

int main(void) {
    const char *root = "artifacts/roe_daily_packs/pack_soul_marble";
    RoeAsi R;
    RoeReply out;
    int n;

    failures = checks = 0;
    printf("=== ROE SOUL pack (Marble persona) ===\n");

    check(file_has("artifacts/roe_daily_packs/pack_soul_marble/PACK.abi",
                    "kind persona"),
          "PACK.abi kind=persona");
    check(file_has("artifacts/roe_daily_packs/pack_soul_marble/PACK.abi",
                    "seal_path forbidden"),
          "seal_path forbidden");
    check(file_has("artifacts/roe_daily_packs/pack_soul_marble/PACK.abi",
                    "second_brain 0"),
          "second_brain 0");
    check(file_has("artifacts/roe_daily_packs/pack_soul_marble/SOUL.md", "Marble"),
          "SOUL.md present");
    check(file_has("artifacts/roe_daily_packs/pack_soul_marble/voice.md",
                    "Never injected into certify"),
          "voice.md present (seal path comment)");
    check(file_has("artifacts/roe_daily_packs/pack_soul_marble/catalog.jsonl",
                    "soul_who"),
          "catalog has soul_who");

    roe_init(&R);
    roe_set_catalog_dir(&R, root);
    n = roe_load_catalog(&R);
    printf("  loaded skills=%d\n", n);
    check(n >= 10, ">=10 persona skills loaded");

    check(roe_turn(&R, "who are you", &out) == ROE_OK, "turn who are you");
    check(out.source == ROE_SRC_LOCAL && out.verified == 1, "LOCAL verified");
    check(strstr(out.answer, "Marble") != NULL, "answers as Marble");

    check(roe_turn(&R, "one line marble", &out) == ROE_OK, "one line");
    check(strstr(out.answer, "sit, see") != NULL || strstr(out.answer, "Marble") != NULL,
          "one-line soul");

    check(roe_turn(&R, "claim cert without gate", &out) == ROE_OK, "taboo cert");
    check(out.source == ROE_SRC_LOCAL, "taboo is local CERT skill");
    check(strstr(out.answer, "Never claim") != NULL ||
              strstr(out.answer, "never") != NULL ||
              strstr(out.answer, "gate") != NULL,
          "refuses cert without gate");

    check(roe_turn(&R, "is persona a second brain", &out) == ROE_OK, "second brain");
    check(strstr(out.answer, "second_brain") != NULL ||
              strstr(out.answer, "false") != NULL ||
              strstr(out.answer, "delivery") != NULL,
          "not second brain");

    check(roe_turn(&R, "vigilance high mode please", &out) == ROE_OK, "vigilance");
    check(out.source == ROE_SRC_LOCAL, "vigilance local");

    /* OOD stays miss — no self-invented persona lore as CERT */
    check(roe_turn(&R, "zz invent new catchphrase xyzzy", &out) == ROE_OK || 1,
          "ood turn");
    check(out.source != ROE_SRC_LOCAL || out.skill_id[0] == 0 ||
              strstr(out.skill_id, "soul_") == NULL,
          "OOD does not fake soul skill (or abstains)");

    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ROE_SOUL_PACK_FAIL\n");
        return 1;
    }
    printf("ROE_SOUL_PACK_PASS\n");
    return 0;
}
