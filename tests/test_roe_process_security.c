/* External document-tool boundary gate.
 * RED marker: ROE_PROCESS_ARGV_RED
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../include/cnet_roe_doc.h"
#include "../include/cnet_roe_ocr.h"

static int fails;

static void check(int ok, const char *name) {
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

static int write_file(const char *path, const char *body, mode_t mode) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (fputs(body, f) < 0 || fclose(f) != 0) return -1;
    return chmod(path, mode);
}

static int read_file(const char *path, char *out, size_t cap) {
    FILE *f;
    size_t n;
    if (!out || cap < 2) return -1;
    out[0] = '\0';
    f = fopen(path, "rb");
    if (!f) return -1;
    n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    fclose(f);
    return 0;
}

int main(void) {
    static const char pdftotext_script[] =
        "#!/bin/sh\n"
        "printf '%s\\n' \"$@\" > \"$CNET_ROE_ARGV_LOG\"\n"
        "for last do :; done\n"
        "printf 'SAFE EXTRACTED TEXT WITH MORE THAN FORTY ALPHANUMERIC CHARACTERS 1234567890\\n' > \"$last\"\n";
    static const char pdfinfo_script[] =
        "#!/bin/sh\n"
        "printf '%s\\n' \"$@\" > \"$CNET_ROE_ARGV_LOG\"\n"
        "printf 'Pages: 7\\n'\n";
    static const char tesseract_script[] =
        "#!/bin/sh\n"
        "printf '%s\\n' \"$@\" > \"$CNET_ROE_ARGV_LOG\"\n"
        "printf 'SAFE TESSERACT TEXT WITH MORE THAN FORTY ALPHANUMERIC CHARACTERS 1234567890\\n' > \"$2.txt\"\n";
    char root[] = "/tmp/cnet_roe_argv_XXXXXX";
    char cwd[ROE_PATH_MAX] = {0};
    char bin_dir[ROE_PATH_MAX], pdf_tool[ROE_PATH_MAX * 2];
    char info_tool[ROE_PATH_MAX * 2], tess_tool[ROE_PATH_MAX * 2];
    char quoted_pdf[ROE_PATH_MAX], unquoted_pdf[ROE_PATH_MAX];
    char quoted_image[ROE_PATH_MAX], leading_pdf[ROE_PATH_MAX];
    char batch_dir[ROE_PATH_MAX], batch_file[ROE_PATH_MAX * 2];
    char argv_log[ROE_PATH_MAX], path_env[ROE_PATH_MAX * 3];
    char body[ROE_DOC_BODY], log_body[ROE_PATH_MAX * 2];
    const char *old_path = getenv("PATH");
    char *saved_path = old_path ? strdup(old_path) : NULL;
    char *dir = mkdtemp(root);
    RoeOcr *ocr = NULL;
    RoeOcrResult ocr_result;
    RoeDocAsset *doc = NULL;
    RoeDocReply reply;

    check(dir != NULL, "create process-boundary temp directory");
    check(getcwd(cwd, sizeof cwd) != NULL, "capture process-boundary cwd");
    if (!dir || !cwd[0]) return 1;

    snprintf(bin_dir, sizeof bin_dir, "%s/bin", dir);
    snprintf(pdf_tool, sizeof pdf_tool, "%s/pdftotext", bin_dir);
    snprintf(info_tool, sizeof info_tool, "%s/pdfinfo", bin_dir);
    snprintf(tess_tool, sizeof tess_tool, "%s/tesseract", bin_dir);
    snprintf(quoted_pdf, sizeof quoted_pdf,
             "%s/doc'$(touch PWNED_DOC)'.pdf", dir);
    snprintf(unquoted_pdf, sizeof unquoted_pdf,
             "%s/doc$(touch PWNED_OCR).pdf", dir);
    snprintf(quoted_image, sizeof quoted_image,
             "%s/image'$(touch PWNED_TESS)'.png", dir);
    snprintf(leading_pdf, sizeof leading_pdf, "%s/-leading.pdf", dir);
    snprintf(batch_dir, sizeof batch_dir,
             "%s/batch'$(touch PWNED_FIND)'", dir);
    snprintf(batch_file, sizeof batch_file, "%s/safe.txt", batch_dir);
    snprintf(argv_log, sizeof argv_log, "%s/argv.log", dir);
    check(mkdir(bin_dir, 0700) == 0, "create fake tool directory");
    check(write_file(pdf_tool, pdftotext_script, 0700) == 0,
          "install fake pdftotext");
    check(write_file(info_tool, pdfinfo_script, 0700) == 0,
          "install fake pdfinfo");
    check(write_file(tess_tool, tesseract_script, 0700) == 0,
          "install fake tesseract");
    check(write_file(quoted_pdf, "PDF", 0600) == 0,
          "create quoted hostile PDF path");
    check(write_file(unquoted_pdf, "PDF", 0600) == 0,
          "create unquoted hostile PDF path");
    check(write_file(quoted_image, "PNG", 0600) == 0,
          "create quoted hostile image path");
    check(write_file(leading_pdf, "PDF", 0600) == 0,
          "create leading-dash PDF path");
    check(mkdir(batch_dir, 0700) == 0,
          "create hostile batch directory path");
    check(write_file(batch_file,
                     "SAFE BATCH TEXT WITH MORE THAN FORTY ALPHANUMERIC "
                     "CHARACTERS 1234567890", 0600) == 0,
          "create batch document fixture");

    snprintf(path_env, sizeof path_env, "%s:%s", bin_dir,
             old_path ? old_path : "");
    check(setenv("PATH", path_env, 1) == 0, "install fake tool PATH");
    check(setenv("CNET_ROE_ARGV_LOG", argv_log, 1) == 0,
          "install argv capture path");
    check(chdir(dir) == 0, "enter process-boundary temp directory");

    remove("PWNED_DOC");
    check(roe_doc_pdftotext(quoted_pdf, body, sizeof body) == 0,
          "pdftotext accepts quotes and shell syntax as data");
    check(access("PWNED_DOC", F_OK) != 0,
          "pdftotext path cannot execute shell syntax");

    remove("PWNED_PAGE");
    {
        char page_pdf[ROE_PATH_MAX];
        snprintf(page_pdf, sizeof page_pdf,
                 "%s/page'$(touch PWNED_PAGE)'.pdf", dir);
        check(write_file(page_pdf, "PDF", 0600) == 0,
              "create hostile page PDF path");
        check(roe_doc_pdftotext_page(page_pdf, 2, body, sizeof body) == 0,
              "page extraction accepts shell syntax as data");
        check(access("PWNED_PAGE", F_OK) != 0,
              "page extraction path cannot execute shell syntax");
        remove(page_pdf);
    }

    remove("PWNED_INFO");
    {
        char info_pdf[ROE_PATH_MAX];
        snprintf(info_pdf, sizeof info_pdf,
                 "%s/info'$(touch PWNED_INFO)'.pdf", dir);
        check(write_file(info_pdf, "PDF", 0600) == 0,
              "create hostile pdfinfo path");
        check(roe_doc_pdf_page_count(info_pdf) == 7,
              "pdfinfo accepts shell syntax as data");
        check(access("PWNED_INFO", F_OK) != 0,
              "pdfinfo path cannot execute shell syntax");
        remove(info_pdf);
    }

    ocr = (RoeOcr *)calloc(1, sizeof *ocr);
    check(ocr != NULL, "allocate OCR router");
    if (ocr) {
        roe_ocr_init(ocr);
        remove("PWNED_OCR");
        check(roe_ocr_file(ocr, unquoted_pdf, &ocr_result) == 0,
              "OCR PDF accepts shell syntax as data");
        check(access("PWNED_OCR", F_OK) != 0,
              "OCR PDF path cannot execute shell syntax");
        free(ocr);
    }

    doc = (RoeDocAsset *)calloc(1, sizeof *doc);
    check(doc != NULL, "allocate document router");
    if (doc) {
        roe_doc_init(doc);
        check(roe_doc_add_corpus(doc, "security") == 0,
              "add document security corpus");
        remove("PWNED_TESS");
        check(roe_doc_route(doc, "security", quoted_image, "x", 0,
                            &reply) == 0,
              "tesseract accepts shell syntax as data");
        check(access("PWNED_TESS", F_OK) != 0,
              "tesseract path cannot execute shell syntax");
        remove("PWNED_FIND");
        {
            int n_ok = 0;
            int n_teacher = 0;
            check(roe_doc_batch_distill(doc, "security", batch_dir, 1,
                                        &n_ok, &n_teacher) >= 0,
                  "batch scan accepts shell syntax as path data");
            check(access("PWNED_FIND", F_OK) != 0,
                  "batch directory cannot execute shell syntax");
        }
        free(doc);
    }

    check(chdir(dir) == 0, "stay in process-boundary directory");
    check(roe_doc_pdftotext("-leading.pdf", body, sizeof body) == 0,
          "leading-dash PDF remains a file operand");
    check(read_file(argv_log, log_body, sizeof log_body) == 0,
          "read captured converter argv");
    check(strstr(log_body, leading_pdf) != NULL,
          "converter receives canonical absolute input path");

    remove("PWNED_DOC");
    remove("PWNED_PAGE");
    remove("PWNED_INFO");
    remove("PWNED_OCR");
    remove("PWNED_TESS");
    remove("PWNED_FIND");
    check(chdir(cwd) == 0, "restore process-boundary cwd");
    if (saved_path) setenv("PATH", saved_path, 1);
    else unsetenv("PATH");
    free(saved_path);
    unsetenv("CNET_ROE_ARGV_LOG");
    remove(argv_log);
    remove(quoted_pdf);
    remove(unquoted_pdf);
    remove(quoted_image);
    remove(leading_pdf);
    remove(batch_file);
    rmdir(batch_dir);
    remove(pdf_tool);
    remove(info_tool);
    remove(tess_tool);
    rmdir(bin_dir);
    rmdir(dir);

    if (fails) {
        printf("ROE_PROCESS_SECURITY_FAIL fails=%d\n", fails);
        return 1;
    }
    printf("ROE_PROCESS_SECURITY_PASS argv_only hostile_paths_refused_as_code\n");
    return 0;
}
