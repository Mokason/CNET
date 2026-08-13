#include "cnet_compete.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FIXTURE_LINE_CAP = 1024, FIXTURE_ID_CAP = 64 };

static void set_error(char *error, size_t cap, const char *fmt, ...) {
    va_list ap;
    if (error == NULL || cap == 0) return;
    va_start(ap, fmt);
    (void)vsnprintf(error, cap, fmt, ap);
    va_end(ap);
}

static int split_fields(char *line, char **field, size_t count) {
    size_t found = 1;
    char *p;
    field[0] = line;
    for (p = line; *p != '\0'; ++p) {
        if (*p != '\t') continue;
        if (found >= count) return -1;
        *p = '\0';
        field[found++] = p + 1;
    }
    return found == count ? 0 : -1;
}

static int parse_integer(const char *text, long *value) {
    char *end = NULL;
    const unsigned char *p;
    long parsed;
    if (text == NULL || *text == '\0') return -1;
    for (p = (const unsigned char *)text; *p != '\0'; ++p)
        if (*p < (unsigned char)'0' || *p > (unsigned char)'9') return -1;
    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return -1;
    if (value != NULL) *value = parsed;
    return 0;
}

static int identifier_valid(const char *id) {
    const unsigned char *p = (const unsigned char *)id;
    if (p == NULL || *p == '\0') return 0;
    for (; *p != '\0'; ++p) {
        if ((*p >= (unsigned char)'a' && *p <= (unsigned char)'z') ||
            (*p >= (unsigned char)'A' && *p <= (unsigned char)'Z') ||
            (*p >= (unsigned char)'0' && *p <= (unsigned char)'9') ||
            *p == (unsigned char)'_' || *p == (unsigned char)'-') continue;
        return 0;
    }
    return 1;
}

static int covered_lane(const char *intent, CnetCompeteLane *lane) {
    static const char *names[CNET_COMPETE_LANE_OOD] = {
        "increment_mod256", "minutes_to_seconds", "crc8_atm",
        "access_policy_v1", "compose3_mod256"
    };
    int i;
    for (i = 0; i < CNET_COMPETE_LANE_OOD; ++i) {
        if (strcmp(intent, names[i]) == 0) {
            if (lane != NULL) *lane = (CnetCompeteLane)i;
            return 0;
        }
    }
    return -1;
}

static int prompt_valid(const char *prompt) {
    const unsigned char *p = (const unsigned char *)prompt;
    if (p == NULL || *p == '\0') return 0;
    for (; *p != '\0'; ++p)
        if (*p < 0x20u || *p == 0x7fu) return 0;
    return 1;
}

static int row_valid(char **f, CnetCompeteLane *lane) {
    long value;
    if (!identifier_valid(f[0]) || strlen(f[0]) >= FIXTURE_ID_CAP ||
        !prompt_valid(f[5]) || strcmp(f[6], "verified_spec_v1") != 0)
        return 0;
    if (strcmp(f[1], "ood") == 0) {
        if (strcmp(f[2], "none") != 0 || strcmp(f[3], "none") != 0 ||
            f[4][0] != '\0') return 0;
        *lane = CNET_COMPETE_LANE_OOD;
        return 1;
    }
    if (strcmp(f[1], "covered") != 0 || covered_lane(f[2], lane) != 0)
        return 0;
    if (*lane == CNET_COMPETE_LANE_POLICY)
        return strcmp(f[3], "string") == 0 &&
               (strcmp(f[4], "allow") == 0 || strcmp(f[4], "deny") == 0);
    if (strcmp(f[3], "integer") != 0 || parse_integer(f[4], &value) != 0)
        return 0;
    if (*lane == CNET_COMPETE_LANE_MINUTES)
        return value >= 0 && value <= 15300 && value % 60 == 0;
    return value >= 0 && value <= 255;
}

int cnet_compete_validate_fixture(const char *path,
                                  CnetCompeteFixtureReport *report,
                                  char *error, size_t error_cap) {
    CnetCompeteFixtureReport local;
    char ids[CNET_COMPETE_TOTAL_ROWS][FIXTURE_ID_CAP];
    char line[FIXTURE_LINE_CAP];
    FILE *file;
    size_t line_number = 0;
    int rc = CNET_COMPETE_OK;

    memset(&local, 0, sizeof local);
    if (report != NULL) memset(report, 0, sizeof *report);
    if (error != NULL && error_cap > 0) error[0] = '\0';
    if (path == NULL || *path == '\0') {
        set_error(error, error_cap, "fixture path is required");
        return CNET_COMPETE_ERR_ARGUMENT;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        set_error(error, error_cap, "cannot open fixture: %s", strerror(errno));
        return CNET_COMPETE_ERR_IO;
    }
    while (fgets(line, sizeof line, file) != NULL) {
        size_t len = strlen(line), i;
        char *field[7];
        CnetCompeteLane lane;
        ++line_number;
        if (len == 0 || line[len - 1] != '\n') {
            set_error(error, error_cap, "line %zu is unterminated or too long",
                      line_number);
            rc = CNET_COMPETE_ERR_FORMAT;
            break;
        }
        line[--len] = '\0';
        if (len > 0 && line[len - 1] == '\r') line[--len] = '\0';
        if (line_number == 1) {
            if (strcmp(line, "#suite=" CNET_COMPETE_SUITE_ID) != 0) {
                set_error(error, error_cap, "suite identity mismatch");
                rc = CNET_COMPETE_ERR_FORMAT;
                break;
            }
            continue;
        }
        if (line_number == 2) {
            if (strcmp(line, "id\tsplit\tintent\tvalue_kind\texpected_value\tprompt\tprovenance") != 0) {
                set_error(error, error_cap, "fixture header mismatch");
                rc = CNET_COMPETE_ERR_FORMAT;
                break;
            }
            continue;
        }
        if (local.total_rows >= CNET_COMPETE_TOTAL_ROWS ||
            split_fields(line, field, 7) != 0 || !row_valid(field, &lane)) {
            ++local.malformed_rows;
            set_error(error, error_cap, "malformed row at line %zu", line_number);
            rc = CNET_COMPETE_ERR_FORMAT;
            break;
        }
        for (i = 0; i < local.total_rows; ++i) {
            if (strcmp(ids[i], field[0]) == 0) {
                ++local.duplicate_ids;
                set_error(error, error_cap, "duplicate id %s", field[0]);
                rc = CNET_COMPETE_ERR_DUPLICATE;
                break;
            }
        }
        if (rc != CNET_COMPETE_OK) break;
        strcpy(ids[local.total_rows], field[0]);
        ++local.total_rows;
        ++local.lane_rows[lane];
        if (lane == CNET_COMPETE_LANE_OOD) ++local.ood_rows;
        else ++local.covered_rows;
    }
    if (rc == CNET_COMPETE_OK && ferror(file)) {
        set_error(error, error_cap, "fixture read failed");
        rc = CNET_COMPETE_ERR_IO;
    }
    if (fclose(file) != 0 && rc == CNET_COMPETE_OK) {
        set_error(error, error_cap, "fixture close failed");
        rc = CNET_COMPETE_ERR_IO;
    }
    if (rc == CNET_COMPETE_OK &&
        (line_number < 2 || local.total_rows != CNET_COMPETE_TOTAL_ROWS ||
         local.covered_rows != CNET_COMPETE_COVERED_ROWS ||
         local.ood_rows != CNET_COMPETE_OOD_ROWS ||
         local.lane_rows[CNET_COMPETE_LANE_INCREMENT] != 64 ||
         local.lane_rows[CNET_COMPETE_LANE_MINUTES] != 64 ||
         local.lane_rows[CNET_COMPETE_LANE_CRC8] != 64 ||
         local.lane_rows[CNET_COMPETE_LANE_POLICY] != 64 ||
         local.lane_rows[CNET_COMPETE_LANE_COMPOSE3] != 64 ||
         local.lane_rows[CNET_COMPETE_LANE_OOD] != 128)) {
        set_error(error, error_cap, "fixture cardinality mismatch");
        rc = CNET_COMPETE_ERR_CARDINALITY;
    }
    if (report != NULL) *report = local;
    return rc;
}
