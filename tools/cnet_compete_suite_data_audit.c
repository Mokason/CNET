#include "cnet_compete_suite_data_audit.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    char commit[CNET_COMPETE_FREEZE_COMMIT_HEX + 1u];
    const char *path;
    int v5 = 0;
    if (argc == 3 && strcmp(argv[1], "--v5") == 0) {
        v5 = 1;
        path = argv[2];
    } else if (argc == 2) {
        path = argv[1];
    } else {
        fprintf(stderr, "usage: %s SUITE_DATA_HEADER\n"
                        "       %s --v5 SUITE_DATA_HEADER\n",
                argv[0], argv[0]);
        return 2;
    }
    if (v5) {
        if (cnet_compete_suite_data_v5_validate_file(path, commit) != 0) {
            printf("CNET_7B_V5_SUITE_DATA_AUDIT_FAIL\n");
            return 1;
        }
        printf("CNET_7B_V5_SUITE_DATA_AUDIT_PASS s5=%s\n", commit);
        return 0;
    }
    if (cnet_compete_suite_data_v4_validate_file(path, commit) != 0) {
        printf("CNET_7B_V4_SUITE_DATA_AUDIT_FAIL\n");
        return 1;
    }
    printf("CNET_7B_V4_SUITE_DATA_AUDIT_PASS s4=%s\n", commit);
    return 0;
}
