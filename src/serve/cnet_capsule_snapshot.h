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
/* Collect only private, unselected cache copies (including interrupted copy
 * attempts). Input registries and the identity ledger are never deleted. */
int cnet_capsule_snapshot_collect(int directory,const char *active,const char *rollback,const char *staged);
#endif
