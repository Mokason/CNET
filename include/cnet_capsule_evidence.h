#ifndef CNET_CAPSULE_EVIDENCE_H
#define CNET_CAPSULE_EVIDENCE_H
#include <stddef.h>
#include "contract/contract.h"
#include "hybrid_ai.h"

/* Asset schema for the existing schema-2 frontend.cvfa sidecar. Not a second
 * capsule format. Checksums and literal-line receipts are NOT authentication. */
#define CNET_CAPSULE_EVIDENCE_SCHEMA 0x43534501u
#define CNET_CAPSULE_EVIDENCE_FACTS 5u
#define CNET_CAPSULE_EVIDENCE_TEXT 256u
#define CNET_CAPSULE_EVIDENCE_INPUT "cnet_source_fact"
#define CNET_CAPSULE_EVIDENCE_OUTPUT "cnet_source_answer"
typedef struct CnetCapsuleEvidence CnetCapsuleEvidence;

/* Strict asset parser: rejects unknown schema, malformed/unsupported data, and
 * disagreement with BOTH the sealed contract and its exact labelled coverage.
 * Copies all retained data; caller frees with close. No filesystem effects. */
CnetCapsuleEvidence *cnet_capsule_evidence_parse(const void *asset, size_t length,
    unsigned schema, const Contract *contract, const HybridCoverage *coverage,
    char *error, size_t error_cap);
void cnet_capsule_evidence_close(CnetCapsuleEvidence *evidence);
/* Reopen the absolute owner-configured root and all relative components using
 * nofollow descriptors; require owner-owned source directories/files and
 * fresh full-file SHA256 plus exact literal/receipt replay. No cached root fd.
 * Caller checks every used hop, and rechecks before emitting its final answer.
 * Source bytes are untrusted read-only data; group-writable checkouts are
 * supported. This grants no output/control authority. Owner must prevent
 * concurrent source mutation during a request; this is not
 * a multi-file atomic filesystem snapshot or a post-return freshness promise. */
int cnet_capsule_evidence_fresh(const CnetCapsuleEvidence *evidence,
    const char *owner_root, char *error, size_t error_cap);
/* Only call after numeric certification and freshness checks. No free prose;
 * renders the canonical decoder's label or refuses an out-of-domain value.
 * All string APIs clear output on failure. */
int cnet_capsule_evidence_render(const CnetCapsuleEvidence *evidence,
    unsigned value, char *out, size_t cap);
int cnet_capsule_evidence_identity(const CnetCapsuleEvidence *evidence,
    char out[65]);
/* 0 = compatible, -1 = conflicting decoder under the same complete output
 * interface. Different interfaces are compatible. NULL is not evidence. */
int cnet_capsule_evidence_compatible(const CnetCapsuleEvidence *a,
    const CnetCapsuleEvidence *b);
/* Closed named query allowlist, suitable for an exact front-door adapter.
 * No path, arbitrary source text, or instructions can become a query. */
const char *cnet_capsule_evidence_fact_name(unsigned index);
int cnet_capsule_evidence_request(const char *name, char *out, size_t cap);
/* Acquisition helper: reads sources and executes actual fixed argv grep
 * literal-line checks on private source snapshots. Emits a malloc-owned asset
 * only after the whole batch and final source freshness pass. No publication,
 * network, teacher, shell or source execution; caller frees *asset. */
int cnet_capsule_evidence_acquire(const char *owner_root, void **asset,
    size_t *length, char *error, size_t error_cap);
#endif
