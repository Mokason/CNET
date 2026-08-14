#ifndef CNET_COMPETE_SUITE_DATA_AUDIT_H
#define CNET_COMPETE_SUITE_DATA_AUDIT_H

#include <stddef.h>

#define CNET_COMPETE_FREEZE_COMMIT_HEX 40u

int cnet_compete_suite_data_v4_validate_buffer(
    const char *data, size_t length,
    char freeze_commit[CNET_COMPETE_FREEZE_COMMIT_HEX + 1u]);

int cnet_compete_suite_data_v4_validate_file(
    const char *path,
    char freeze_commit[CNET_COMPETE_FREEZE_COMMIT_HEX + 1u]);

int cnet_compete_suite_data_v5_validate_buffer(
    const char *data, size_t length,
    char freeze_commit[CNET_COMPETE_FREEZE_COMMIT_HEX + 1u]);

int cnet_compete_suite_data_v5_validate_file(
    const char *path,
    char freeze_commit[CNET_COMPETE_FREEZE_COMMIT_HEX + 1u]);

#endif
