#define main capsule_fixture_main
#include "test_knowledge_capsule.c"
#undef main
#define main quarantine_main
#include "../tools/cnet_quarantine_base.c"
#undef main

int main(void) {
    char root[] = "/tmp/cnet-quarantine-test-XXXXXX", src[256], dst[256], path[300];
    if (!mkdtemp(root)) return 2;
    snprintf(src, sizeof src, "%s/source.cnb", root);
    snprintf(dst, sizeof dst, "%s/deployment", root);
    CnetBase b, loaded; HybridAi h;
    cnb_init(&b); cnb_init(&loaded); hybrid_ai_init(&h);
    check(!build_unit(&b, &h, "keep_unit") && !build_unit(&b, &h, "bad_unit"), "fixture");
    check(!cnb_save(&b, src), "save source");
    char *args[] = {"quarantine", src, "missing_unit", dst};
    check(quarantine_main(4, args) != 0 && access(dst, F_OK), "missing target refuses without publishing");
    args[2] = "bad_unit";
    check(!quarantine_main(4, args), "quarantine publishes separate deployment");
    check(quarantine_main(4, args) != 0, "existing deployment never overwritten");
    snprintf(path, sizeof path, "%s/active.cnb", dst);
    check(!cnb_load(&loaded, path) && loaded.unit_count == 1 && cnb_has_unit(&loaded, "keep_unit"), "only requested unit excluded");
    cnb_free(&loaded); cnb_init(&loaded); unlink(path);
    snprintf(path, sizeof path, "%s/active.cnb.coverage", dst);
    check(!access(path, F_OK), "zero-mined deployment has explicit empty coverage store");
    unlink(path);
    snprintf(path, sizeof path, "%s/quarantined.cnb", dst);
    check(!cnb_load(&loaded, path) && loaded.unit_count == 1 && cnb_has_unit(&loaded, "bad_unit"), "quarantine recoverable");
    cnb_free(&loaded); cnb_init(&loaded); unlink(path);
    check(!cnb_load(&loaded, src) && loaded.unit_count == 2, "source untouched");
    cnb_free(&loaded); cnb_free(&b); hybrid_ai_free(&h);
    unlink(src); rmdir(dst); rmdir(root);
    printf("BASE_QUARANTINE_%s checks=%d failures=%d\n", failures ? "RED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
