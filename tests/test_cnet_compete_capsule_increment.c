#include "cnet_capsule.h"
#include "cnet_compete_capsules.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void clean_capsule(const char *dir) {
    char path[256];
    snprintf(path, sizeof path, "%s/unit.cnb", dir); remove(path);
    snprintf(path, sizeof path, "%s/manifest.cknow", dir); remove(path);
    rmdir(dir);
}

int main(void) {
    CnetBase source, fresh;
    HybridAi source_cov, fresh_cov;
    CnetCompeteCapsuleBuildReport build;
    CnetCapsuleReport capsule;
    char dir[] = "/tmp/cnet_compete_increment_XXXXXX";
    int fd, rc = 1;
    unsigned x;

#define REQUIRE(condition, reason)                                            \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("CNET_7B_CAPSULE_RED reason=%s\n", reason);               \
            goto done;                                                        \
        }                                                                     \
    } while (0)

    cnb_init(&source); cnb_init(&fresh);
    hybrid_ai_init(&source_cov); hybrid_ai_init(&fresh_cov);
    REQUIRE(cnet_compete_capsule_build(&source, &source_cov,
                                       CNET_COMPETE_UNIT_INCREMENT, &build) == 0,
            "increment_build");
    fd = mkstemp(dir);
    REQUIRE(fd >= 0, "temp_path");
    close(fd); unlink(dir);
    memset(&capsule, 0, sizeof capsule);
    REQUIRE(cnet_capsule_export(&source, &source_cov, "increment_mod256", dir,
                                &capsule) == 0, "capsule_export");
    REQUIRE(strcmp(capsule.scope, "exhaustive") == 0,
            "scope_not_exhaustive");
    REQUIRE(capsule.coverage_rows == 256, "coverage_rows");
    REQUIRE(capsule.payload_bytes > 0, "empty_payload");
    memset(&capsule, 0, sizeof capsule);
    REQUIRE(cnet_capsule_import(&fresh, &fresh_cov, dir, &capsule) == 0,
            "capsule_import");
    for (x = 0; x < 256; ++x) {
        unsigned output = 999;
        REQUIRE(cnet_compete_capsule_eval(&fresh, &fresh_cov,
                                          CNET_COMPETE_UNIT_INCREMENT, x,
                                          &output) == 0,
                "fresh_eval_refused");
        REQUIRE(output == ((x + 1u) & 255u), "fresh_eval_wrong");
    }
    REQUIRE(build.domain_rows == 256, "build_domain_rows");
    REQUIRE(build.certified_rows == 256, "build_certified_rows");
    REQUIRE(build.min_margin >= CNET_COMPETE_CAPSULE_MARGIN_FLOOR,
            "build_margin");
    REQUIRE(build.behavior_digest != 0, "build_digest");
    printf("CNET_7B_CAPSULE_INCREMENT_PASS rows=256 margin=%.6f bytes=%zu\n",
           build.min_margin, capsule.payload_bytes);
    rc = 0;
done:
    clean_capsule(dir);
    hybrid_ai_free(&fresh_cov); hybrid_ai_free(&source_cov);
    cnb_free(&fresh); cnb_free(&source);
#undef REQUIRE
    return rc;
}
