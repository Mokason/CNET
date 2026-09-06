/* Canonical export/import capacity gate. The two seed capsules are certified
 * external-tool XOR tables. Renaming interfaces creates isolated synthetic
 * chains, NOT new knowledge and NOT a sequential acquisition benchmark. */
#include "cnet_capsule_core.h"
#include "cnet_capsule.h"
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static int emit(const char *seed, const char *root, unsigned i) {
    CnetBase source, out; HybridAi cov, outcov; CnetCapsuleReport report;
    BinaryTransformNetwork btn = {0}; Contract ct = {0};
    char path[1200], name[64]; int rc = -1;
    cnb_init(&source); cnb_init(&out); hybrid_ai_init(&cov); hybrid_ai_init(&outcov);
    snprintf(path, sizeof path, "%s/edge_%u", seed, i % 2);
    if (cnet_capsule_import(&source, &cov, path, &report)) goto done;
    if (cnb_get_unit(&source, report.unit, &btn, &ct)) goto done;
    snprintf(name, sizeof name, "edge_%u", i);
    snprintf(btn.input_ports[0].tag, PORT_TAG_MAX, "%s%03uq%03u", i%2 ? "beta" : "alpha", i/2, i/2);
    snprintf(btn.output_ports[0].tag, PORT_TAG_MAX, "%s%03uq%03u", i%2 ? "gamma" : "beta", i/2, i/2);
    snprintf(ct.name, sizeof ct.name, "%s", name);
    ct.input_ports[0] = btn.input_ports[0]; ct.output_ports[0] = btn.output_ports[0];
    if (cnb_add_unit(&out, &btn, &ct, NULL) ||
        hybrid_coverage_record(&outcov, btn.input_ports[0], btn.output_ports[0], name,
            ct.inputs, ct.outputs, ct.exemplar_count, btn.input_count, btn.output_count)) goto done;
    snprintf(path, sizeof path, "%s/edge_%u", root, i);
    if (cnet_capsule_export(&out, &outcov, name, path, &report)) goto done;
    rc = 0;
done:
    if (rc) fprintf(stderr, "fixture export %u: %s\n", i, report.reject_reason);
    contract_free(&ct); btn_free(&btn);
    hybrid_ai_free(&cov); hybrid_ai_free(&outcov); cnb_free(&source); cnb_free(&out);
    return rc;
}
int main(int argc, char **argv) {
    if (argc != 4) return 2;
    unsigned n = (unsigned)strtoul(argv[3], NULL, 10);
    if (n < 2 || n > 4096 || n % 2) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (unsigned i=0; i<n; i++) if (emit(argv[1], argv[2], i)) return 2;
    contract_cache_reset(); malloc_trim(0);
    struct mallinfo2 baseline = mallinfo2();
    double start = now(); char error[160];
    CnetCapsuleCore *core = cnet_capsule_core_open(argv[2], error, sizeof error);
    if (!core) { printf("CAPSULE_LARGE_INVENTORY_RED count=%u reason=%s\n", n, error); return 1; }
    double load_seconds = now()-start;
    struct mallinfo2 loaded = mallinfo2();
    printf("CAPSULE_LARGE_LOADED count=%u seconds=%.6f heap_delta=%zu\n", n, load_seconds,
        loaded.uordblks+loaded.hblkhd-baseline.uordblks-baseline.hblkhd);
    size_t checked=0, correct=0, refused=0; start=now();
    if (cnet_capsule_core_validate_growth(core, core, &checked) || checked!=(size_t)n*96) {
        printf("CAPSULE_LARGE_INVENTORY_RED replay_obligations=%zu seconds=%.6f\n", checked, now()-start); return 1;
    }
    printf("CAPSULE_LARGE_REPLAY obligations=%zu seconds=%.6f\n", checked, now()-start);
    start=now();
    for (unsigned g=0; g<n/2; g++) for (unsigned pair=0; pair<3; pair++) for (unsigned x=0; x<=32; x++) {
        char ask[160]; CnetCapsuleCoreReply reply;
        snprintf(ask, sizeof ask, "capsule %s%03uq%03u %s%03uq%03u %u",
            pair==1 ? "beta" : "alpha", g,g, pair==0 ? "beta" : "gamma",g,g,x);
        int rc=cnet_capsule_core_ask(core,ask,&reply);
        if (x==32) { if (!rc || reply.verified) return 1; refused++; }
        else {
            if (rc || !reply.verified || reply.value!=(x ^ (pair==0 ? 15u : pair==1 ? 7u : 8u)) ||
                reply.hops!=(pair==2 ? 2u : 1u)) return 1;
            correct++;
        }
    }
    printf("CAPSULE_LARGE_ASKS correct=%zu refused=%zu seconds=%.6f\n", correct,refused,now()-start);
    if (n==4096) {
        if (emit(argv[1],argv[2],n)) return 2;
        CnetCapsuleCore *excess=cnet_capsule_core_open(argv[2],error,sizeof error);
        if (excess || strcmp(error,"capsule_count_limit")) return 1;
        CnetCapsuleCoreReply reply;
        if (cnet_capsule_core_ask(core,"capsule alpha000q000 gamma000q000 3",&reply) || reply.value!=11) return 1;
        printf("CAPSULE_LARGE_EXCESS_REFUSED old_snapshot_retained=1\n");
    }
    cnet_capsule_core_close(core);
    printf("CAPSULE_LARGE_INVENTORY_PASS count=%u correct=%zu refused=%zu obligations=%zu\n",n,correct,refused,checked);
    return 0;
}
