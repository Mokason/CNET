#include "cnet_compete_suite_data_audit.h"

#include <stdio.h>

int main(int argc, char **argv) {
    char commit[CNET_COMPETE_FREEZE_COMMIT_HEX + 1u];
    if (argc != 2) {
        fprintf(stderr, "usage: %s SUITE_DATA_HEADER\n", argv[0]);
        return 2;
    }
    if (cnet_compete_suite_data_v4_validate_file(argv[1], commit) != 0) {
        printf("CNET_7B_V4_SUITE_DATA_AUDIT_FAIL\n");
        return 1;
    }
    printf("CNET_7B_V4_SUITE_DATA_AUDIT_PASS s4=%s\n", commit);
    return 0;
}
