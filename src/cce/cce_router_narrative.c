#include "cce/cce_router.h"
#include "contract/narrative_coherence.h"

/* Full router integration for Narrative Specialist preference */

bool cce_router_should_use_narrative_specialist(const char* task_description) {
    if (!task_description) return false;
    return cnet_narrative_should_prefer_specialist(task_description);
}