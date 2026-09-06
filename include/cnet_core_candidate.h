#ifndef CNET_CORE_CANDIDATE_H
#define CNET_CORE_CANDIDATE_H
#include "cnet_core_cell.h"
#define CNET_CORE_CANDIDATE_BYTES 404
typedef struct {
    CnetCoreCell cell;
    char training_sha256[65],evaluation_sha256[65];
} CnetCoreCandidate;
/* Version-1 canonical little-endian FP32 core candidate, NOT a capsule.
 * Fixed shape/model/feature/dtype; status is always unapproved. Evidence hashes
 * and content SHA256 bind bytes, not authenticity or a passed evaluation.
 * Trusted owner-private directory FD, single filename component, owner-private
 * regular file. Save is exclusive and fsynced; partial/crashed writes refuse.
 * Failure clears load output. No activation is performed by these functions. */
int cnet_core_candidate_save_at(int dirfd,const char *name,const CnetCoreCandidate *candidate);
int cnet_core_candidate_load_at(int dirfd,const char *name,CnetCoreCandidate *candidate);
#endif
