#ifndef CNET_CAPSULE_STORE_H
#define CNET_CAPSULE_STORE_H
#include "cnet_core_host.h"
typedef struct CnetCapsuleStore CnetCapsuleStore;
typedef struct {
    uint64_t revision;
    char active[65],rollback[65],staged[65];
    int durability_uncertain;
} CnetCapsuleStoreStatus;
/* Serial owner API. Existing absolute owner directories only. One exclusive
 * process owns state; no live registry files are overwritten or removed.
 * Recovery verifies selected snapshots, identity history and publication sync.
 * A failed post-rename sync freezes mutations, but retains incumbent serving.
 * Restart reconciles the selection; never assume an uncertain commit rolled back. */
CnetCapsuleStore *cnet_capsule_store_open(const char *sets,const char *state);
int cnet_capsule_store_close(CnetCapsuleStore *store);
int cnet_capsule_store_status(CnetCapsuleStore *store,CnetCapsuleStoreStatus *out);
CnetCoreLease *cnet_capsule_store_pin(CnetCapsuleStore *store);
/* Named owner set only; stage copies and validates before changing anything.
 * preserve=1 replays incumbent obligations; preserve=0 explicitly switches. */
int cnet_capsule_store_stage(CnetCapsuleStore *store,uint64_t expected,const char *name,
                             int preserve,char digest[65]);
int cnet_capsule_store_discard(CnetCapsuleStore *store,const char *digest);
/* token: 1..63 ASCII letters/digits/_/-. Exact retry of the last successful
 * mutation returns its result without repeating it; older revisions refuse.
 * Return 0 durable success/retry, -1 refusal, -2 publication uncertain. */
int cnet_capsule_store_activate(CnetCapsuleStore *store,uint64_t expected,
                               const char *token,const char *digest);
int cnet_capsule_store_unload(CnetCapsuleStore *store,uint64_t expected,const char *token);
int cnet_capsule_store_rollback(CnetCapsuleStore *store,uint64_t expected,const char *token);
#endif
