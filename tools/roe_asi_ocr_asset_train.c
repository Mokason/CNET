/* Highest-leverage OCR asset: doc L3 + pack ABI + $/page freeze.
 * make roe_asi_ocr_asset → ROE_ASI_OCR_ASSET_PASS
 * Not a second brain — better SKU than bare Unlimited.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../include/cnet_roe_doc.h"

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-66s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    RoeDocAsset *A, *B, *C;
    RoeDocReply r;
    RoeDocDollarBench dol;
    char stats[600];
    const char *cat = "artifacts/roe_ocr_asset";
    const char *pack = "artifacts/roe_ocr_asset/pack_export";
    const char *badpack = "artifacts/roe_ocr_asset/bad_pack";

    failures = checks = 0;
    printf("=== ROE OCR asset (L3 + pack + $/page) — not second brain ===\n");

    A = (RoeDocAsset *)calloc(1, sizeof *A);
    B = (RoeDocAsset *)calloc(1, sizeof *B);
    C = (RoeDocAsset *)calloc(1, sizeof *C);
    if (!A || !B || !C) {
        printf("alloc fail\n");
        return 1;
    }

    roe_doc_init(A);
    roe_doc_set_catalog(A, cat);
    roe_doc_set_unit_cost(A, 200.0, 0.0);
    check(roe_doc_seed_asset(A) == 0, "seed asset skills");
    check(roe_doc_add_corpus(A, "acme_annual_2024") == 0, "add corpus");

    check(roe_doc_resolve(A, "acme_annual_2024",
                          "ACME CORP ANNUAL REPORT 2024 Confidential header",
                          &r) == 1,
          "cold miss abstain");
    check(r.source == ROE_DOC_ABSTAIN, "source abstain");
    check(roe_doc_verify_learn(
              A, "acme_annual_2024",
              "ACME CORP ANNUAL REPORT 2024 Confidential header",
              "# ACME 2024 Revenue up 12%. CERT from teacher.", "tests_ok", 1,
              0) == 1,
          "verify learn L3");
    check(roe_doc_resolve(A, "acme_annual_2024",
                          "ACME CORP ANNUAL REPORT 2024 Confidential header",
                          &r) == 0,
          "warm L3 hit");
    check(r.source == ROE_DOC_L3_HIT && r.certified_local, "L3 certified local");
    check(r.cost_units == 0.0, "L3 free");
    check(strstr(r.body, "Revenue") != NULL, "body retained");

    check(roe_doc_resolve(A, "other_corp",
                          "ACME CORP ANNUAL REPORT 2024 Confidential header",
                          &r) == 1,
          "no cross-corpus L3 leak");

    check(roe_doc_verify_learn(A, "acme_annual_2024", "random page foo",
                               "poison", "nope", 0, 0) == 0,
          "no silent CERT");

    check(roe_doc_verify_learn(A, "acme_annual_2024",
                               "ACME footer page numbers financials table",
                               "| Rev | 100 | Cost | 40 |", "table", 1, 0) == 1,
          "learn table page");
    check(roe_doc_resolve(A, "acme_annual_2024",
                          "ACME footer page numbers financials table",
                          &r) == 0 &&
              r.source == ROE_DOC_L3_HIT,
          "table L3 hit");

    {
        int nexp = roe_doc_pack_export(A, pack);
        check(nexp >= 2, "pack export L3 entries");
        check(roe_doc_pack_validate(pack) == 1, "pack ABI valid");
        mkdir(badpack, 0755);
        {
            FILE *f = fopen("artifacts/roe_ocr_asset/bad_pack/PACK.abi", "w");
            if (f) {
                fprintf(f, "NOT_ROE\nabi_version 99\n");
                fclose(f);
            }
        }
        check(roe_doc_pack_validate(badpack) == 0, "bad pack refused");

        roe_doc_init(B);
        roe_doc_set_catalog(B, "artifacts/roe_ocr_asset/import_tmp");
        check(roe_doc_pack_import(B, pack) >= 2, "pack import");
        check(roe_doc_resolve(B, "acme_annual_2024",
                              "ACME CORP ANNUAL REPORT 2024 Confidential header",
                              &r) == 0 &&
                  r.source == ROE_DOC_L3_HIT,
              "imported L3 serves");
        check(roe_doc_pack_import(B, badpack) < 0, "import bad fails closed");
    }

    roe_doc_init(C);
    roe_doc_set_unit_cost(C, 200.0, 0.0);
    roe_doc_seed_asset(C);
    check(roe_doc_dollar_bench(C, 100, 10, 0.001, &dol) == 0, "dollar bench");
    printf("  %s\n", dol.summary);
    check(dol.save_ratio >= 0.85, "save >= 85% on 10%% unique");
    check(dol.usd_per_page_hybrid < dol.usd_per_page_teacher, "$/page hybrid wins");
    check(dol.quality_match == 1, "quality match flag");
    check(dol.l3_hits >= 85, "most pages L3");
    {
        FILE *f = fopen("artifacts/roe_ocr_asset/dollar_per_page_freeze.json", "w");
        if (f) {
            fprintf(f,
                    "{\n"
                    "  \"abi\": \"ROE_OCR_PACK\",\n"
                    "  \"second_brain\": false,\n"
                    "  \"teacher_unit_cost\": 200,\n"
                    "  \"local_unit_cost\": 0,\n"
                    "  \"unit_to_usd\": 0.001,\n"
                    "  \"scenario\": {\"pages\": 100, \"unique\": 10},\n"
                    "  \"usd_per_page_teacher_always\": %.6f,\n"
                    "  \"usd_per_page_hybrid\": %.6f,\n"
                    "  \"save_ratio\": %.4f,\n"
                    "  \"l3_hits\": %d,\n"
                    "  \"teacher_calls\": %d,\n"
                    "  \"claim\": \"Better asset than bare Unlimited: teacher on "
                    "miss only, CERT packs, fail-closed import, lower $/page.\"\n"
                    "}\n",
                    dol.usd_per_page_teacher, dol.usd_per_page_hybrid,
                    dol.save_ratio, dol.l3_hits, dol.teacher_calls);
            fclose(f);
        }
    }
    check(access("artifacts/roe_ocr_asset/dollar_per_page_freeze.json", R_OK) == 0,
          "freeze file written");

    check(roe_doc_save(A) >= 2, "save asset catalog");
    roe_doc_dump_stats(A, stats, sizeof stats);
    printf("\n  %s\n", stats);
    printf("  catalog → %s\n", cat);
    printf("  pack → %s\n", pack);

    free(A);
    free(B);
    free(C);

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ROE_ASI_OCR_ASSET_FAIL\n");
        return 1;
    }
    printf("ROE_ASI_OCR_ASSET_PASS\n");
    return 0;
}
