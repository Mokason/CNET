#ifndef CNET_COMPETE_CRC_RECOVER_H
#define CNET_COMPETE_CRC_RECOVER_H

#include "cnet_compete_runtime.h"

#include <stddef.h>

#define CNET_COMPETE_CRC_DEV_COVERED 16u
#define CNET_COMPETE_CRC_DEV_OOD 16u
#define CNET_COMPETE_CRC_DEV_TOTAL \
    (CNET_COMPETE_CRC_DEV_COVERED + CNET_COMPETE_CRC_DEV_OOD)
#define CNET_COMPETE_CRC_DEV_PROMPT_MAX 256u

typedef struct {
    char prompt[CNET_COMPETE_CRC_DEV_PROMPT_MAX];
    int covered;
    unsigned octet;
} CnetCompeteCrcDevCase;

/* Answer-free CRC recoverer development cases. Covered rows bind ATM CRC-8
   plus one in-range octet. OOD rows are paired refusals. No CRC output
   values are stored. */
int cnet_compete_crc_dev_case(size_t index, CnetCompeteCrcDevCase *dev_case);
int cnet_compete_crc_dev_export(const char *path, size_t *prompt_count);

#endif
