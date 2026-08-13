#ifndef CNET_COMPETE_H
#define CNET_COMPETE_H

#include <stddef.h>

#define CNET_COMPETE_SUITE_ID "CNET-ASI-5-v3"
#define CNET_COMPETE_FIXTURE_PATH "benchmarks/cnet_asi5_v3/heldout.tsv"
#define CNET_COMPETE_SYSTEM_PATH "benchmarks/cnet_asi5_v3/baseline_system.txt"
#define CNET_COMPETE_GENERATOR_PATH "tools/cnet_compete_fixture_v3.c"
#define CNET_COMPETE_FIXTURE_SHA256 \
    "ea1ee9ff33a3f1fea632e758659a315a20e24eefb2fc803d25c44a513afcaf8c"
#define CNET_COMPETE_SYSTEM_SHA256 \
    "f3ef4c33535a007728e13dd9b1c89bcc124a94261d6814136a62735d34a8bf0a"
#define CNET_COMPETE_GENERATOR_SHA256 \
    "c265a2743f81f66483c110cb77fd1d87afda57da4ffab8e5bb41e54f75fd8f44"
#define CNET_COMPETE_TOTAL_ROWS 448u
#define CNET_COMPETE_COVERED_ROWS 320u
#define CNET_COMPETE_OOD_ROWS 128u

typedef enum {
    CNET_COMPETE_LANE_INCREMENT = 0,
    CNET_COMPETE_LANE_MINUTES,
    CNET_COMPETE_LANE_CRC8,
    CNET_COMPETE_LANE_POLICY,
    CNET_COMPETE_LANE_COMPOSE3,
    CNET_COMPETE_LANE_OOD,
    CNET_COMPETE_LANE_COUNT
} CnetCompeteLane;

typedef struct {
    size_t total_rows;
    size_t covered_rows;
    size_t ood_rows;
    size_t duplicate_ids;
    size_t malformed_rows;
    size_t lane_rows[CNET_COMPETE_LANE_COUNT];
} CnetCompeteFixtureReport;

enum {
    CNET_COMPETE_OK = 0,
    CNET_COMPETE_ERR_ARGUMENT = -1,
    CNET_COMPETE_ERR_IO = -2,
    CNET_COMPETE_ERR_FORMAT = -3,
    CNET_COMPETE_ERR_CARDINALITY = -4,
    CNET_COMPETE_ERR_DUPLICATE = -5,
    CNET_COMPETE_ERR_UNIMPLEMENTED = -6
};

int cnet_compete_validate_fixture(const char *path,
                                  CnetCompeteFixtureReport *report,
                                  char *error, size_t error_cap);

#endif
