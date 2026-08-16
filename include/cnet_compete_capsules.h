#ifndef CNET_COMPETE_CAPSULES_H
#define CNET_COMPETE_CAPSULES_H

#include <stddef.h>

#include "base.h"
#include "hybrid_ai.h"

#define CNET_COMPETE_CAPSULE_MARGIN_FLOOR 0.49

typedef enum {
    CNET_COMPETE_UNIT_INCREMENT = 0,
    CNET_COMPETE_UNIT_DOUBLE,
    CNET_COMPETE_UNIT_ADD3,
    CNET_COMPETE_UNIT_MINUTES,
    CNET_COMPETE_UNIT_CRC8,
    CNET_COMPETE_UNIT_POLICY,
    CNET_COMPETE_UNIT_COUNT
} CnetCompeteUnit;

typedef struct {
    size_t domain_rows;
    size_t input_bits;
    size_t output_bits;
    size_t certified_rows;
    double min_margin;
    unsigned long long behavior_digest;
} CnetCompeteCapsuleBuildReport;

const char *cnet_compete_unit_name(CnetCompeteUnit unit);
int cnet_compete_capsule_build(CnetBase *base, HybridAi *coverage,
                               CnetCompeteUnit unit,
                               CnetCompeteCapsuleBuildReport *report);
int cnet_compete_capsule_eval(const CnetBase *base, const HybridAi *coverage,
                              CnetCompeteUnit unit, unsigned input,
                              unsigned *output);

#endif
