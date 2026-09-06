/* External tool truth -> learned capsule -> fresh serving core -> typed chain.
 * Reuses only the existing capsule fixture's training/export scaffolding. */
#define main original_capsule_fixture_main
#include "test_knowledge_capsule.c"
#undef main
#include "cnet_capsule_core.h"
#include <dlfcn.h>

int main(void) {
    void *lib = dlopen("bin/libcnet_capsule_core.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { puts("CAPSULE_CORE_GROWTH_RED runtime_unavailable"); return 1; }
    CnetCapsuleCore *(*open_core)(const char *, char *, size_t);
    void (*close_core)(CnetCapsuleCore *);
    int (*ask)(CnetCapsuleCore *, const char *, CnetCapsuleCoreReply *);
    *(void **)(&open_core) = dlsym(lib, "cnet_capsule_core_open");
    *(void **)(&close_core) = dlsym(lib, "cnet_capsule_core_close");
    *(void **)(&ask) = dlsym(lib, "cnet_capsule_core_ask");
    if (!open_core || !close_core || !ask) return 2;
    char root[] = "/tmp/cnet-core-growth-XXXXXX", pack[256], error[160];
    if (!mkdtemp(root)) return 2;
    snprintf(pack, sizeof pack, "%s/offset", root);
    CnetCapsuleCoreReply r;
    CnetCapsuleCore *core = open_core(root, error, sizeof error);
    check(core && ask(core, "capsule cap_in cap_out 2", &r) != 0 && !r.verified,
          "before teaching: actual serving core refuses unknown capability");
    close_core(core);
    CnetBase b; HybridAi h; CnetCapsuleReport rep;
    cnb_init(&b); hybrid_ai_init(&h);
    check(build_unit(&b, &h, "offset") == 0, "external modulo tool teaches native BTN");
    check(cnet_capsule_export(&b, &h, "offset", pack, &rep) == 0,
          "certified unit publishes using existing capsule format");
    for (int restart = 0; restart < 2; restart++) {
        core = open_core(root, error, sizeof error);
        check(core != NULL, "fresh runtime imports and replays certification");
        if (!core) { fprintf(stderr, "%s\n", error); break; }
        for (unsigned x = 0; x < SYM; x++) {
            char q[96]; snprintf(q, sizeof q, "capsule cap_in cap_out %u", x);
            int rc = ask(core, q, &r);
            check(x < COV_ROWS ? (!rc && r.verified && r.value == (x + 3) % SYM)
                              : (rc != 0 && !r.verified),
                  x < COV_ROWS ? "independent tool expected answer matches" : "OOD remains refused");
        }
        const char *bad[] = {"capsule cap_in cap_out -1", "capsule cap_in cap_out 1.5",
            "capsule cap_in cap_out 1 trailing", "capsule cap_in cap_out 8",
            "capsule cap_in wrong 1", "capsule cap_in cap_out 99999999999999999999"};
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
            check(ask(core, bad[i], &r) != 0 && !r.verified, "malformed or incompatible request refuses");
        close_core(core);
    }
    cnb_free(&b); hybrid_ai_free(&h); rm_pack(pack); rmdir(root); dlclose(lib);
    printf("CAPSULE_CORE_GROWTH_%s checks=%d failures=%d\n", failures ? "RED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
