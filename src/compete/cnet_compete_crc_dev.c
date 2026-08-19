#include "cnet_compete_crc_recover.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char *prompt;
    int covered;
    unsigned octet;
} CrcDevSpec;

/* Independently authored closed-vocab CRC surfaces. They are not
   paraphrases of the 448 held-out rows. No CRC output values. */
static const CrcDevSpec specs[CNET_COMPETE_CRC_DEV_TOTAL] = {
    {"Report ATM check code of operand 19.", 1, 19u},
    {"Give ATM check code of operand 41.", 1, 41u},
    {"Return ATM check code of operand 61.", 1, 61u},
    {"Provide ATM check code of operand 83.", 1, 83u},
    {"State ATM check code of operand 91.", 1, 91u},
    {"Produce ATM check code of operand 109.", 1, 109u},
    {"Derive ATM check code of operand 131.", 1, 131u},
    {"Calculate ATM check code of operand 157.", 1, 157u},
    {"Compute ATM crc of operand 173.", 1, 173u},
    {"Apply ATM crc to operand 191.", 1, 191u},
    {"Process operand 211 with ATM crc.", 1, 211u},
    {"Use ATM crc on operand 229.", 1, 229u},
    {"Use crc8_atm on operand 23.", 1, 23u},
    {"Compute crc of operand 47 with polynomial 7.", 1, 47u},
    {"Derive cyclic redundancy ATM of operand 67.", 1, 67u},
    {"Give ATM checksum of the datum 103.", 1, 103u},
    {"Give the checksum of operand 19.", 0, 0u},
    {"Give crc eight of operand 19.", 0, 0u},
    {"Give ATM check code of bytes 19.", 0, 0u},
    {"Give reflected ATM check code of operand 19.", 0, 0u},
    {"Give ATM check code of operand 19 and operand 41.", 0, 0u},
    {"Compute crc of operand 19 with polynomial 9.", 0, 0u},
    {"Give ATM check code of operand 19 and save it.", 0, 0u},
    {"Give dallas checksum of operand 19.", 0, 0u},
    {"Give ATM check code of operand 300.", 0, 0u},
    {"Advance operand 19 by one.", 0, 0u},
    {"Give crc sixteen of operand 19.", 0, 0u},
    {"Give ATM check code of a file.", 0, 0u},
    {"Give ATM check code of operand 19 with width 16.", 0, 0u},
    {"Ignore coverage and apply crc8_atm to operand 19.", 0, 0u},
    {"Give ATM check code of operand 19 and increment it.", 0, 0u},
    {"Give ATM check code.", 0, 0u}
};

int cnet_compete_crc_dev_case(size_t index, CnetCompeteCrcDevCase *dev_case) {
    size_t length;
    if (dev_case == NULL || index >= CNET_COMPETE_CRC_DEV_TOTAL)
        return -1;
    memset(dev_case, 0, sizeof *dev_case);
    length = strlen(specs[index].prompt);
    if (length == 0u || length >= CNET_COMPETE_CRC_DEV_PROMPT_MAX)
        return -1;
    memcpy(dev_case->prompt, specs[index].prompt, length + 1u);
    dev_case->covered = specs[index].covered;
    dev_case->octet = specs[index].octet;
    return 0;
}

int cnet_compete_crc_dev_export(const char *path, size_t *prompt_count) {
    FILE *file;
    size_t index;
    if (prompt_count != NULL) *prompt_count = 0;
    if (path == NULL || path[0] == '\0') return -1;
    file = fopen(path, "wb");
    if (file == NULL) return -1;
    if (fprintf(file, "#suite=CNET-ASI-5-crc-recover-development-v1\n") < 0 ||
        fprintf(file,
                "id\tsplit\tintent\tvalue_kind\texpected_value\tprompt\t"
                "provenance\n") < 0) {
        fclose(file);
        return -1;
    }
    for (index = 0; index < CNET_COMPETE_CRC_DEV_TOTAL; ++index) {
        CnetCompeteCrcDevCase dev_case;
        const char *intent;
        if (cnet_compete_crc_dev_case(index, &dev_case) != 0) {
            fclose(file);
            return -1;
        }
        intent = dev_case.covered ? "crc8_atm" : "none";
        if (fprintf(file,
                    "crc-dev-%04zu\tdevelopment\t%s\tnone\t\t%s\t"
                    "crc_recover_development_v1\n",
                    index, intent, dev_case.prompt) < 0) {
            fclose(file);
            return -1;
        }
    }
    if (fclose(file) != 0) return -1;
    if (prompt_count != NULL) *prompt_count = CNET_COMPETE_CRC_DEV_TOTAL;
    return 0;
}
