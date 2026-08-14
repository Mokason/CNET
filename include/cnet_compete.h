#ifndef CNET_COMPETE_H
#define CNET_COMPETE_H

#include <stddef.h>

#ifndef CNET_COMPETE_SUITE_DATA_HEADER
#define CNET_COMPETE_SUITE_DATA_HEADER "cnet_compete_suite_data.h"
#endif
#include CNET_COMPETE_SUITE_DATA_HEADER

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
