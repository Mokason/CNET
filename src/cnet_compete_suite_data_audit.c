#include "cnet_compete_suite_data_audit.h"

#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SUITE_DATA_MAX_BYTES 4096u

typedef struct {
    const char *data;
    size_t length;
    size_t offset;
} SuiteDataCursor;

static int next_line(SuiteDataCursor *cursor, const char **line,
                     size_t *length) {
    size_t start, end;
    if (cursor == NULL || line == NULL || length == NULL ||
        cursor->offset >= cursor->length)
        return -1;
    start = cursor->offset;
    end = start;
    while (end < cursor->length && cursor->data[end] != '\n') ++end;
    if (end == cursor->length) return -1;
    *line = cursor->data + start;
    *length = end - start;
    cursor->offset = end + 1u;
    return 0;
}

static int expect_line(SuiteDataCursor *cursor, const char *expected) {
    const char *line;
    size_t length, expected_length;
    if (expected == NULL || next_line(cursor, &line, &length) != 0) return -1;
    expected_length = strlen(expected);
    return length == expected_length &&
                   memcmp(line, expected, expected_length) == 0
               ? 0
               : -1;
}

static int lowercase_hex_line(SuiteDataCursor *cursor, const char *prefix,
                              size_t digits, char *value) {
    const char *line;
    size_t length, prefix_length, index;
    int nonzero = 0;
    if (prefix == NULL || value == NULL ||
        next_line(cursor, &line, &length) != 0)
        return -1;
    prefix_length = strlen(prefix);
    if (length != prefix_length + digits + 1u ||
        memcmp(line, prefix, prefix_length) != 0 || line[length - 1u] != '"')
        return -1;
    for (index = 0; index < digits; ++index) {
        unsigned char byte = (unsigned char)line[prefix_length + index];
        if (!((byte >= '0' && byte <= '9') ||
              (byte >= 'a' && byte <= 'f')))
            return -1;
        if (byte != '0') nonzero = 1;
        value[index] = (char)byte;
    }
    if (!nonzero) return -1;
    value[digits] = '\0';
    return 0;
}

int cnet_compete_suite_data_v4_validate_buffer(
    const char *data, size_t length,
    char freeze_commit[CNET_COMPETE_FREEZE_COMMIT_HEX + 1u]) {
    SuiteDataCursor cursor;
    char digest[65];
    if (data == NULL || freeze_commit == NULL || length == 0 ||
        length > SUITE_DATA_MAX_BYTES)
        return -1;
    freeze_commit[0] = '\0';
    cursor.data = data;
    cursor.length = length;
    cursor.offset = 0;
    if (expect_line(&cursor, "#ifndef CNET_COMPETE_SUITE_DATA_V4_H") != 0 ||
        expect_line(&cursor, "#define CNET_COMPETE_SUITE_DATA_V4_H") != 0 ||
        expect_line(&cursor, "") != 0 ||
        expect_line(&cursor,
                    "#define CNET_COMPETE_SUITE_ID \"CNET-ASI-5-v4\"") != 0 ||
        lowercase_hex_line(
            &cursor,
            "#define CNET_COMPETE_CANDIDATE_FREEZE_COMMIT \"",
            CNET_COMPETE_FREEZE_COMMIT_HEX, freeze_commit) != 0 ||
        expect_line(
            &cursor,
            "#define CNET_COMPETE_FIXTURE_PATH \"benchmarks/cnet_asi5_v4/heldout.tsv\"") != 0 ||
        expect_line(
            &cursor,
            "#define CNET_COMPETE_SYSTEM_PATH \"benchmarks/cnet_asi5_v4/baseline_system.txt\"") != 0 ||
        expect_line(
            &cursor,
            "#define CNET_COMPETE_GENERATOR_PATH \"tools/cnet_compete_fixture_v4.c\"") != 0 ||
        expect_line(
            &cursor,
            "#define CNET_COMPETE_FIXTURE_PROVENANCE \"verified_spec_v4\"") != 0 ||
        lowercase_hex_line(&cursor,
                           "#define CNET_COMPETE_FIXTURE_SHA256 \"", 64u,
                           digest) != 0 ||
        lowercase_hex_line(&cursor,
                           "#define CNET_COMPETE_SYSTEM_SHA256 \"", 64u,
                           digest) != 0 ||
        lowercase_hex_line(&cursor,
                           "#define CNET_COMPETE_GENERATOR_SHA256 \"", 64u,
                           digest) != 0 ||
        expect_line(&cursor, "#define CNET_COMPETE_TOTAL_ROWS 448u") != 0 ||
        expect_line(&cursor, "#define CNET_COMPETE_COVERED_ROWS 320u") != 0 ||
        expect_line(&cursor, "#define CNET_COMPETE_OOD_ROWS 128u") != 0 ||
        expect_line(
            &cursor,
            "#define CNET_COMPETE_STATE_ROOT \"/home/marble/.local/state/cnet/cnet_asi5_v4\"") != 0 ||
        expect_line(&cursor, "") != 0 || expect_line(&cursor, "#endif") != 0 ||
        cursor.offset != cursor.length)
        return -1;
    return 0;
}

int cnet_compete_suite_data_v4_validate_file(
    const char *path,
    char freeze_commit[CNET_COMPETE_FREEZE_COMMIT_HEX + 1u]) {
    struct stat before, opened;
    char buffer[SUITE_DATA_MAX_BYTES];
    size_t offset = 0;
    int descriptor, rc = -1;
    if (path == NULL || freeze_commit == NULL || lstat(path, &before) != 0 ||
        !S_ISREG(before.st_mode) || before.st_nlink != 1 || before.st_size <= 0 ||
        (uintmax_t)before.st_size > sizeof buffer)
        return -1;
    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0 || fstat(descriptor, &opened) != 0 ||
        !S_ISREG(opened.st_mode) || opened.st_nlink != 1 ||
        opened.st_dev != before.st_dev || opened.st_ino != before.st_ino ||
        opened.st_size != before.st_size) {
        if (descriptor >= 0) (void)close(descriptor);
        return -1;
    }
    while (offset < (size_t)opened.st_size) {
        ssize_t count = read(descriptor, buffer + offset,
                             (size_t)opened.st_size - offset);
        if (count <= 0) goto done;
        offset += (size_t)count;
    }
    if (cnet_compete_suite_data_v4_validate_buffer(
            buffer, offset, freeze_commit) != 0)
        goto done;
    rc = 0;
done:
    if (close(descriptor) != 0) rc = -1;
    return rc;
}

int cnet_compete_suite_data_v5_validate_buffer(
    const char *data, size_t length,
    char freeze_commit[CNET_COMPETE_FREEZE_COMMIT_HEX + 1u]) {
    (void)data;
    (void)length;
    if (freeze_commit != NULL) freeze_commit[0] = '\0';
    return -1;
}

int cnet_compete_suite_data_v5_validate_file(
    const char *path,
    char freeze_commit[CNET_COMPETE_FREEZE_COMMIT_HEX + 1u]) {
    (void)path;
    if (freeze_commit != NULL) freeze_commit[0] = '\0';
    return -1;
}
