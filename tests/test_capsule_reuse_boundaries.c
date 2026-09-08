/* Compile the production callback locally to inject expectations, not a weakened
 * alternative validator or a production fault hook. */
#define main original_capsule_fixture_main
#include "test_knowledge_capsule.c"
#undef main
#include <dirent.h>
#include <fcntl.h>
#define cnet_capsule_core_reuse fixture_reuse
#include "../src/serve/cnet_capsule_reuse.c"
#undef cnet_capsule_core_reuse

static int empty_directory(const char *path) {
    DIR *dir=opendir(path);if(!dir)return 0;
    struct dirent *entry;int empty=1;
    while((entry=readdir(dir)))if(strcmp(entry->d_name,".")&&strcmp(entry->d_name,".."))empty=0;
    closedir(dir);return empty;
}

int main(void) {
    char root[]="/tmp/cnet-reuse-boundaries-XXXXXX",source[256],destination[256],unit[300];
    if(!mkdtemp(root))return 2;
    snprintf(source,sizeof source,"%s/source",root);snprintf(destination,sizeof destination,"%s/destination",root);
    if(mkdir(source,0700)||mkdir(destination,0700))return 2;
    snprintf(unit,sizeof unit,"%s/physical-name",source);
    CnetBase base;HybridAi coverage;CnetCapsuleReport report;
    cnb_init(&base);hybrid_ai_init(&coverage);
    check(!build_unit(&base,&coverage,"offset")&&
          !cnet_capsule_export(&base,&coverage,"offset",unit,&report)&&!chmod(unit,0700),
          "independently labelled owner-sealed callback fixture");
    int src=open(source,O_RDONLY|O_DIRECTORY),dst=open(destination,O_RDONLY|O_DIRECTORY);
    CnetCapsuleCore *core=open_pinned(src);
    check(core!=NULL,"open exact callback source");
    if(!core)return 2;
    CnetCapsuleIdentity identities[1];
    ReuseCheck expected={.request="capsule cap_in cap_out 0",.identities=identities,.selected=1};
    check(!cnet_capsule_core_identities(core,identities,1,&expected.count)&&
          !cnet_capsule_core_ask(core,expected.request,&expected.expected),"original verified receipt and identities");
    const char *names[]={"physical-name"};char digest[65];size_t bytes;
    for(unsigned fault=0;fault<7;fault++) {
        ReuseCheck altered=expected;CnetCapsuleIdentity wrong=identities[0];
        if(fault==0){wrong.sha256[0]=wrong.sha256[0]=='a'?'b':'a';altered.identities=&wrong;}
        if(fault==1)altered.expected.value++;
        if(fault==2)altered.expected.hops++;
        if(fault==3)strcpy(altered.expected.units,"different_unit");
        if(fault==4)altered.selected++;
        if(fault==5)altered.count=0;
        if(fault==6)altered.request="capsule cap_in cap_out 5";
        check(cnet_capsule_snapshot_selected(src,dst,names,1,validate_subset,&altered,digest,&bytes)!=0&&
              !digest[0]&&!bytes&&empty_directory(destination),"identity/route/replay mismatch refuses before publication");
    }
    check(!cnet_capsule_snapshot_selected(src,dst,names,1,validate_subset,&expected,digest,&bytes)&&bytes,
          "correct identities and replay publish");
    CnetCapsuleCoreReply reply={.verified=1,.value=99,.hops=1};strcpy(digest,"old");bytes=99;
    check(fixture_reuse(NULL,destination,expected.request,digest,&bytes,&reply)!=0&&
          !reply.verified&&!reply.value&&!reply.hops&&!reply.units[0]&&!digest[0]&&!bytes,
          "bad API input clears prior successful answer fields");
    cnet_capsule_core_close(core);close(src);close(dst);cnb_free(&base);hybrid_ai_free(&coverage);
    printf("CAPSULE_REUSE_BOUNDARIES_%s checks=%d failures=%d artifacts=%s\n",failures?"RED":"PASS",checks,failures,root);
    return failures?1:0;
}
