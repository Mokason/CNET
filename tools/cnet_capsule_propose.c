/* cnet_capsule_propose — propose a capsule export directory (NOT seal).
 *
 * Usage:
 *   cnet_capsule_propose --unit NAME [--base path.cnb] [--out dir]
 *
 * Writes pending under var/capsule_inbox/<unit>-<ts>/ with PROPOSE.json
 * and calls cnet_capsule_export when base+unit available.
 * Always auto_cert=false / pending verify.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../include/base.h"
#include "../include/cnet_capsule.h"
#include "../include/cnet_platform.h"

static void usage(void) {
    fprintf(stderr,
            "usage: cnet_capsule_propose --unit NAME [--base path.cnb] [--out DIR]\n"
            "Proposes capsule only — never self-CERTs.\n");
}

int main(int argc, char **argv) {
    const char *unit = NULL;
    const char *base_path = NULL;
    const char *out_root = NULL;
    char outdir[512];
    char prop[640];
    FILE *f;
    time_t now = time(NULL);
    int i, have_export = 0;
    CnetBase base;
    HybridAi cov;
    CnetCapsuleReport rep;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--unit") && i + 1 < argc) unit = argv[++i];
        else if (!strcmp(argv[i], "--base") && i + 1 < argc) base_path = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_root = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        }
    }
    if (!unit || !unit[0]) {
        usage();
        return 2;
    }
    if (!base_path || !base_path[0])
        base_path = getenv("CNET_BASE_PATH");
    if (!base_path || !base_path[0])
        base_path = "/home/marble/AI/CNET/soul_gemma4v2_final.cnb";
    if (!out_root || !out_root[0]) {
        const char *min = getenv("CNET_MINIMAL_ROOT");
        if (min && min[0]) {
            static char def[512];
            snprintf(def, sizeof def, "%s/var/capsule_inbox", min);
            out_root = def;
        } else {
            out_root = "var/capsule_inbox";
        }
    }
    cnet_mkdir(out_root, 0755);
    snprintf(outdir, sizeof outdir, "%s/%s-%ld", out_root, unit, (long)now);
    cnet_mkdir(outdir, 0755);

    memset(&rep, 0, sizeof rep);
    cnb_init(&base);
    hybrid_ai_init(&cov);
    if (cnb_load(&base, base_path) == 0) {
        if (cnet_capsule_export(&base, &cov, unit, outdir, &rep) == 0) {
            have_export = 1;
        }
    }

    snprintf(prop, sizeof prop, "%s/PROPOSE.json", outdir);
    f = fopen(prop, "w");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", prop);
        cnb_free(&base);
        return 1;
    }
    fprintf(f,
            "{\n"
            "  \"kind\": \"capsule_propose\",\n"
            "  \"unit\": \"%s\",\n"
            "  \"base\": \"%s\",\n"
            "  \"out\": \"%s\",\n"
            "  \"export_ok\": %s,\n"
            "  \"auto_cert\": false,\n"
            "  \"status\": \"pending_verify\",\n"
            "  \"ts\": %ld,\n"
            "  \"law\": \"propose_only_never_self_cert\"\n"
            "}\n",
            unit, base_path, outdir, have_export ? "true" : "false", (long)now);
    fclose(f);

    printf("CAPSULE_PROPOSE unit=%s out=%s export_ok=%d auto_cert=0\n", unit, outdir,
           have_export);
    if (have_export)
        printf("CAPSULE_PROPOSE_PASS\n");
    else {
        printf("CAPSULE_PROPOSE_PENDING_NO_EXPORT (unit missing or base load fail — "
               "PROPOSE.json still written)\n");
        /* still pass propose path */
        printf("CAPSULE_PROPOSE_PASS\n");
    }
    cnb_free(&base);
    return 0;
}
