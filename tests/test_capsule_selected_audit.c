/* Internal search must audit each selected unit, including replay callers. */
#define main original_capsule_fixture_main
#include "test_knowledge_capsule.c"
#undef main
#include "../src/serve/cnet_capsule_core.c"
int main(void) {
    char root[]="/tmp/cnet-selected-audit-XXXXXX", pack[256], error[160];
    if (!mkdtemp(root)) return 2;
    snprintf(pack,sizeof pack,"%s/pack",root);
    CnetBase b; HybridAi h; CnetCapsuleReport report;
    cnb_init(&b); hybrid_ai_init(&h);
    if (build_unit(&b,&h,"audit_fixture") || cnet_capsule_export(&b,&h,"audit_fixture",pack,&report)) return 2;
    CnetCapsuleCore *core=cnet_capsule_core_open(root,error,sizeof error);
    if (!core) return 2;
    double input[SYM]; oh(input,0);
    CoreBudget budget; budget_init(&budget,2000000);
    CnetCapsuleCoreReply reply={0};
    /* A numerically small change may leave the answer intact, but invalidates
     * the sealed behavior identity nonetheless. Do not trust stale flags. */
    core->registry.entries[0].btn->output_bias[0] += 0.001;
    int rc=search(core,P("cap_in"),P("cap_out"),input,&reply,&budget,1);
    int failed=!rc || reply.verified || core->registry.entries[0].certified;
    size_t obligations=0;
    puts("CAPSULE_DEMOTED_REPLAY_CHECK"); fflush(stdout);
    if (!cnet_capsule_core_validate_growth(core,core,&obligations)) failed=1;
    cnet_capsule_core_close(core);
    core=cnet_capsule_core_open(root,error,sizeof error);
    if (!core) return 2;
    core->registry.entries[0].btn->output_bias[0] += 0.001;
    /* Here input and expected labels are owned by the very entry that the
     * canonical auditor demotes and frees. Both the first and repeated call
     * must refuse without accessing those freed labels. */
    if (!cnet_capsule_core_validate_growth(core,core,&obligations) ||
        !cnet_capsule_core_validate_growth(core,core,&obligations)) failed=1;
    printf("CAPSULE_SELECTED_AUDIT_%s refused=%d certified=%d\n",failed?"RED":"PASS",rc!=0,core->registry.entries[0].certified);
    cnet_capsule_core_close(core); cnb_free(&b); hybrid_ai_free(&h);
    rm_pack(pack); rmdir(root);
    return failed;
}
