#include "cnet_capsule.h"
#include "cnet_compete_capsules.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUILD_PATH_MAX 1024

typedef struct {
    size_t units;
    size_t certified_rows;
    size_t payload_bytes;
} ArtifactReport;

static int join_path(char output[BUILD_PATH_MAX], const char *root,
                     const char *leaf) {
    int written = snprintf(output, BUILD_PATH_MAX, "%s/%s", root, leaf);
    return written < 0 || written >= BUILD_PATH_MAX ? -1 : 0;
}

static size_t unit_domain(CnetCompeteUnit unit) {
    return unit == CNET_COMPETE_UNIT_POLICY ? 16u : 256u;
}

static unsigned crc8_atm(unsigned byte) {
    unsigned crc = byte & 255u;
    int bit;
    for (bit = 0; bit < 8; ++bit)
        crc = (crc & 0x80u) ? ((crc << 1) ^ 0x07u) & 255u
                            : (crc << 1) & 255u;
    return crc;
}

static unsigned reference_value(CnetCompeteUnit unit, unsigned input) {
    switch (unit) {
        case CNET_COMPETE_UNIT_INCREMENT: return (input + 1u) & 255u;
        case CNET_COMPETE_UNIT_DOUBLE: return (input * 2u) & 255u;
        case CNET_COMPETE_UNIT_ADD3: return (input + 3u) & 255u;
        case CNET_COMPETE_UNIT_MINUTES: return input * 60u;
        case CNET_COMPETE_UNIT_CRC8: return crc8_atm(input);
        case CNET_COMPETE_UNIT_POLICY:
            return ((input & 1u) || ((input & 2u) && (input & 4u))) &&
                   !(input & 8u) ? 1u : 0u;
        default: return 0;
    }
}

static void cleanup_created_root(const char *root) {
    char path[BUILD_PATH_MAX], file[BUILD_PATH_MAX];
    int unit;
    for (unit = 0; unit < CNET_COMPETE_UNIT_COUNT; ++unit) {
        const char *name = cnet_compete_unit_name((CnetCompeteUnit)unit);
        if (name == NULL || join_path(path, root, name) != 0) continue;
        if (join_path(file, path, "unit.cnb") == 0) (void)unlink(file);
        if (join_path(file, path, "manifest.cknow") == 0) (void)unlink(file);
        (void)rmdir(path);
    }
    if (join_path(path, root, ".complete") == 0) (void)unlink(path);
    (void)rmdir(root);
}

static int verify_set(const char *root, ArtifactReport *report) {
    CnetBase base;
    HybridAi coverage;
    ArtifactReport local;
    int unit, rc = -1;
    memset(&local, 0, sizeof local);
    cnb_init(&base);
    hybrid_ai_init(&coverage);
    for (unit = 0; unit < CNET_COMPETE_UNIT_COUNT; ++unit) {
        CnetCapsuleReport capsule;
        CnetCompeteUnit typed_unit = (CnetCompeteUnit)unit;
        const char *name = cnet_compete_unit_name(typed_unit);
        size_t domain = unit_domain(typed_unit);
        char path[BUILD_PATH_MAX];
        unsigned input;
        memset(&capsule, 0, sizeof capsule);
        if (name == NULL || join_path(path, root, name) != 0 ||
            cnet_capsule_import(&base, &coverage, path, &capsule) != 0 ||
            strcmp(capsule.unit, name) != 0 ||
            strcmp(capsule.scope, "exhaustive") != 0 ||
            capsule.coverage_rows != domain || capsule.payload_bytes == 0)
            goto done;
        for (input = 0; input < domain; ++input) {
            unsigned output = ~0u;
            if (cnet_compete_capsule_eval(&base, &coverage, typed_unit,
                                          input, &output) != 0 ||
                output != reference_value(typed_unit, input))
                goto done;
        }
        ++local.units;
        local.certified_rows += domain;
        local.payload_bytes += capsule.payload_bytes;
    }
    if (base.unit_count != CNET_COMPETE_UNIT_COUNT ||
        coverage.coverage_count != CNET_COMPETE_UNIT_COUNT)
        goto done;
    if (report != NULL) *report = local;
    rc = 0;
done:
    hybrid_ai_free(&coverage);
    cnb_free(&base);
    return rc;
}

static int manifest_text(char output[256], const ArtifactReport *report,
                         size_t *length_out) {
    int written = snprintf(output, 256,
                           "CNET_ASI5_CAPSULE_SET 1\n"
                           "units %zu\n"
                           "certified_rows %zu\n"
                           "payload_bytes %zu\n",
                           report->units, report->certified_rows,
                           report->payload_bytes);
    if (written < 0 || written >= 256) return -1;
    *length_out = (size_t)written;
    return 0;
}

static int write_all(int file, const char *data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t written = write(file, data + offset, length - offset);
        if (written <= 0) return -1;
        offset += (size_t)written;
    }
    return 0;
}

static int write_manifest(const char *root, const ArtifactReport *report) {
    char path[BUILD_PATH_MAX], text[256];
    size_t length = 0;
    int file;
    if (join_path(path, root, ".complete") != 0 ||
        manifest_text(text, report, &length) != 0)
        return -1;
    file = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (file < 0) return -1;
    if (write_all(file, text, length) != 0) {
        (void)close(file);
        (void)unlink(path);
        return -1;
    }
    if (close(file) != 0) {
        (void)unlink(path);
        return -1;
    }
    return 0;
}

static int verify_manifest(const char *root, const ArtifactReport *report) {
    char path[BUILD_PATH_MAX], expected[256], actual[256];
    struct stat status;
    size_t length = 0;
    FILE *file;
    if (join_path(path, root, ".complete") != 0 ||
        manifest_text(expected, report, &length) != 0 ||
        lstat(path, &status) != 0 || !S_ISREG(status.st_mode) ||
        (size_t)status.st_size != length)
        return -1;
    file = fopen(path, "rb");
    if (file == NULL) return -1;
    if (fread(actual, 1, length, file) != length || fgetc(file) != EOF ||
        ferror(file)) {
        (void)fclose(file);
        return -1;
    }
    if (fclose(file) != 0) return -1;
    return memcmp(actual, expected, length) == 0 ? 0 : -1;
}

static int build_new_set(const char *root) {
    CnetBase source;
    HybridAi coverage;
    CnetCompeteCapsuleBuildReport build;
    CnetCapsuleReport capsule;
    int unit, rc = -1;
    cnb_init(&source);
    hybrid_ai_init(&coverage);
    if (mkdir(root, 0700) != 0) goto done;
    for (unit = 0; unit < CNET_COMPETE_UNIT_COUNT; ++unit) {
        CnetCompeteUnit typed_unit = (CnetCompeteUnit)unit;
        const char *name = cnet_compete_unit_name(typed_unit);
        char path[BUILD_PATH_MAX];
        memset(&build, 0, sizeof build);
        memset(&capsule, 0, sizeof capsule);
        if (name == NULL ||
            cnet_compete_capsule_build(&source, &coverage, typed_unit,
                                       &build) != 0 ||
            build.domain_rows != unit_domain(typed_unit) ||
            build.certified_rows != build.domain_rows ||
            build.min_margin < CNET_COMPETE_CAPSULE_MARGIN_FLOOR ||
            join_path(path, root, name) != 0 ||
            cnet_capsule_export(&source, &coverage, name, path, &capsule) != 0 ||
            strcmp(capsule.scope, "exhaustive") != 0 ||
            capsule.coverage_rows != build.domain_rows)
            goto done;
    }
    rc = 0;
done:
    hybrid_ai_free(&coverage);
    cnb_free(&source);
    if (rc != 0) cleanup_created_root(root);
    return rc;
}

int main(int argc, char **argv) {
    struct stat status;
    ArtifactReport report;
    int created = 0;
    if (argc != 2 || argv[1][0] == '\0') {
        fprintf(stderr, "usage: %s CAPSULE_ROOT\n", argv[0]);
        return 2;
    }
    memset(&report, 0, sizeof report);
    if (lstat(argv[1], &status) != 0) {
        if (errno != ENOENT || build_new_set(argv[1]) != 0) goto fail;
        created = 1;
    } else if (!S_ISDIR(status.st_mode)) {
        goto fail;
    }
    if (verify_set(argv[1], &report) != 0) goto fail;
    if (created) {
        if (write_manifest(argv[1], &report) != 0) goto fail;
    } else if (verify_manifest(argv[1], &report) != 0) {
        goto fail;
    }
    printf("CNET_7B_CAPSULE_ARTIFACTS_PASS units=%zu certified_rows=%zu "
           "payload_bytes=%zu\n",
           report.units, report.certified_rows, report.payload_bytes);
    return 0;
fail:
    if (created) cleanup_created_root(argv[1]);
    printf("CNET_7B_CAPSULE_ARTIFACTS_FAIL\n");
    return 1;
}
