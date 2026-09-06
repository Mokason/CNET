#define main original_capsule_fixture_main
#include "test_knowledge_capsule.c"
#undef main
#include "cnet_capsule_store.h"
#include <sys/wait.h>
#include <dirent.h>

static int pending_records(const char *path) {
    DIR *d=opendir(path);if(!d)return -1;int count=0;struct dirent *e;
    while((e=readdir(d)))if(!strncmp(e->d_name,"selection-pending-",18))count++;
    closedir(d);return count;
}

static CnetCapsuleStore *start(const char *sets,const char *state) {
    if(mkdir(state,0700))return NULL;
    CnetCapsuleStore *s=cnet_capsule_store_open(sets,state);char hash[65];
    if(!s)return NULL;
    if(cnet_capsule_store_stage(s,1,"original",1,hash)||cnet_capsule_store_activate(s,1,"initial",hash)) {
        cnet_capsule_store_close(s);return NULL;
    }
    return s;
}
static int answers(CnetCapsuleStore *s) {
    CnetCoreLease *lease=cnet_capsule_store_pin(s);CnetCapsuleCoreReply r;
    int ok=lease&&!cnet_core_host_ask(lease,"capsule cap_in cap_out 2",&r)&&r.verified&&r.value==5;
    cnet_core_host_unpin(lease);return ok;
}
int main(void) {
    char root[]="/tmp/cnet-store-faults-XXXXXX",sets[256],a[300],piece[350],state[300];
    if(!mkdtemp(root))return 2;
    snprintf(sets,sizeof sets,"%s/sets",root);snprintf(a,sizeof a,"%s/original",sets);
    snprintf(piece,sizeof piece,"%s/offset",a);
    if(mkdir(sets,0700)||mkdir(a,0700)||mkdir(piece,0700))return 2;
    CnetBase b;HybridAi h;CnetCapsuleReport rep;cnb_init(&b);hybrid_ai_init(&h);
    check(!build_unit(&b,&h,"offset")&&!cnet_capsule_export(&b,&h,"offset",piece,&rep),"independent fixture exports");
    const char *points[]={"record","before_rename","after_rename"};
    for(unsigned i=0;i<3;i++) {
        snprintf(state,sizeof state,"%s/failure%u",root,i);
        CnetCapsuleStore *s=start(sets,state);check(s!=NULL,"prepare original durable state");if(!s)return 1;
        setenv("CNET_CAPSULE_STORE_FAIL_SYNC",points[i],1);
        int rc=cnet_capsule_store_unload(s,2,"fault");unsetenv("CNET_CAPSULE_STORE_FAIL_SYNC");
        check(rc==(i==2?-2:-1),"failure reports exact side of visibility commit");
        CnetCapsuleStoreStatus st;
        check(!cnet_capsule_store_status(s,&st)&&st.revision==2&&st.durability_uncertain==(i==2),"failed publication retains volatile revision and uncertainty");
        check(answers(s),"incumbent remains usable after failed publication");
        if(i==2) {
            check(cnet_capsule_store_unload(s,2,"fault")!=0,"uncertainty freezes mutation, not ASK");
            check(!cnet_capsule_store_close(s),"close uncertain process");s=cnet_capsule_store_open(sets,state);
            check(s&&!cnet_capsule_store_status(s,&st)&&st.revision==3&&!st.durability_uncertain,"restart reconciles postrename selection");if(!s)return 1;
            check(!cnet_capsule_store_unload(s,2,"fault"),"uncertain lost-response retry resolves without double commit");
        } else check(!cnet_capsule_store_unload(s,2,"fault"),"precommit failure retries prepared operation");
        check(!answers(s),"successful or recovered unload stays terminal");
        check(!cnet_capsule_store_rollback(s,3,"restore")&&answers(s),"postfailure rollback restores independently verified answer");
        cnet_capsule_store_close(s);
    }
    for(unsigned i=0;i<3;i++) {
        snprintf(state,sizeof state,"%s/crash%u",root,i);
        CnetCapsuleStore *s=start(sets,state);check(s!=NULL,"prepare crash boundary");if(!s)return 1;
        cnet_capsule_store_close(s);
        pid_t child=fork();if(child<0)return 2;
        if(!child){s=cnet_capsule_store_open(sets,state);if(!s)_exit(90);
            setenv("CNET_CAPSULE_STORE_CRASH_SYNC",points[i],1);
            (void)cnet_capsule_store_unload(s,2,"crash");_exit(91);}
        int status=0;check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,"actual child terminates at injected publication boundary");
        s=cnet_capsule_store_open(sets,state);CnetCapsuleStoreStatus st;
        check(s&&!cnet_capsule_store_status(s,&st)&&st.revision==(i==2?3:2),"crash recovery restores one complete selection");if(!s)return 1;
        check(pending_records(state)==0,"CAPSULE_STORE_ORPHAN_RED recovery bounds abandoned selection records");
        check(answers(s)==(i!=2),"crash does not expose a mixed inventory");cnet_capsule_store_close(s);
    }
    cnb_free(&b);hybrid_ai_free(&h);
    printf("CAPSULE_STORE_FAULTS_%s checks=%d failures=%d artifacts=%s\n",failures?"RED":"PASS",checks,failures,root);
    return failures?1:0;
}
