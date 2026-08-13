/* Page-chunked PDF runner for ROE (digital text + optional image pages).
 *
 * Usage:
 *   roe_pdf_chunk_run book.pdf --out artifacts/roe_pdf_chunk [--max-pages N]
 *                            [--render-images]
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir((p), 0755)
#endif

static int ensure_dir(const char *path) {
    char tmp[1024];
    size_t i, len;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = 0;
            MKDIR(tmp);
            tmp[i] = c;
        }
    }
    MKDIR(path);
    return 0;
}

static int page_count(const char *pdf) {
    char cmd[2048], line[512];
    FILE *p;
    int pages = -1;
    snprintf(cmd, sizeof cmd, "pdfinfo \"%s\"", pdf);
    p = popen(cmd, "r");
    if (!p) return -1;
    while (fgets(line, sizeof line, p)) {
        if (strncmp(line, "Pages:", 6) == 0) {
            pages = atoi(line + 6);
            break;
        }
    }
    pclose(p);
    return pages;
}

static char *pdftotext_page(const char *pdf, int page) {
    char cmd[2048], tmp[512];
    FILE *f;
    long sz;
    char *buf;
#ifdef _WIN32
    snprintf(tmp, sizeof tmp, "roe_pdf_page_%d.txt", page);
#else
    snprintf(tmp, sizeof tmp, "/tmp/roe_pdf_page_%d_%d.txt", (int)getpid(), page);
#endif
    snprintf(cmd, sizeof cmd,
             "pdftotext -q -layout -f %d -l %d \"%s\" \"%s\"",
             page, page, pdf, tmp);
    system(cmd);
    f = fopen(tmp, "rb");
    if (!f) {
        remove(tmp);
        buf = (char *)malloc(1);
        if (buf) buf[0] = 0;
        return buf;
    }
    fseek(f, 0, SEEK_END); sz = ftell(f); rewind(f);
    if (sz < 0) sz = 0;
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); remove(tmp); return NULL; }
    if (sz) fread(buf, 1, (size_t)sz, f);
    buf[sz] = 0;
    fclose(f);
    remove(tmp);
    return buf;
}

static void json_escape(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\r') fputs("\\r", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

int main(int argc, char **argv) {
    const char *pdf = NULL, *out = "artifacts/roe_pdf_chunk";
    int max_pages = 512, render_images = 0, ai;
    int n, lim, i, empty = 0;
    long total_chars = 0;
    char pages_dir[1024], report_path[1024];
    FILE *rep;

    typedef struct { int page, chars, alnum, empty; } Row;
    Row *rows;

    for (ai = 1; ai < argc; ai++) {
        if (strcmp(argv[ai], "--out") == 0 && ai + 1 < argc) out = argv[++ai];
        else if (strcmp(argv[ai], "--max-pages") == 0 && ai + 1 < argc) max_pages = atoi(argv[++ai]);
        else if (strcmp(argv[ai], "--render-images") == 0) render_images = 1;
        else if (!pdf) pdf = argv[ai];
        else {
            fprintf(stderr, "usage: %s book.pdf [--out DIR] [--max-pages N] [--render-images]\n",
                    argv[0]);
            return 2;
        }
    }
    if (!pdf) {
        fprintf(stderr, "usage: %s book.pdf [--out DIR] [--max-pages N] [--render-images]\n",
                argv[0]);
        return 2;
    }

    ensure_dir(out);
    snprintf(pages_dir, sizeof pages_dir, "%s/pages", out);
    ensure_dir(pages_dir);

    n = page_count(pdf);
    if (n < 1) {
        printf("bad pdf page count %d\n", n);
        return 2;
    }
    lim = n < max_pages ? n : max_pages;
    printf("[pdf] %s pages=%d run=%d\n", pdf, n, lim);

    rows = (Row *)calloc((size_t)lim, sizeof(Row));
    if (!rows) return 1;

    for (i = 1; i <= lim; i++) {
        char *text = pdftotext_page(pdf, i);
        int alnum = 0, chars;
        char page_path[1100];
        FILE *pf;
        const char *t;
        if (!text) text = (char *)calloc(1, 1);
        chars = (int)strlen(text);
        for (t = text; *t; t++)
            if (isalnum((unsigned char)*t)) alnum++;
        snprintf(page_path, sizeof page_path, "%s/page_%04d.txt", pages_dir, i);
        pf = fopen(page_path, "w");
        if (pf) { fputs(text, pf); fclose(pf); }
        rows[i - 1].page = i;
        rows[i - 1].chars = chars;
        rows[i - 1].alnum = alnum;
        rows[i - 1].empty = alnum < 20;
        if (rows[i - 1].empty) empty++;
        total_chars += chars;
        if (i % 10 == 0 || i == lim)
            printf("  page %d/%d chars=%d empty=%s\n",
                   i, lim, chars, rows[i - 1].empty ? "true" : "false");
        free(text);
    }

    if (render_images) {
        char img_dir[1024], cmd[2048];
        snprintf(img_dir, sizeof img_dir, "%s/images", out);
        ensure_dir(img_dir);
        snprintf(cmd, sizeof cmd, "pdftoppm -png -f 1 -l %d \"%s\" \"%s/p\"",
                 lim, pdf, img_dir);
        system(cmd);
    }

    snprintf(report_path, sizeof report_path, "%s/chunk_report.json", out);
    rep = fopen(report_path, "w");
    if (rep) {
        fprintf(rep, "{\n");
        fprintf(rep, "  \"pdf\": "); json_escape(rep, pdf); fprintf(rep, ",\n");
        fprintf(rep, "  \"pages_total\": %d,\n", n);
        fprintf(rep, "  \"pages_run\": %d,\n", lim);
        fprintf(rep, "  \"total_chars\": %ld,\n", total_chars);
        fprintf(rep, "  \"empty_or_image_pages\": %d,\n", empty);
        fprintf(rep, "  \"avg_chars\": %.17g,\n", (double)total_chars / (lim > 0 ? lim : 1));
        fprintf(rep, "  \"second_brain\": false,\n");
        fprintf(rep, "  \"note\": \"Streamed page-by-page; never one giant image.\",\n");
        fprintf(rep, "  \"rows\": [\n");
        for (i = 0; i < lim; i++) {
            fprintf(rep,
                    "    {\"page\": %d, \"chars\": %d, \"alnum\": %d, \"empty\": %s}%s\n",
                    rows[i].page, rows[i].chars, rows[i].alnum,
                    rows[i].empty ? "true" : "false",
                    i + 1 < lim ? "," : "");
        }
        fprintf(rep, "  ]\n}\n");
        fclose(rep);
    }

    printf("{\n");
    printf("  \"pdf\": \"%s\",\n", pdf);
    printf("  \"pages_total\": %d,\n", n);
    printf("  \"pages_run\": %d,\n", lim);
    printf("  \"total_chars\": %ld,\n", total_chars);
    printf("  \"empty_or_image_pages\": %d,\n", empty);
    printf("  \"avg_chars\": %.17g,\n", (double)total_chars / (lim > 0 ? lim : 1));
    printf("  \"second_brain\": false,\n");
    printf("  \"note\": \"Streamed page-by-page; never one giant image.\"\n");
    printf("}\n");
    printf("ROE_PDF_CHUNK_PASS\n");
    free(rows);
    return 0;
}
