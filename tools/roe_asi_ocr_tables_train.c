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

    /* XLSX fixture script */
    write_f("artifacts/roe_ocr_tables/make_xlsx.py",
            "import zipfile, pathlib\n"
            "p=pathlib.Path('artifacts/roe_ocr_tables/budget.xlsx')\n"
            "ss=['Dept','Budget','Actual','Engineering','100','90','Sales','80','95']\n"
            "def sst():\n"
            " items=''.join(f'<si><t>{x}</t></si>' for x in ss)\n"
            " return ('<?xml version=\"1.0\"?><sst xmlns=\"http://schemas.openxmlformats.org/"
            "spreadsheetml/2006/main\" count=\"%d\" uniqueCount=\"%d\">%s</sst>'%(len(ss),len(ss),items))\n"
            "rows=''\n"
            "idx=0\n"
            "for r in range(1,4):\n"
            " cells=''\n"
            " for c in 'ABC':\n"
            "  cells+=f'<c r=\"{c}{r}\" t=\"s\"><v>{idx}</v></c>'\n"
            "  idx+=1\n"
            " rows+=f'<row r=\"{r}\">{cells}</row>'\n"
            "sheet=('<?xml version=\"1.0\"?><worksheet xmlns=\"http://schemas.openxmlformats.org/"
            "spreadsheetml/2006/main\"><sheetData>%s</sheetData></worksheet>'%rows)\n"
            "ct='''<?xml version=\"1.0\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
            "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
            "<Default Extension=\"xml\" ContentType=\"application/xml\"/>\n"
            "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>\n"
            "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>\n"
            "<Override PartName=\"/xl/sharedStrings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>\n"
            "</Types>'''\n"
            "rels='''<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>\n"
            "</Relationships>'''\n"
            "wb='''<?xml version=\"1.0\"?><workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">\n"
            "<sheets><sheet name=\"Sheet1\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>'''\n"
            "wbr='''<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>\n"
            "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" Target=\"sharedStrings.xml\"/>\n"
            "</Relationships>'''\n"
            "with zipfile.ZipFile(p,'w') as z:\n"
            " z.writestr('[Content_Types].xml', ct)\n"
            " z.writestr('_rels/.rels', rels)\n"
            " z.writestr('xl/workbook.xml', wb)\n"
            " z.writestr('xl/_rels/workbook.xml.rels', wbr)\n"
            " z.writestr('xl/sharedStrings.xml', sst())\n"
            " z.writestr('xl/worksheets/sheet1.xml', sheet)\n"
            "print('ok')\n");
    rc = system("python3 artifacts/roe_ocr_tables/make_xlsx.py");
    check(rc == 0, "create xlsx fixture");
    snprintf(path, sizeof path, "%s/budget.xlsx", root);
    check(roe_table_load_path(path, T) == 0, "load xlsx");
    check(T->n_cols == 3 && T->n_rows >= 2, "xlsx shape");
    check(strstr(T->markdown, "Dept") != NULL, "xlsx header Dept");
    check(strstr(T->schema, "Budget") != NULL, "xlsx schema");
    printf("  xlsx md:\n%.240s\n", T->markdown);

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
