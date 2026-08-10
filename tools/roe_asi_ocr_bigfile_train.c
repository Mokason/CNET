/* Big-file capacity + page-chunked PDF stream bench.
 * make roe_asi_ocr_bigfile → ROE_ASI_OCR_BIGFILE_PASS
 *
 * Compares OLD caps (body 768 / table 64x32) vs NEW (4096 / 256x48)
 * and streams a multi-page PDF twice (cold local + warm L3).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../include/cnet_roe_doc.h"
#include "../include/cnet_roe_table.h"

static int g_fail;

static void expect(int cond, const char *msg) {
    if (cond) {
        printf("  %-56s PASS\n", msg);
    } else {
        printf("  %-56s FAIL\n", msg);
        g_fail = 1;
    }
}

static void mkdir_p(const char *p) {
    char cmd[512];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", p);
    system(cmd);
}

/* Simulate old truncation */
static size_t old_body_cap(void) { return 768; }
static int old_tbl_rows(void) { return 64; }
static int old_tbl_cols(void) { return 32; }

static void write_big_csv(const char *path, int rows, int cols) {
    FILE *f = fopen(path, "w");
    int r, c;
    if (!f) return;
    for (c = 0; c < cols; c++) fprintf(f, "%sCol%d", c ? "," : "", c);
    fputc('\n', f);
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++)
            fprintf(f, "%sR%dC%d", c ? "," : "", r, c);
        fputc('\n', f);
    }
    fclose(f);
}

static void write_long_txt(const char *path, size_t nchars) {
    FILE *f = fopen(path, "w");
    size_t i;
    if (!f) return;
    for (i = 0; i < nchars; i++) fputc((char)('a' + (i % 26)), f);
    fputc('\n', f);
    fclose(f);
}

int main(void) {
    RoeDocAsset *A;
    RoeTable *T;
    RoeDocReply r;
    char stats[800];
    const char *art = "artifacts/roe_ocr_bigfile";
    const char *pdf =
        "/home/marble/AI/stack/data/papers/machine-learning/"
        "1610.05492-federated-learning-strategies-for/1610.05492.pdf";
    char csv_path[256], long_path[256];
    int pages = 0, l3 = 0, local = 0, teacher = 0;
    int pages2 = 0, l32 = 0, local2 = 0, teacher2 = 0;
    size_t kept_old, kept_new;
    FILE *jf;
    struct stat st;

    g_fail = 0;
    printf("=== ROE big-file + PDF stream bench ===\n");
    printf("limits: BODY=%d MEM=%d TBL %dx%d MD=%d PDF_MAX=%d ABI=%d\n", ROE_DOC_BODY,
           ROE_DOC_MEM_MAX, ROE_TBL_MAX_ROWS, ROE_TBL_MAX_COLS, ROE_TBL_MD,
           ROE_DOC_PDF_MAX_PAGES, ROE_DOC_ABI_VER);

    mkdir_p(art);
    A = (RoeDocAsset *)calloc(1, sizeof *A);
    T = (RoeTable *)calloc(1, sizeof *T);
    if (!A || !T) {
        printf("alloc fail\n");
        return 1;
    }
    roe_doc_init(A);
    roe_doc_set_catalog(A, art);
    roe_doc_set_unit_cost(A, 200.0, 0.0);
    expect(roe_doc_seed_asset(A) == 0, "seed");

    /* ---- capacity: long body ---- */
    snprintf(long_path, sizeof long_path, "%s/long_3k.txt", art);
    write_long_txt(long_path, 3000);
    {
        FILE *f = fopen(long_path, "r");
        char buf[8192];
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        fclose(f);
        kept_old = n > old_body_cap() ? old_body_cap() : n;
        expect(roe_doc_verify_learn(A, "big", buf, buf, "long", 1, 0) == 1,
               "CERT long body");
        expect(roe_doc_resolve(A, "big", buf, &r) == 0 && r.source == ROE_DOC_L3_HIT,
               "L3 hit long body");
        kept_new = strlen(r.body);
        expect(kept_new > kept_old, "new body keeps more than old 768 cap");
        expect(kept_new >= 2000, "body retains >=2000 chars of 3000");
        printf("    body_kept old_sim=%zu new=%zu (limit %d)\n", kept_old, kept_new,
               ROE_DOC_BODY);
    }

    /* ---- capacity: big table ---- */
    snprintf(csv_path, sizeof csv_path, "%s/wide.csv", art);
    write_big_csv(csv_path, 200, 40); /* 200 data rows + header = 201, 40 cols */
    expect(roe_table_load_path(csv_path, T) == 0, "load 200x40 csv");
    expect(T->n_cols == 40, "cols=40 (> old 32)");
    expect(T->n_rows >= 65, "rows > old 64 cap");
    expect(T->n_rows <= ROE_TBL_MAX_ROWS, "rows within new max");
    expect((int)strlen(T->markdown) > 640, "markdown > old 640");
    expect(roe_table_route(A, "tables", csv_path, 1, &r, T) == 0, "route big table");
    expect(r.source == ROE_DOC_LOCAL_PDF || r.source == ROE_DOC_L3_HIT ||
               r.source == ROE_DOC_LOCAL_CLASSIC,
           "table local/L3 source");
    printf("    table n_rows=%d n_cols=%d md_len=%zu (old max rows=%d cols=%d md=%d)\n",
           T->n_rows, T->n_cols, strlen(T->markdown), old_tbl_rows(), old_tbl_cols(),
           640);

    /* ---- PDF stream ---- */
    if (stat(pdf, &st) != 0) {
        /* fallback smaller pdf */
        pdf =
            "/home/marble/AI/AliveValleyDemo/Library/PackageCache/"
            "com.unity.testtools.codecoverage@19a841221620/Samples~/Tutorial/"
            "Worksheet.pdf";
    }
    expect(stat(pdf, &st) == 0, "pdf fixture exists");
    {
        int pc = roe_doc_pdf_page_count(pdf);
        printf("    pdf=%s pages=%d size=%lld\n", pdf, pc, (long long)st.st_size);
        expect(pc >= 1, "pdfinfo page count");
    }
    expect(roe_doc_pdf_stream(A, "paper", pdf, 0, 1, 0, &pages, &l3, &local, &teacher) ==
               0,
           "stream PDF cold");
    expect(pages >= 1, "streamed >=1 page");
    expect(local + l3 + teacher == pages, "page accounting");
    expect(local >= 1 || l3 >= 1, "got local or L3 pages");
    printf("    cold pages=%d l3=%d local=%d teacher=%d\n", pages, l3, local, teacher);

    expect(roe_doc_pdf_stream(A, "paper", pdf, 0, 1, 0, &pages2, &l32, &local2,
                              &teacher2) == 0,
           "stream PDF warm");
    expect(l32 >= l3, "warm L3 >= cold L3");
    expect(l32 >= pages2 / 2 || pages2 <= 2, "warm majority L3 when multi-page");
    printf("    warm pages=%d l3=%d local=%d teacher=%d\n", pages2, l32, local2,
           teacher2);

    /* pack export with ABI v2 */
    {
        char pack[256];
        snprintf(pack, sizeof pack, "%s/pack", art);
        expect(roe_doc_pack_export(A, pack) >= 1, "export pack ABI2");
        expect(roe_doc_pack_validate(pack) == 1, "validate pack");
    }

    roe_doc_dump_stats(A, stats, sizeof stats);
    printf("stats: %s\n", stats);

    jf = fopen("artifacts/roe_ocr_bigfile/bigfile_bench.json", "w");
    if (jf) {
        fprintf(jf,
                "{\n"
                "  \"body_limit_old\": 768,\n"
                "  \"body_limit_new\": %d,\n"
                "  \"body_kept_old_sim\": %zu,\n"
                "  \"body_kept_new\": %zu,\n"
                "  \"table_rows_old\": 64,\n"
                "  \"table_cols_old\": 32,\n"
                "  \"table_rows_new\": %d,\n"
                "  \"table_cols_new\": %d,\n"
                "  \"table_md_len\": %zu,\n"
                "  \"pdf_pages_cold\": %d,\n"
                "  \"pdf_l3_cold\": %d,\n"
                "  \"pdf_local_cold\": %d,\n"
                "  \"pdf_pages_warm\": %d,\n"
                "  \"pdf_l3_warm\": %d,\n"
                "  \"pdf_local_warm\": %d,\n"
                "  \"pdf_path\": \"%s\",\n"
                "  \"abi_version\": %d,\n"
                "  \"second_brain\": false\n"
                "}\n",
                ROE_DOC_BODY, kept_old, kept_new, T->n_rows, T->n_cols,
                strlen(T->markdown), pages, l3, local, pages2, l32, local2, pdf,
                ROE_DOC_ABI_VER);
        fclose(jf);
    }

    free(T);
    free(A);
    if (g_fail) {
        printf("ROE_ASI_OCR_BIGFILE_FAIL\n");
        return 1;
    }
    printf("ROE_ASI_OCR_BIGFILE_PASS\n");
    return 0;
}
