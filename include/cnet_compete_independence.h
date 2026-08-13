#ifndef CNET_COMPETE_INDEPENDENCE_H
#define CNET_COMPETE_INDEPENDENCE_H

#include <stddef.h>

typedef struct {
    size_t candidate_prompts;
    size_t excluded_prompts;
    size_t duplicate_prompts;
    size_t canonical_matches;
    size_t near_matches;
} CnetCompeteIndependenceReport;

enum {
    CNET_INDEPENDENCE_OK = 0,
    CNET_INDEPENDENCE_ERR_ARGUMENT = -1,
    CNET_INDEPENDENCE_ERR_IO = -2,
    CNET_INDEPENDENCE_ERR_FORMAT = -3,
    CNET_INDEPENDENCE_ERR_DUPLICATE = -4,
    CNET_INDEPENDENCE_ERR_OVERLAP = -5
};

int cnet_compete_independence_audit(
    const char *candidate_fixture,
    const char *const *prior_fixtures, size_t prior_fixture_count,
    const char *const *development_sources, size_t development_source_count,
    CnetCompeteIndependenceReport *report,
    char *error, size_t error_capacity);

int cnet_compete_independence_export_exclusions(
    const char *output_path,
    const char *const *fixture_paths, size_t fixture_count,
    const char *const *development_sources, size_t development_source_count,
    size_t *prompt_count,
    char *error, size_t error_capacity);

#endif
