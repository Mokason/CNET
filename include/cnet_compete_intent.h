#ifndef CNET_COMPETE_INTENT_H
#define CNET_COMPETE_INTENT_H

#include <stddef.h>

#define CNET_COMPETE_INTENT_BUCKETS 1021
#define CNET_COMPETE_INTENT_LABEL_BASE 1024
#define CNET_COMPETE_INTENT_VOCAB 1030
#define CNET_COMPETE_INTENT_CONTEXT 20
#define CNET_COMPETE_INTENT_V5_CONTEXT 32
#define CNET_COMPETE_INTENT_EMBED 32
#define CNET_COMPETE_INTENT_HIDDEN 128
#define CNET_COMPETE_INTENT_SEED 20260813u
#define CNET_COMPETE_INTENT_PROVENANCE \
    "external_verified_spec_dual_head_v4"
#define CNET_COMPETE_INTENT_V5_PROVENANCE \
    "external_verified_spec_dual_head_v5"
#define CNET_COMPETE_INTENT_V5_SOURCE_EXAMPLES 1176u
#define CNET_COMPETE_INTENT_V5_TRAIN_EXAMPLES 1428u
#define CNET_COMPETE_INTENT_V5_TRAIN_STEPS 214200u

typedef enum {
    CNET_INTENT_INCREMENT = 0,
    CNET_INTENT_MINUTES,
    CNET_INTENT_CRC8,
    CNET_INTENT_POLICY,
    CNET_INTENT_COMPOSE3,
    CNET_INTENT_ABSTAIN,
    CNET_INTENT_COUNT
} CnetCompeteIntent;

typedef struct CnetCompeteIntentModel CnetCompeteIntentModel;

typedef struct {
    unsigned seed;
    int vocabulary;
    int context;
    int embedding;
    int hidden;
    long parameters;
    size_t source_examples;
    size_t train_examples;
    size_t train_steps;
    double final_mean_loss;
    double threshold;
    size_t calibration_covered;
    size_t calibration_answered;
    size_t calibration_correct;
    size_t calibration_wrong;
    size_t calibration_ood;
    size_t calibration_ood_abstained;
    size_t packed_parity_mismatches;
    double packed_max_nll_delta;
    size_t artifact_bytes;
    unsigned long long artifact_fnv;
    char provenance[64];
} CnetCompeteIntentReport;

const char *cnet_compete_intent_name(CnetCompeteIntent intent);

/* Lowercase ASCII word tokens and normalized numeric spans are mapped into
   fixed FNV-1a buckets. The unique buckets are sorted so intent grounding is
   insensitive to harmless word order. Returns 0, or 1 when the prompt is not
   safely representable (non-ASCII, empty, overlong token, or >20 buckets). */
int cnet_compete_intent_tokenize(const char *prompt,
                                 int tokens[CNET_COMPETE_INTENT_CONTEXT]);

/* Export every expanded training and calibration prompt without answers.
   This is benchmark-contamination evidence, never a serving input. */
int cnet_compete_intent_export_development_corpus(const char *path,
                                                  size_t *prompt_count);

/* Deterministic native-C training from the built-in external specification
   generator plus an answer-free, manifest-frozen semantic intent corpus. It
   never consumes CNET answers or a held-out fixture. */
int cnet_compete_intent_train(const char *artifact_path,
                              const char *metadata_path,
                              const char *semantic_corpus_path,
                              CnetCompeteIntentReport *report);

/* Train the v5 profile by extending the frozen v4 semantic grounding with
   the answer-free v5 boundary corpus. The original v4 entry point remains
   byte-reproducible and does not consume this additional corpus. */
int cnet_compete_intent_train_v5(const char *artifact_path,
                                 const char *metadata_path,
                                 const char *v4_semantic_corpus_path,
                                 const char *v5_semantic_corpus_path,
                                 CnetCompeteIntentReport *report);

/* Load only the fixed, manifest-bound CNET-ASI-5 base artifact. Header,
   dimensions, size and checksum are verified before the packed loader sees it. */
int cnet_compete_intent_load(const char *artifact_path,
                             const char *metadata_path,
                             CnetCompeteIntentModel **model_out,
                             CnetCompeteIntentReport *report);
void cnet_compete_intent_free(CnetCompeteIntentModel *model);

/* Returns 0 for an accepted covered intent, 1 for calibrated abstention, and
   -1 for invalid arguments. `intent` is ABSTAIN on every refusal path. */
int cnet_compete_intent_classify(CnetCompeteIntentModel *model,
                                 const char *prompt,
                                 CnetCompeteIntent *intent,
                                 double *confidence);

#endif
