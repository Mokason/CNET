#ifndef CNET_CAPSULE_SNAPSHOT_H
#define CNET_CAPSULE_SNAPSHOT_H
#include <stddef.h>
/* Linux owner-only snapshot boundary, not another capsule encoding. Each set
 * contains <=4096 capsule directories and <=32GiB of original capsule files.
 * FDs must pin owner directories. No symlinks/hardlinks/unknown artifacts.
 * name selects one safe child of source, or NULL selects source itself. */
int cnet_capsule_snapshot_create(int source,int destination,const char *name,
                                char digest[65],size_t *bytes);
int cnet_capsule_snapshot_verify(int directory,const char *digest,size_t *bytes);
int cnet_capsule_owner_directory(const char *path,int private_mode);
/* Read-only complete inventory boundary, including the exact publisher lock.
 * Call before parsing owner-stable source content through other interfaces. */
int cnet_capsule_snapshot_inspect(int source);
/* Copy 1..8 distinct safe child directories; validate ALL source artifacts,
 * including unselected capsules. Callback must return zero after validating the
 * staged inventory through its borrowed FD. It must not mutate/close that FD.
 * Validation precedes publication and also runs on identical retries. */
int cnet_capsule_snapshot_selected(int source,int destination,
    const char *const *names,size_t count,int (*validate)(int,void *),void *context,
    char digest[65],size_t *bytes);
/* Collect only private, unselected cache copies (including interrupted copy
 * attempts). Input registries and the identity ledger are never deleted. */
int cnet_capsule_snapshot_collect(int directory,const char *active,const char *rollback,const char *staged);
#endif
