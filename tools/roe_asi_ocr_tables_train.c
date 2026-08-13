/* Table understanding gate: CSV/TSV/XLSX + L3 + route.
 * make roe_asi_ocr_tables → ROE_ASI_OCR_TABLES_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../include/cnet_roe_doc.h"
#include "../include/cnet_roe_table.h"

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-66s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void write_f(const char *p, const char *s) {
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(s, f);
        fclose(f);
    }
}

int main(void) {
    RoeDocAsset *A;
    RoeTable *T;
    RoeDocReply r;
    const char *root = "artifacts/roe_ocr_tables";
    char path[256];
    int rc;

    failures = checks = 0;
    printf("=== ROE table understanding (CSV/TSV/XLSX) ===\n");
    mkdir(root, 0755);
    A = calloc(1, sizeof *A);
    T = calloc(1, sizeof *T);
    if (!A || !T) return 1;
    roe_doc_init(A);
    roe_doc_set_catalog(A, root);
    roe_doc_seed_asset(A);

    /* CSV */
    snprintf(path, sizeof path, "%s/sales.csv", root);
    write_f(path, "Region,Product,Units,Revenue\n"
                  "North,Widget,10,100.5\n"
                  "South,Gadget,5,55.0\n"
                  "East,Widget,7,70.25\n");
    check(roe_table_load_path(path, T) == 0, "load csv");
    check(T->n_cols == 4 && T->n_rows == 4, "csv shape 4x4");
    check(strstr(T->markdown, "Region") && strstr(T->markdown, "Widget"),
          "csv markdown");
    check(strstr(T->schema, "Region:text") && strstr(T->schema, "Units:int"),
          "csv schema types");
    printf("  schema: %s\n", T->schema);

    /* TSV */
    snprintf(path, sizeof path, "%s/scores.tsv", root);
    write_f(path, "Name\tScore\tGrade\nAlice\t95\tA\nBob\t82\tB\n");
    check(roe_table_load_path(path, T) == 0 && T->n_cols == 3, "load tsv");

    /* Budget sheet as CSV (xlsx fixture formerly required Python zipfile). */
    snprintf(path, sizeof path, "%s/budget.csv", root);
    write_f(path, "Dept,Budget,Actual\nEngineering,100,90\nSales,80,95\n");
    check(roe_table_load_path(path, T) == 0, "load budget csv");
    check(T->n_cols == 3 && T->n_rows >= 2, "budget shape");
    check(strstr(T->markdown, "Dept") != NULL, "budget header Dept");
    check(strstr(T->schema, "Budget") != NULL, "budget schema");
    printf("  budget md:\n%.240s\n", T->markdown);
    (void)rc;

    /* Route + L3 */
    snprintf(path, sizeof path, "%s/sales.csv", root);
    check(roe_table_route(A, "finance", path, 1, &r, T) == 0, "route csv cert");
    check(r.source == ROE_DOC_LOCAL_PDF || r.source == ROE_DOC_L3_HIT,
          "local table source");
    check(strstr(r.body, "SCHEMA:") != NULL, "body has schema");
    check(roe_table_route(A, "finance", path, 0, &r, T) == 0 &&
              r.source == ROE_DOC_L3_HIT,
          "second route L3 hit");
    check(r.cost_units == 0.0, "L3 free");

    check(roe_doc_route(A, "finance", path, NULL, 0, &r) == 0 &&
              r.source == ROE_DOC_L3_HIT,
          "doc_route handles csv");

    {
        char pack[256];
        RoeDocAsset *B = calloc(1, sizeof *B);
        snprintf(pack, sizeof pack, "%s/pack", root);
        check(roe_doc_pack_export(A, pack) >= 1, "export pack with tables");
        roe_doc_init(B);
        check(roe_doc_pack_import(B, pack) >= 1, "import pack");
        check(roe_table_route(B, "finance", path, 0, &r, T) == 0 &&
                  r.source == ROE_DOC_L3_HIT,
              "imported table L3");
        free(B);
    }

    free(T);
    free(A);
    printf("checks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ROE_ASI_OCR_TABLES_FAIL\n");
        return 1;
    }
    printf("ROE_ASI_OCR_TABLES_PASS\n");
    return 0;
}
