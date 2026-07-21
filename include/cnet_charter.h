/* G4 actuator charter — human allowlist for teachable goal tags. */
#ifndef CNET_CHARTER_H
#define CNET_CHARTER_H

#include "cnet_export.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 1 if CNET_CHARTER_ENFORCE=1 */
CNET_API int cnet_charter_enforce(void);

/* Path: CNET_CHARTER_PATH or config/actuator_charter.txt */
CNET_API const char *cnet_charter_path(void);

/* 1 if goal_tag is allowed (prefix or exact). Empty tag → 0 when enforce. */
CNET_API int cnet_charter_allows(const char *goal_tag);

#ifdef __cplusplus
}
#endif
#endif
