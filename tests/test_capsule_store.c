#define main original_capsule_fixture_main
#include "test_knowledge_capsule.c"
#undef main
#include "cnet_capsule_store.h"
#include <dlfcn.h>

int main(void) {
    const char *library=getenv("CNET_TEST_CORE_LIBRARY");
    void *lib=dlopen(library?library:"bin/libcnet_capsule_core.so",RTLD_NOW|RTLD_LOCAL);
    CnetCapsuleStore *(*open_store)(const char *,const char *);
    int (*close_store)(CnetCapsuleStore *);
    int (*status)(CnetCapsuleStore *,CnetCapsuleStoreStatus *);
    CnetCoreLease *(*pin)(CnetCapsuleStore *);
    int (*stage)(CnetCapsuleStore *,uint64_t,const char *,int,char[65]);
    int (*activate)(CnetCapsuleStore *,uint64_t,const char *,const char *);
    int (*unload)(CnetCapsuleStore *,uint64_t,const char *);
    int (*rollback)(CnetCapsuleStore *,uint64_t,const char *);
#define SYMBOL(variable,symbol) do { *(void **)(&variable)=dlsym(lib,symbol); \
    if(!variable){puts("CAPSULE_STORE_RED missing_durable_lifecycle");return 1;} }while(0)
    if(!lib)return 2;
    SYMBOL(open_store,"cnet_capsule_store_open");SYMBOL(close_store,"cnet_capsule_store_close");
    SYMBOL(status,"cnet_capsule_store_status");SYMBOL(pin,"cnet_capsule_store_pin");
    SYMBOL(stage,"cnet_capsule_store_stage");SYMBOL(activate,"cnet_capsule_store_activate");
    SYMBOL(unload,"cnet_capsule_store_unload");SYMBOL(rollback,"cnet_capsule_store_rollback");
    char root[]="/tmp/cnet-store-XXXXXX",sets[256],state[256],a[300],piece[350],digest[65];
    if(!mkdtemp(root))return 2;
    snprintf(sets,sizeof sets,"%s/sets",root);snprintf(state,sizeof state,"%s/state",root);
    snprintf(a,sizeof a,"%s/original",sets);snprintf(piece,sizeof piece,"%s/offset",a);
    if(mkdir(sets,0700)||mkdir(state,0700)||mkdir(a,0700))return 2;
    CnetBase b;HybridAi h;CnetCapsuleReport rep;cnb_init(&b);hybrid_ai_init(&h);
    check(!build_unit(&b,&h,"offset")&&!cnet_capsule_export(&b,&h,"offset",piece,&rep),"create owner-approved set");
    check(!chmod(piece,0700),"owner seals capsule directory permissions");
    CnetCapsuleStore *store=open_store(sets,state);check(store!=NULL,"initialize durable empty selection");
    if(!store)return 1;
    CnetCapsuleStoreStatus st;check(!status(store,&st)&&st.revision==1&&!st.durability_uncertain,"initial durable revision");
    CnetCapsuleStore *duplicate=open_store(sets,state);check(!duplicate,"second process owner refuses");if(duplicate)close_store(duplicate);
    check(stage(store,1,"../escape",0,digest)!=0,"named staging rejects path escape");
    check(!stage(store,1,"original",1,digest),"prepare immutable upgrade without activation");
    check(activate(store,9,"first",digest)!=0,"stale activation refuses");
    check(!activate(store,1,"first",digest),"activate durable prepared inventory");
    check(!activate(store,1,"first",digest),"lost-response retry is idempotent");
    check(activate(store,1,"different",digest)!=0,"same revision different operation refuses");
    CnetCoreLease *old=pin(store);CnetCapsuleCoreReply reply;
    check(old&&!cnet_core_host_ask(old,"capsule cap_in cap_out 2",&reply)&&reply.value==5,"selected snapshot answers");
    check(!unload(store,2,"empty"),"unload is durable explicit selection");
    check(!cnet_core_host_ask(old,"capsule cap_in cap_out 2",&reply)&&reply.value==5,"pinned request survives durable unload");
    check(close_store(store)!=0,"store teardown refuses while pinned");cnet_core_host_unpin(old);
    check(!close_store(store),"close durable store");store=open_store(sets,state);
    check(store&&!status(store,&st)&&st.revision==3,"restart restores unloaded revision");if(!store)return 1;
    CnetCoreLease *empty=pin(store);
    check(empty&&cnet_core_host_ask(empty,"capsule cap_in cap_out 2",&reply)!=0&&!reply.verified,"restart does not resurrect unloaded set");
    cnet_core_host_unpin(empty);
    check(!rollback(store,3,"restore"),"rollback survives restart and advances revision");
    check(!rollback(store,3,"restore"),"rollback retry cannot toggle again");
    old=pin(store);check(old&&!cnet_core_host_ask(old,"capsule cap_in cap_out 2",&reply)&&reply.value==5,"rollback restores certified answer");
    cnet_core_host_unpin(old);
    check(!status(store,&st)&&st.revision==4,"rollback revision is monotonic");
    char bad[300],changed[350];
    snprintf(bad,sizeof bad,"%s/changed",sets);snprintf(changed,sizeof changed,"%s/offset",bad);
    check(!mkdir(bad,0700)&&!mkdir(changed,0700),"create changed same-name candidate");
    h.coverage[0].n_rows--;
    check(!cnet_capsule_export(&b,&h,"offset",changed,&rep),"export conflicting coverage identity");
    check(stage(store,4,"changed",0,digest)!=0,"durable history rejects changed identity after restart and unload");
    check(!stage(store,4,"original",1,digest),"repeated preparation reuses original immutable snapshot");
    char selected[500];snprintf(selected,sizeof selected,"%s/snapshots/%s/offset/unit.cnb",state,digest);
    check(!chmod(selected,0600),"inject private staged artifact corruption");
    FILE *corrupt=fopen(selected,"r+b");if(corrupt){fputc(0,corrupt);fclose(corrupt);}
    check(corrupt&&activate(store,4,"corrupt",digest)!=0,"commit rehash refuses mutation after preparation");
    old=pin(store);check(old&&!cnet_core_host_ask(old,"capsule cap_in cap_out 2",&reply)&&reply.value==5,
                       "artifact corruption cannot rewrite an already pinned inventory");cnet_core_host_unpin(old);
    check(!close_store(store),"final close");cnb_free(&b);hybrid_ai_free(&h);dlclose(lib);
    /* Persisted selected snapshot corruption is loud on recovery. */
    store=cnet_capsule_store_open(sets,state);check(!store,"corrupt selected snapshot refuses recovery");
    if(store)cnet_capsule_store_close(store);
    printf("CAPSULE_STORE_%s checks=%d failures=%d artifacts=%s\n",failures?"RED":"PASS",checks,failures,root);
    return failures?1:0;
}
