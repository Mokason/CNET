/* RED marker: CAPSULE_RESIDENT_RED; reuse independent modulo-tool fixture. */
#define main original_capsule_fixture_main
#include "test_knowledge_capsule.c"
#undef main
#include "cnet_core_host.h"
#include <dlfcn.h>

int main(void) {
    const char *library=getenv("CNET_TEST_CORE_LIBRARY");
    void *lib = dlopen(library?library:"bin/libcnet_capsule_core.so", RTLD_NOW | RTLD_LOCAL);
    int (*stage_registry)(CnetCoreHost *, const char *, int, uint64_t *);
    uint64_t (*revision)(CnetCoreHost *);
    if (!lib) { puts("CAPSULE_RESIDENT_RED missing_runtime"); return 1; }
    *(void **)(&stage_registry) = dlsym(lib, "cnet_core_host_stage_registry");
    *(void **)(&revision) = dlsym(lib, "cnet_core_host_revision");
    if (!stage_registry || !revision) {
        puts("CAPSULE_RESIDENT_RED missing_registry_lifecycle"); return 1;
    }
    char root[] = "/tmp/cnet-resident-XXXXXX", a[256], b[256], bad[256], pack[300], changed[300];
    if (!mkdtemp(root)) return 2;
    snprintf(a,sizeof a,"%s/a",root); snprintf(b,sizeof b,"%s/empty",root);
    snprintf(bad,sizeof bad,"%s/changed",root);
    if (mkdir(a,0700)||mkdir(b,0700)||mkdir(bad,0700)) return 2;
    snprintf(pack,sizeof pack,"%s/offset",a); snprintf(changed,sizeof changed,"%s/offset",bad);
    CnetBase base; HybridAi h; CnetCapsuleReport rep;
    cnb_init(&base); hybrid_ai_init(&h);
    check(!build_unit(&base,&h,"offset"),"independent modulo tool teaches capsule");
    check(!cnet_capsule_export(&base,&h,"offset",pack,&rep),"export original capsule");
    /* Same sealed BTN, fewer coverage rows: numerically plausible, different identity. */
    check(h.coverage_count==1 && h.coverage[0].n_rows==COV_ROWS,"fixture has sampled coverage");
    h.coverage[0].n_rows--;
    check(!cnet_capsule_export(&base,&h,"offset",changed,&rep),"export changed coverage under same name");
    CnetCoreHost *host=cnet_core_host_open(a);
    check(host!=NULL,"resident initial self-closure load");
    if (!host) return 1;
    CnetCoreLease *old=cnet_core_host_pin(host); CnetCapsuleCoreReply r;
    uint64_t id=0, rev=revision(host);
    check(old && !cnet_core_host_ask(old,"capsule cap_in cap_out 2",&r) && r.value==5,"resident serves original");
    check(!stage_registry(host,a,1,&id) && !cnet_core_host_activate(host,id),"identical set passes identity and upgrade gates");
    rev=revision(host);
    check(stage_registry(host,b,1,&id)!=0,"upgrade cannot lose incumbent coverage");
    check(revision(host)==rev,"failed upgrade preserves activation revision");
    check(!stage_registry(host,b,0,&id) && !cnet_core_host_activate(host,id),"explicit set switch can unload capability");
    check(revision(host)>rev,"successful switch advances revision"); rev=revision(host);
    CnetCoreLease *empty=cnet_core_host_pin(host);
    check(empty && cnet_core_host_ask(empty,"capsule cap_in cap_out 2",&r)!=0 && !r.verified,"new generation abstains when unloaded");
    check(!cnet_core_host_ask(old,"capsule cap_in cap_out 2",&r) && r.value==5,"old request retains original inventory");
    check(stage_registry(host,bad,0,&id)!=0,"switch-away cannot conceal conflicting same-name coverage");
    check(!cnet_core_host_rollback(host) && revision(host)>rev,"rollback advances revision, not ABA");
    rev=revision(host);
    check(stage_registry(host,"/definitely/absent/cnet-capsules",0,&id)!=0,"missing candidate refuses");
    check(revision(host)==rev,"failed load retains selected state");
    /* Direct +3 conflicts with +3 then +3. All three individual contracts are
       valid: only candidate self-closure detects the composed contradiction. */
    char conflict[256], piece[300];
    snprintf(conflict,sizeof conflict,"%s/contradiction",root);
    check(!mkdir(conflict,0700),"create composed conflict fixture");
    const char *units[]={"direct","first","second"};
    const char *inputs[]={"cap_in","cap_in","middle"};
    const char *outputs[]={"cap_out","middle","cap_out"};
    for(unsigned j=0;j<3;j++) {
        CnetBase cb;HybridAi ch;cnb_init(&cb);hybrid_ai_init(&ch);
        snprintf(piece,sizeof piece,"%s/%s",conflict,units[j]);
        check(!build_unit_ports(&cb,&ch,units[j],P(inputs[j]),P(outputs[j])) &&
              !cnet_capsule_export(&cb,&ch,units[j],piece,&rep),"independently valid unit exports");
        cnb_free(&cb);hybrid_ai_free(&ch);
    }
    CnetCoreHost *contradictory=cnet_core_host_open(conflict);
    check(!contradictory,"initial inventory rejects contradictory two-hop route");
    if(contradictory)cnet_core_host_close(contradictory);
    check(stage_registry(host,conflict,0,&id)!=0 && revision(host)==rev,
          "set switch rejects self-closure failure without changing revision");
    CnetCoreLease *pins[64]={0};unsigned held=0;
    while(held<64 && (pins[held]=cnet_core_host_pin(host)))held++;
    check(held==62,"lease limit accounts for two existing requests");
    for(unsigned j=0;j<held;j++)cnet_core_host_unpin(pins[j]);
    check(cnet_core_host_close(host)!=0,"cannot destroy pinned runtime");
    cnet_core_host_unpin(old); cnet_core_host_unpin(empty);
    check(!stage_registry(host,NULL,0,&id) && !cnet_core_host_activate(host,id),"explicit unload creates terminal empty inventory");
    check(!cnet_core_host_close(host),"unpinned runtime destroys cleanly");
    cnb_free(&base); hybrid_ai_free(&h); dlclose(lib);
    printf("CAPSULE_RESIDENT_%s checks=%d failures=%d artifacts=%s\n", failures?"RED":"PASS",checks,failures,root);
    return failures?1:0;
}
