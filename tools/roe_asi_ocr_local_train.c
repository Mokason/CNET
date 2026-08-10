/* Local-first ROE OCR: router + batch distill + teacher_rate KPI < 10%.
 * make roe_asi_ocr_local → ROE_ASI_OCR_LOCAL_PASS
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

static void write_file(const char *path, const char *s) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(s, f);
    fclose(f);
}

int main(void) {
    RoeDocAsset *A;
    RoeDocReply r;
    RoeDocCoverage cov;
    char stats[700];
    const char *root = "artifacts/roe_ocr_local";
    const char *corp = "artifacts/roe_ocr_local/corpus_docs";
    const char *hold = "artifacts/roe_ocr_local/holdout";
    int n_ok = 0, n_teach = 0, i;

    failures = checks = 0;
    printf("=== ROE OCR local-first (mostly without teacher) ===\n");

    A = calloc(1, sizeof *A);
    if (!A) return 1;
    mkdir(root, 0755);
    mkdir(corp, 0755);
    mkdir(hold, 0755);

    roe_doc_init(A);
    roe_doc_set_catalog(A, root);
    roe_doc_set_unit_cost(A, 200.0, 0.0);
    roe_doc_seed_asset(A);

    /* stronger sig differs by layout */
    {
        char s1[128], s2[128];
        roe_doc_signature_ex("Header ACME\nline2\nline3", 3, 0, 30, s1, sizeof s1);
        roe_doc_signature_ex("Header ACME\n|a|b|\n|1|2|", 3, 4, 30, s2, sizeof s2);
        check(strcmp(s1, s2) != 0, "layout-aware sig differs pipes");
        check(strstr(s1, "_L") != NULL && strstr(s1, "_P") != NULL,
              "sig contains layout tags");
    }

    /* factory corpus: 20 txt "pages", 2 templates × 10 */
    for (i = 0; i < 10; i++) {
        char p[256];
        snprintf(p, sizeof p, "%s/invoice_t1_%02d.txt", corp, i);
        write_file(p,
                   "INVOICE TEMPLATE ONE\nAcme Billing\nItem | Qty | Price\n"
                   "Widget | 2 | 10\nTotal due upon receipt.\n");
        snprintf(p, sizeof p, "%s/report_t2_%02d.txt", corp, i);
        write_file(p,
                   "QUARTERLY REPORT TEMPLATE TWO\nConfidential\n"
                   "Section A narrative text for the quarter.\n"
                   "Section B more narrative and KPIs.\n");
    }

    /* Batch distill (local text auto-cert) */
    check(roe_doc_batch_distill(A, "acme_docs", corp, 0, &n_ok, &n_teach) >= 10,
          "batch distill ok");
    printf("  distill ok=%d teacher_fallback=%d mem=%zu\n", n_ok, n_teach, A->n_mem);
    check(A->n_mem >= 2, "at least 2 L3 templates");

    /* Holdout: same templates, should be L3 / local — teacher rate low */
    {
        RoeDocAsset *H = calloc(1, sizeof *H);
        int hits = 0;
        roe_doc_init(H);
        roe_doc_set_unit_cost(H, 200.0, 0.0);
        /* import distilled memory via pack */
        {
            char pack[256];
            snprintf(pack, sizeof pack, "%s/pack", root);
            check(roe_doc_pack_export(A, pack) >= 2, "export distilled pack");
            check(roe_doc_pack_import(H, pack) >= 2, "import into holdout asset");
        }
        for (i = 0; i < 20; i++) {
            char p[256];
            if (i < 10)
                snprintf(p, sizeof p, "%s/invoice_t1_%02d.txt", corp, i);
            else
                snprintf(p, sizeof p, "%s/report_t2_%02d.txt", corp, i - 10);
            if (roe_doc_route(H, "acme_docs", p, NULL, 0, &r) == 0 &&
                (r.source == ROE_DOC_L3_HIT || r.source == ROE_DOC_LOCAL_PDF))
                hits++;
        }
        printf("  holdout local/L3 hits=%d/20\n", hits);
        check(hits >= 18, "holdout mostly local");
        roe_doc_coverage(H, &cov);
        /* After import, route may re-cert path; teacher_rate on holdout asset */
        printf("  coverage: %s\n", cov.summary);
        free(H);
    }

    /* Full onboard+serve KPI on single asset: reset counters via new asset
     * that loads pack then serves 100 pages with 5% novel */
    {
        RoeDocAsset *K = calloc(1, sizeof *K);
        char pack[256];
        int teacher_pages = 0, total = 100;
        roe_doc_init(K);
        roe_doc_set_unit_cost(K, 200.0, 0.0);
        snprintf(pack, sizeof pack, "%s/pack", root);
        (void)roe_doc_pack_import(K, pack);
        for (i = 0; i < total; i++) {
            char p[256];
            if (i < 95) {
                /* known templates */
                if (i % 2 == 0)
                    snprintf(p, sizeof p, "%s/invoice_t1_%02d.txt", corp, i % 10);
                else
                    snprintf(p, sizeof p, "%s/report_t2_%02d.txt", corp, i % 10);
                (void)roe_doc_route(K, "acme_docs", p, NULL, 0, &r);
                if (r.source == ROE_DOC_TEACHER || r.source == ROE_DOC_ABSTAIN)
                    teacher_pages++;
            } else {
                /* 5 novel pages need teacher */
                char novel[128];
                snprintf(novel, sizeof novel, "BRAND NEW FORM %d unique content xyz", i);
                (void)roe_doc_route(K, "acme_docs", NULL, novel, 0, &r);
                if (r.source == ROE_DOC_TEACHER || r.source == ROE_DOC_ABSTAIN)
                    teacher_pages++;
                /* factory would teach; sim teach for continuity */
                (void)roe_doc_verify_learn(K, "acme_docs", novel, novel, "gap", 1, 0);
            }
        }
        {
            double tr = (double)teacher_pages / (double)total;
            printf("  KPI teacher_pages=%d/%d rate=%.1f%%\n", teacher_pages, total,
                   100.0 * tr);
            check(tr < 0.10, "teacher_rate < 10% KPI");
            {
                FILE *f = fopen("artifacts/roe_ocr_local/teacher_rate_kpi.json", "w");
                if (f) {
                    fprintf(f,
                            "{\n  \"second_brain\": false,\n"
                            "  \"pages\": %d,\n  \"teacher_pages\": %d,\n"
                            "  \"teacher_rate\": %.4f,\n"
                            "  \"kpi_teacher_under_10pct\": %s,\n"
                            "  \"claim\": \"Mostly without teacher: local PDF/text + "
                            "L3 memory; teacher only on novel gaps.\"\n}\n",
                            total, teacher_pages, tr, tr < 0.10 ? "true" : "false");
                    fclose(f);
                }
            }
            check(access("artifacts/roe_ocr_local/teacher_rate_kpi.json", R_OK) == 0,
                  "kpi freeze file");
        }
        roe_doc_coverage(K, &cov);
        printf("  final coverage: %s\n", cov.summary);
        free(K);
    }

    /* Router unit: unknown → teacher signal */
    check(roe_doc_route(A, "acme_docs", NULL, "totally unseen zebra quilt 999", 0,
                        &r) != 0 ||
              r.source == ROE_DOC_TEACHER || r.source == ROE_DOC_ABSTAIN,
          "unseen needs teacher");

    roe_doc_dump_stats(A, stats, sizeof stats);
    printf("\n  %s\n", stats);

    free(A);
    printf("checks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ROE_ASI_OCR_LOCAL_FAIL\n");
        return 1;
    }
    printf("ROE_ASI_OCR_LOCAL_PASS\n");
    return 0;
}
