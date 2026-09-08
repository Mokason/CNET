#ifndef CNET_CAPSULE_REUSE_INTERNAL_H
#define CNET_CAPSULE_REUSE_INTERNAL_H
#include "cnet_capsule_core.h"
/* Borrowed original directory basenames, valid until core_close. Only for a
 * successful receipt from this core; never an externally supplied plan. */
int cnet_capsule_core_selected_directories(const CnetCapsuleCore *core,
    const CnetCapsuleCoreReply *reply,const char *names[8],size_t *count);
#endif
