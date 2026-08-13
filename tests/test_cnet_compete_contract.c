#include "cnet_compete.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int make_bad_fixture(char path[64], int mode) {
    char line[1024], first[1024] = "";
    FILE *src, *dst;
    int fd, n = 0;
    strcpy(path, "/tmp/cnet_compete_contract_XXXXXX");
    fd = mkstemp(path);
    if (fd < 0) return -1;
    dst = fdopen(fd, "wb");
    src = fopen(CNET_COMPETE_FIXTURE_PATH, "rb");
    if (dst == NULL || src == NULL) {
        if (src != NULL) fclose(src);
        if (dst != NULL) fclose(dst); else close(fd);
        remove(path);
        return -1;
    }
    while (fgets(line, sizeof line, src) != NULL && n < 3) {
        if (n == 2) strcpy(first, line);
        if (fputs(line, dst) == EOF) break;
        ++n;
    }
    if (mode == 0) fputs(first, dst);                /* duplicate ID */
    if (mode == 1) fputs("bad\trow\n", dst);       /* malformed TSV */
    /* mode 2 deliberately ends after one data row: wrong cardinality. */
    fclose(src);
    if (fclose(dst) != 0) { remove(path); return -1; }
    return n == 3 ? 0 : -1;
}

static int negative_contracts(void) {
    static const int expected[3] = {
        CNET_COMPETE_ERR_DUPLICATE,
        CNET_COMPETE_ERR_FORMAT,
        CNET_COMPETE_ERR_CARDINALITY
    };
    int mode;
    for (mode = 0; mode < 3; ++mode) {
        CnetCompeteFixtureReport report;
        char path[64], error[256];
        int rc;
        if (make_bad_fixture(path, mode) != 0) return -1;
        rc = cnet_compete_validate_fixture(path, &report, error, sizeof error);
        remove(path);
        if (rc != expected[mode]) return -1;
    }
    return cnet_compete_validate_fixture(NULL, NULL, NULL, 0) ==
           CNET_COMPETE_ERR_ARGUMENT ? 0 : -1;
}

int main(void) {
    CnetCompeteFixtureReport report;
    char error[256];
    int rc = cnet_compete_validate_fixture(CNET_COMPETE_FIXTURE_PATH,
                                           &report, error, sizeof error);
    if (rc != CNET_COMPETE_OK) {
        printf("CNET_7B_COMPETE_RED rc=%d reason=%s\n", rc, error);
        return 1;
    }
    if (report.total_rows != CNET_COMPETE_TOTAL_ROWS ||
        report.covered_rows != CNET_COMPETE_COVERED_ROWS ||
        report.ood_rows != CNET_COMPETE_OOD_ROWS ||
        report.duplicate_ids != 0 || report.malformed_rows != 0) {
        printf("CNET_7B_COMPETE_RED reason=cardinality total=%zu covered=%zu "
               "ood=%zu duplicates=%zu malformed=%zu\n",
               report.total_rows, report.covered_rows, report.ood_rows,
               report.duplicate_ids, report.malformed_rows);
        return 1;
    }
    if (negative_contracts() != 0) {
        printf("CNET_7B_COMPETE_RED reason=negative_fixture_was_admitted\n");
        return 1;
    }
    printf("CNET_7B_COMPETE_CONTRACT_PASS suite=%s rows=%zu\n",
           CNET_COMPETE_SUITE_ID, report.total_rows);
    return 0;
}
