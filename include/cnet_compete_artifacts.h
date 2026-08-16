#ifndef CNET_COMPETE_ARTIFACTS_H
#define CNET_COMPETE_ARTIFACTS_H

#include <stddef.h>
#include <string.h>

#define CNET_COMPETE_ARTIFACT_ROOT "artifacts/cnet_asi5_v5"
#define CNET_COMPETE_ARTIFACT_FILE_COUNT 15u
#define CNET_COMPETE_CAPSULE_PAYLOAD_BYTES 192352u
#define CNET_COMPETE_CAPSULE_ARTIFACT_BYTES 255109u
/* Base-artifact identity of the suite being built. The defaults below are v5's.
   They are overridable so that more than one suite's runtime gate can be built
   from this one tree: while these were unconditional, every freeze rebased them
   onto the newest suite (91581 v3 -> 272605 v4 -> 321757 v5) and each older
   suite's runtime gate became permanently unpassable. Override with -D, as
   CNET_V4_BASE_DEFINE in the Makefile does. BASE_ARTIFACT_BYTES is intent.wlm
   plus intent.meta, not intent.wlm alone. */
#ifndef CNET_COMPETE_BASE_PARAMETERS
#define CNET_COMPETE_BASE_PARAMETERS 321757u
#endif
#ifndef CNET_COMPETE_BASE_ARTIFACT_BYTES
#define CNET_COMPETE_BASE_ARTIFACT_BYTES 277687u
#endif
#define CNET_COMPETE_ARTIFACT_MANIFEST_SHA256 \
    "81fe446218431d7520a7a2d4309e069600ae11be0d3d73e92e04dea78cb7c009"
#define CNET_COMPETE_ARTIFACT_MODEL \
    CNET_COMPETE_ARTIFACT_ROOT "/intent.wlm"
#define CNET_COMPETE_ARTIFACT_META \
    CNET_COMPETE_ARTIFACT_ROOT "/intent.meta"
#define CNET_COMPETE_ARTIFACT_CAPSULE_ROOT \
    CNET_COMPETE_ARTIFACT_ROOT "/capsules"
#define CNET_COMPETE_ARTIFACT_MANIFEST \
    CNET_COMPETE_ARTIFACT_ROOT "/artifacts.sha256"

#ifndef CNET_COMPETE_RELEASE_ARTIFACT_ROOT
#define CNET_COMPETE_RELEASE_ARTIFACT_ROOT CNET_COMPETE_ARTIFACT_ROOT
#endif
#ifndef CNET_COMPETE_MANIFEST_DECLARED_ROOT
#define CNET_COMPETE_MANIFEST_DECLARED_ROOT CNET_COMPETE_ARTIFACT_ROOT
#endif
#define CNET_COMPETE_RELEASE_ARTIFACT_MODEL \
    CNET_COMPETE_RELEASE_ARTIFACT_ROOT "/intent.wlm"
#define CNET_COMPETE_RELEASE_ARTIFACT_META \
    CNET_COMPETE_RELEASE_ARTIFACT_ROOT "/intent.meta"
#define CNET_COMPETE_RELEASE_ARTIFACT_CAPSULE_ROOT \
    CNET_COMPETE_RELEASE_ARTIFACT_ROOT "/capsules"
#define CNET_COMPETE_RELEASE_ARTIFACT_MANIFEST \
    CNET_COMPETE_RELEASE_ARTIFACT_ROOT "/artifacts.sha256"

static inline int cnet_compete_artifact_paths_are_canonical(
    const char *model, const char *meta, const char *capsule_root,
    const char *manifest) {
    return model != NULL && meta != NULL && capsule_root != NULL &&
           manifest != NULL &&
           strcmp(model, CNET_COMPETE_RELEASE_ARTIFACT_MODEL) == 0 &&
           strcmp(meta, CNET_COMPETE_RELEASE_ARTIFACT_META) == 0 &&
           strcmp(capsule_root,
                  CNET_COMPETE_RELEASE_ARTIFACT_CAPSULE_ROOT) == 0 &&
           strcmp(manifest, CNET_COMPETE_RELEASE_ARTIFACT_MANIFEST) == 0;
}

static inline const char *cnet_compete_artifact_member(size_t index) {
    static const char *const members[CNET_COMPETE_ARTIFACT_FILE_COUNT] = {
        "intent.wlm",
        "intent.meta",
        "capsules/.complete",
        "capsules/access_policy_v1/manifest.cknow",
        "capsules/access_policy_v1/unit.cnb",
        "capsules/add3_mod256/manifest.cknow",
        "capsules/add3_mod256/unit.cnb",
        "capsules/crc8_atm/manifest.cknow",
        "capsules/crc8_atm/unit.cnb",
        "capsules/double_mod256/manifest.cknow",
        "capsules/double_mod256/unit.cnb",
        "capsules/increment_mod256/manifest.cknow",
        "capsules/increment_mod256/unit.cnb",
        "capsules/minutes_to_seconds/manifest.cknow",
        "capsules/minutes_to_seconds/unit.cnb"
    };
    return index < CNET_COMPETE_ARTIFACT_FILE_COUNT ? members[index] : NULL;
}

#endif
