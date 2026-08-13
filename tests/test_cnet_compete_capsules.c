#include "cnet_capsule.h"
#include "cnet_compete_capsules.h"
#include "router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_MANIFEST_BYTES (1024u * 1024u)

typedef struct {
    HybridAi *coverage;
    char seen[4][64];
    size_t seen_count;
} GuardTrace;

static void clean_capsule(const char *dir) {
    char path[1024];
    if (dir == NULL || dir[0] == '\0') return;
    snprintf(path, sizeof path, "%s/unit.cnb", dir); (void)remove(path);
    snprintf(path, sizeof path, "%s/manifest.cknow", dir); (void)remove(path);
    (void)rmdir(dir);
}

static int reserve_capsule_path(char *path, size_t capacity,
                                const char *stem) {
    int fd;
    if (snprintf(path, capacity, "/tmp/%s_XXXXXX", stem) < 0) return -1;
    fd = mkstemp(path);
    if (fd < 0) return -1;
    if (close(fd) != 0 || unlink(path) != 0) return -1;
    return 0;
}

static int copy_file(const char *from, const char *to) {
    FILE *input = fopen(from, "rb"), *output;
    unsigned char buffer[4096];
    size_t count;
    if (input == NULL) return -1;
    output = fopen(to, "wb");
    if (output == NULL) { fclose(input); return -1; }
    while ((count = fread(buffer, 1, sizeof buffer, input)) > 0) {
        if (fwrite(buffer, 1, count, output) != count) {
            fclose(input); fclose(output); return -1;
        }
    }
    if (ferror(input)) { fclose(input); fclose(output); return -1; }
    fclose(input);
    return fclose(output);
}

static int clone_capsule(const char *source, const char *destination) {
    char from[256], to[256];
    if (mkdir(destination, 0700) != 0) return -1;
    snprintf(from, sizeof from, "%s/unit.cnb", source);
    snprintf(to, sizeof to, "%s/unit.cnb", destination);
    if (copy_file(from, to) != 0) return -1;
    snprintf(from, sizeof from, "%s/manifest.cknow", source);
    snprintf(to, sizeof to, "%s/manifest.cknow", destination);
    return copy_file(from, to);
}

static int corrupt_payload(const char *dir) {
    char path[256];
    FILE *file;
    int byte;
    snprintf(path, sizeof path, "%s/unit.cnb", dir);
    file = fopen(path, "r+b");
    if (file == NULL || fseek(file, 64, SEEK_SET) != 0) {
        if (file != NULL) fclose(file);
        return -1;
    }
    byte = fgetc(file);
    if (byte == EOF || fseek(file, 64, SEEK_SET) != 0 ||
        fputc(byte ^ 0x5a, file) == EOF) {
        fclose(file);
        return -1;
    }
    return fclose(file);
}

static unsigned long long manifest_fnv(const char *text, size_t length) {
    unsigned long long hash = 14695981039346656037ULL;
    size_t i;
    for (i = 0; i < length; ++i) {
        hash ^= (unsigned char)text[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static int rewrite_manifest_version(const char *dir) {
    char path[256], *input = NULL, *output = NULL, *line, *save = NULL;
    FILE *file;
    long file_length;
    size_t output_length = 0, capacity;
    int replaced = 0, rc = -1;

    snprintf(path, sizeof path, "%s/manifest.cknow", dir);
    file = fopen(path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
        (file_length = ftell(file)) < 0 ||
        (unsigned long)file_length > MAX_MANIFEST_BYTES ||
        fseek(file, 0, SEEK_SET) != 0) {
        if (file != NULL) fclose(file);
        return -1;
    }
    capacity = (size_t)file_length + 128u;
    input = (char *)calloc((size_t)file_length + 1u, 1u);
    output = (char *)calloc(capacity, 1u);
    if (input == NULL || output == NULL ||
        fread(input, 1, (size_t)file_length, file) != (size_t)file_length) {
        fclose(file); file = NULL; goto done;
    }
    fclose(file);
    file = NULL;
    for (line = strtok_r(input, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        const char *emit = line;
        int written;
        if (strncmp(line, "manifest_fnv", 12) == 0) continue;
        if (strncmp(line, "cnb_version", 11) == 0) {
            emit = "cnb_version 99";
            replaced = 1;
        }
        written = snprintf(output + output_length, capacity - output_length,
                           "%s\n", emit);
        if (written < 0 || (size_t)written >= capacity - output_length)
            goto done;
        output_length += (size_t)written;
    }
    if (!replaced) goto done;
    file = fopen(path, "wb");
    if (file == NULL || fwrite(output, 1, output_length, file) != output_length ||
        fprintf(file, "manifest_fnv %llu\n",
                manifest_fnv(output, output_length)) < 0) {
        if (file != NULL) { fclose(file); file = NULL; }
        goto done;
    }
    if (fclose(file) != 0) {
        file = NULL;
        goto done;
    }
    file = NULL;
    rc = 0;
done:
    if (file != NULL) fclose(file);
    free(output); free(input);
    return rc;
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

static unsigned input_bits(CnetCompeteUnit unit) {
    return unit == CNET_COMPETE_UNIT_POLICY ? 4u : 8u;
}

static void encode_msb(double *bits, unsigned width, unsigned value) {
    unsigned i;
    for (i = 0; i < width; ++i)
        bits[i] = (double)((value >> (width - i - 1u)) & 1u);
}

static unsigned decode_msb(const double *bits, unsigned width) {
    unsigned i, value = 0;
    for (i = 0; i < width; ++i)
        value = (value << 1) | (bits[i] >= 0.5 ? 1u : 0u);
    return value;
}

static Port binary_port(const char *tag) {
    Port port = {PORT_BINARY_MSB, 8, 1, ""};
    (void)port_set_tag(&port, tag);
    return port;
}

static int coverage_guard(const char *unit,
                          const BinaryTransformNetwork *network,
                          const double *input, size_t input_length,
                          void *context) {
    GuardTrace *trace = (GuardTrace *)context;
    if (trace == NULL || trace->coverage == NULL || unit == NULL ||
        network == NULL || network->input_port_count != 1 ||
        network->output_port_count != 1)
        return -1;
    if (trace->seen_count < 4)
        snprintf(trace->seen[trace->seen_count++], 64, "%s", unit);
    return hybrid_coverage_admits_exact(trace->coverage, unit,
                                        network->input_ports[0],
                                        network->output_ports[0], input,
                                        input_length) ? 0 : -1;
}

static void collect_names(const DagNode *node, char names[4][64],
                          size_t *count) {
    size_t child, existing;
    if (node == NULL || *count >= 4) return;
    if (node->kind == DAG_PRIMITIVE && node->name != NULL) {
        int duplicate = 0;
        for (existing = 0; existing < *count; ++existing)
            if (strcmp(names[existing], node->name) == 0) duplicate = 1;
        if (!duplicate)
            snprintf(names[(*count)++], 64, "%s", node->name);
    }
    for (child = 0; child < node->child_count; ++child)
        collect_names(node->children[child], names, count);
}

static int import_must_refuse(const char *dir, const char *reason_fragment) {
    CnetBase base;
    HybridAi coverage;
    CnetCapsuleReport report;
    int refused;
    cnb_init(&base);
    hybrid_ai_init(&coverage);
    memset(&report, 0, sizeof report);
    refused = cnet_capsule_import(&base, &coverage, dir, &report) != 0 &&
              base.unit_count == 0 && coverage.coverage_count == 0 &&
              strstr(report.reject_reason, reason_fragment) != NULL;
    hybrid_ai_free(&coverage);
    cnb_free(&base);
    return refused;
}

int main(void) {
    CnetBase source, fresh;
    HybridAi source_coverage, fresh_coverage;
    CnetCompeteCapsuleBuildReport builds[CNET_COMPETE_UNIT_COUNT];
    CnetCapsuleReport capsule;
    char dirs[CNET_COMPETE_UNIT_COUNT][128] = {{0}};
    char corrupt[128] = {0}, incompatible[128] = {0};
    size_t payload_bytes = 0, certified_rows = 0;
    int rc = 1, unit;

#define REQUIRE(condition, reason)                                            \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("CNET_7B_CAPSULES_RED reason=%s\n", reason);             \
            goto done;                                                        \
        }                                                                     \
    } while (0)

    cnb_init(&source); cnb_init(&fresh);
    hybrid_ai_init(&source_coverage); hybrid_ai_init(&fresh_coverage);
    {
        CnetBase atomic_base;
        HybridAi full_coverage;
        CnetCompeteCapsuleBuildReport ignored;
        Port input_port = {PORT_BINARY_MSB, 1, 1, "atomic_in"};
        Port output_port = {PORT_BINARY_MSB, 1, 1, "atomic_out"};
        double row[1] = {0.0}, target[1] = {0.0};
        size_t index;
        cnb_init(&atomic_base);
        hybrid_ai_init(&full_coverage);
        for (index = 0; index < HYBRID_COVERAGE_MAX; ++index) {
            char owner[64];
            snprintf(owner, sizeof owner, "atomic_owner_%zu", index);
            REQUIRE(hybrid_coverage_record(&full_coverage, input_port,
                                            output_port, owner, row, target,
                                            1, 1, 1) == 0,
                    "atomicity_setup");
        }
        REQUIRE(cnet_compete_capsule_build(&atomic_base, &full_coverage,
                                            CNET_COMPETE_UNIT_INCREMENT,
                                            &ignored) != 0,
                "full_coverage_not_refused");
        REQUIRE(atomic_base.unit_count == 0 && atomic_base.blob_count == 0 &&
                    atomic_base.tag_count == 0 &&
                    full_coverage.coverage_count == HYBRID_COVERAGE_MAX,
                "build_failure_mutated_base");
        hybrid_ai_free(&full_coverage);
        cnb_free(&atomic_base);
    }
    for (unit = 0; unit < CNET_COMPETE_UNIT_COUNT; ++unit) {
        unsigned bits = input_bits((CnetCompeteUnit)unit);
        size_t domain = (size_t)1u << bits;
        unsigned input;
        REQUIRE(cnet_compete_capsule_build(&source, &source_coverage,
                                            (CnetCompeteUnit)unit,
                                            &builds[unit]) == 0,
                "build_failed");
        REQUIRE(builds[unit].domain_rows == domain &&
                    builds[unit].certified_rows == domain,
                "certification_not_exhaustive");
        REQUIRE(builds[unit].min_margin >= CNET_COMPETE_CAPSULE_MARGIN_FLOOR,
                "certification_margin");
        REQUIRE(reserve_capsule_path(dirs[unit], sizeof dirs[unit],
                                     cnet_compete_unit_name(
                                         (CnetCompeteUnit)unit)) == 0,
                "temp_path");
        memset(&capsule, 0, sizeof capsule);
        REQUIRE(cnet_capsule_export(&source, &source_coverage,
                                     cnet_compete_unit_name(
                                         (CnetCompeteUnit)unit),
                                     dirs[unit], &capsule) == 0,
                "export_failed");
        REQUIRE(strcmp(capsule.scope, "exhaustive") == 0 &&
                    capsule.coverage_rows == domain && capsule.payload_bytes > 0,
                "export_evidence");
        payload_bytes += capsule.payload_bytes;
        memset(&capsule, 0, sizeof capsule);
        REQUIRE(cnet_capsule_import(&fresh, &fresh_coverage, dirs[unit],
                                    &capsule) == 0,
                "fresh_import_failed");
        REQUIRE(strcmp(capsule.scope, "exhaustive") == 0 &&
                    capsule.coverage_rows == domain,
                "import_evidence");
        for (input = 0; input < domain; ++input) {
            unsigned output = ~0u;
            REQUIRE(cnet_compete_capsule_eval(&fresh, &fresh_coverage,
                                               (CnetCompeteUnit)unit, input,
                                               &output) == 0,
                    "replay_refused");
            REQUIRE(output == reference_value((CnetCompeteUnit)unit, input),
                    "replay_wrong");
        }
        {
            unsigned output = 0;
            REQUIRE(cnet_compete_capsule_eval(&fresh, &fresh_coverage,
                                               (CnetCompeteUnit)unit,
                                               (unsigned)domain, &output) == 1,
                    "out_of_domain_not_refused");
        }
        certified_rows += domain;
    }
    REQUIRE(source.unit_count == CNET_COMPETE_UNIT_COUNT &&
                fresh.unit_count == CNET_COMPETE_UNIT_COUNT,
            "unit_count");

    REQUIRE(reserve_capsule_path(corrupt, sizeof corrupt,
                                 "cnet_asi5_corrupt") == 0 &&
                clone_capsule(dirs[0], corrupt) == 0 &&
                corrupt_payload(corrupt) == 0,
            "corruption_setup");
    REQUIRE(import_must_refuse(corrupt, "payload_integrity"),
            "corruption_not_refused");

    REQUIRE(reserve_capsule_path(incompatible, sizeof incompatible,
                                 "cnet_asi5_incompatible") == 0 &&
                clone_capsule(dirs[0], incompatible) == 0 &&
                rewrite_manifest_version(incompatible) == 0,
            "incompatibility_setup");
    REQUIRE(import_must_refuse(incompatible, "incompatible_cnb_version"),
            "incompatibility_not_refused");

    {
        PrimitiveRegistry registry;
        DagSource sources[1];
        DagPlan plan;
        Port input_port = binary_port("byte_raw");
        Port output_port = binary_port("byte_final");
        double input_vector[8], output_vector[8];
        char members[4][64] = {{0}};
        size_t member_count = 0, member_index, skipped = 0, guard_checks = 0;
        int has_increment = 0, has_double = 0, has_add3 = 0;
        unsigned input;

        registry_init(&registry);
        memset(&plan, 0, sizeof plan);
        REQUIRE(cnb_load_registry(&fresh, &registry, &skipped) == 0 &&
                    skipped == 0 && registry.count == CNET_COMPETE_UNIT_COUNT,
                "registry_bridge");
        registry.require_certified = 1;
        encode_msb(input_vector, 8, 0);
        sources[0].type = input_port;
        sources[0].values = input_vector;
        REQUIRE(dag_plan(&registry, sources, 1, output_port, &plan) == 0 &&
                    plan.root != NULL,
                "composition_plan");
        collect_names(plan.root, members, &member_count);
        for (member_index = 0; member_index < member_count; ++member_index) {
            if (strcmp(members[member_index], "increment_mod256") == 0)
                has_increment = 1;
            if (strcmp(members[member_index], "double_mod256") == 0)
                has_double = 1;
            if (strcmp(members[member_index], "add3_mod256") == 0)
                has_add3 = 1;
        }
        REQUIRE(member_count == 3 && has_increment && has_double && has_add3,
                "composition_members");
        REQUIRE(!cnb_has_unit(&fresh, "compose3_mod256"),
                "unexpected_direct_composite");
        for (input = 0; input < 256; ++input) {
            GuardTrace trace;
            unsigned expected = ((((input + 1u) & 255u) * 2u) + 3u) & 255u;
            encode_msb(input_vector, 8, input);
            memset(output_vector, 0, sizeof output_vector);
            memset(&trace, 0, sizeof trace);
            trace.coverage = &fresh_coverage;
            plan.guard.allow = coverage_guard;
            plan.guard.ctx = &trace;
            REQUIRE(dag_execute(&plan, sources, 1, output_vector, 8) == 0,
                    "composition_execute");
            REQUIRE(decode_msb(output_vector, 8) == expected,
                    "composition_wrong");
            REQUIRE(trace.seen_count == 3 &&
                        strcmp(trace.seen[0], "increment_mod256") == 0 &&
                        strcmp(trace.seen[1], "double_mod256") == 0 &&
                        strcmp(trace.seen[2], "add3_mod256") == 0,
                    "composition_guard_not_per_hop");
            guard_checks += trace.seen_count;
        }
        REQUIRE(guard_checks == 768, "composition_guard_count");
        dag_free(&plan);
        registry_free(&registry);
    }

    printf("CNET_7B_CAPSULES_PASS units=%d certified_rows=%zu "
           "compose_rows=256 guard_checks=768 payload_bytes=%zu "
           "corruption_refused=1 incompatibility_refused=1\n",
           CNET_COMPETE_UNIT_COUNT, certified_rows, payload_bytes);
    rc = 0;
done:
    clean_capsule(incompatible);
    clean_capsule(corrupt);
    for (unit = 0; unit < CNET_COMPETE_UNIT_COUNT; ++unit)
        clean_capsule(dirs[unit]);
    hybrid_ai_free(&fresh_coverage); hybrid_ai_free(&source_coverage);
    cnb_free(&fresh); cnb_free(&source);
#undef REQUIRE
    return rc;
}
