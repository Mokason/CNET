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

typedef enum {
    CNET_COMPETE_REFUSAL_NONE = 0,
    CNET_COMPETE_REFUSAL_INTENT_PROPOSAL,
    CNET_COMPETE_REFUSAL_SEMANTIC_FRAME,
    CNET_COMPETE_REFUSAL_INTENT_DISAGREEMENT,
    CNET_COMPETE_REFUSAL_ARGUMENT,
    CNET_COMPETE_REFUSAL_COVERAGE,
    CNET_COMPETE_REFUSAL_EXECUTION
} CnetCompeteRefusal;

typedef struct {
    CnetCompeteIntent proposed_intent;
    CnetCompeteIntent semantic_intent;
    double confidence;
    CnetCompeteRefusal refusal;
} CnetCompeteDiagnostic;

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

/* Execute the identical serving path while recording answer-free development
   evidence about the admission stage. JSON output and scored behavior remain
   defined solely by CnetCompeteResult. */
int cnet_compete_runtime_execute_diagnostic(
    CnetCompeteRuntime *runtime, const char *prompt,
    CnetCompeteResult *result, CnetCompeteDiagnostic *diagnostic);

/* Serialize exactly the preregistered JSON contract, with no trailing newline.
   The caller may append a record delimiter when streaming multiple requests. */
int cnet_compete_result_json(const CnetCompeteResult *result,
                             char *output, size_t capacity);

#endif
