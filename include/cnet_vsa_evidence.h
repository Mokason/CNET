#ifndef CNET_VSA_EVIDENCE_H
#define CNET_VSA_EVIDENCE_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

/* In-memory bounded operator; no capsule format or certification claim.
 * Checks consequences of supplied facts, not their external factual truth.
 * Caller owns immutable input storage and a separate writable output buffer. */
#define CNET_VSA_EVIDENCE_ENTITIES 128
#define CNET_VSA_EVIDENCE_RELATIONS 8
#define CNET_VSA_EVIDENCE_HOPS 8
#define CNET_VSA_EVIDENCE_FACTS 512
#define CNET_VSA_EVIDENCE_OUTPUT 512

typedef struct {
    int subject, relation, object, sign; /* sign must be +1 or -1 */
} CnetVsaEvidenceFact;

enum {
    CNET_VSA_EVIDENCE_OK = 0,
    CNET_VSA_EVIDENCE_REFUSE = 1,
    CNET_VSA_EVIDENCE_INVALID = -1,
    CNET_VSA_EVIDENCE_NOSPACE = -2
};

/* Follow relations from start. Refuse missing/multiple endpoints or a positive
 * reachable edge also explicitly negated. Intermediate branches may reconverge.
 * proposed == -1 computes the answer; otherwise it must equal the unique answer.
 * Success writes answer + one complete evidence path. Refusal writes ABSTAIN.
 * Invalid/short output writes no partial answer (out[0]=0 when possible).
 * Uses bounded stack storage and no heap. No global mutable state. */
int cnet_vsa_evidence_response(const CnetVsaEvidenceFact *facts, size_t count,
                              int start, const int *relations, size_t hops,
                              int proposed, char *out, size_t out_size);
#ifdef __cplusplus
}
#endif
#endif
