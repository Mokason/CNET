#ifndef CNET_CAPSULE_CONTROL_H
#define CNET_CAPSULE_CONTROL_H
#include "cnet_capsule_store.h"
typedef struct CnetCapsuleControl CnetCapsuleControl;
/* Owner-only Unix socket, separate from ASK. Parent must already be0700.
 * No external path strings or commands are accepted through chat. */
CnetCapsuleControl *cnet_capsule_control_open(CnetCapsuleStore *store,const char *path);
int cnet_capsule_control_fd(const CnetCapsuleControl *control);
void cnet_capsule_control_accept(CnetCapsuleControl *control);
void cnet_capsule_control_close(CnetCapsuleControl *control);
#endif
