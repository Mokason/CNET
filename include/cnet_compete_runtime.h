#ifndef CNET_COMPETE_RUNTIME_H
#define CNET_COMPETE_RUNTIME_H

#include <stddef.h>

#include "cnet_compete_intent.h"

typedef struct CnetCompeteRuntime CnetCompeteRuntime;

typedef struct {
    size_t imported_units;
    size_t certified_rows;
    size_t capsule_payload_bytes;
    size_t composition_members;
    long base_parameters;
    size_t base_artifact_bytes;
    double intent_threshold;
} CnetCompeteRuntimeReport;

typedef struct {
    int answered;
    CnetCompeteIntent intent;
    unsigned value;
    double confidence;
    size_t composition_guard_checks;
} CnetCompeteResult;

/* Load the fixed intent base and six independently serialized capsules. Every
   capsule is integrity checked, imported into a fresh base, and re-certified;
   the three-hop composition plan is built only from certified registry units. */
int cnet_compete_runtime_load(const char *model_path,
                              const char *metadata_path,
                              const char *capsule_root,
                              CnetCompeteRuntime **runtime_out,
                              CnetCompeteRuntimeReport *report);
void cnet_compete_runtime_free(CnetCompeteRuntime *runtime);

/* Execute one request. A representational, coverage, or argument refusal is a
   successful abstention (return 0, answered=0). Internal certification or
   execution faults return -1 and still leave the result as an abstention. */
int cnet_compete_runtime_execute(CnetCompeteRuntime *runtime,
                                 const char *prompt,
                                 CnetCompeteResult *result);

/* Serialize exactly the preregistered JSON contract, with no trailing newline.
   The caller may append a record delimiter when streaming multiple requests. */
int cnet_compete_result_json(const CnetCompeteResult *result,
                             char *output, size_t capacity);

#endif
