#include "cnet_capsule_reuse_internal.h"
#include "cnet_capsule_snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    const char *request;
    CnetCapsuleCoreReply expected;
    CnetCapsuleIdentity *identities;
    size_t count,selected;
} ReuseCheck;

static CnetCapsuleCore *open_pinned(int fd) {
    char path[64];snprintf(path,sizeof path,"/proc/self/fd/%d",fd);
    return cnet_capsule_core_open(path,NULL,0);
}

static int validate_subset(int fd,void *context) {
    ReuseCheck *check=context;
    CnetCapsuleCore *candidate=open_pinned(fd);
    if(!candidate)return -1;
    CnetCapsuleIdentity identities[8];size_t count=0;
    CnetCapsuleCoreReply reply={0};
    int bad=cnet_capsule_core_identities(candidate,identities,8,&count)||count!=check->selected;
    for(size_t i=0;i<count&&!bad;i++) {
        size_t j=0;while(j<check->count&&strcmp(identities[i].unit,check->identities[j].unit))j++;
        if(j==check->count||strcmp(identities[i].sha256,check->identities[j].sha256))bad=1;
    }
    if(!bad)bad=cnet_capsule_core_ask(candidate,check->request,&reply)||!reply.verified||
        reply.value!=check->expected.value||reply.hops!=check->expected.hops||
        strcmp(reply.units,check->expected.units);
    cnet_capsule_core_close(candidate);return bad?-1:0;
}

int cnet_capsule_core_reuse(const char *source,const char *destination,
    const char *request,char digest[65],size_t *bytes,CnetCapsuleCoreReply *reply) {
    if(digest)digest[0]=0;
    if(bytes)*bytes=0;
    if(reply)memset(reply,0,sizeof *reply);
    const char *reason="reuse_arguments";
    int src=-1,dst=-1,rc=-1;CnetCapsuleCore *core=NULL;
    ReuseCheck check={.request=request};const char *names[8];
    if(!reply||!bytes||!digest||!source||!destination||!request||
       source[0]!='/'||destination[0]!='/'||strnlen(request,256)>255)goto done;
    reason="reuse_source_boundary";
    src=cnet_capsule_owner_directory(source,1);if(src<0)goto done;
    reason="reuse_destination_boundary";
    dst=cnet_capsule_owner_directory(destination,1);if(dst<0)goto done;
    /* Bound and validate ALL source entries before core_open's scandir/parser.
     * The copier repeats whole-source validation, even for unselected files. */
    reason="reuse_source_inventory";
    if(cnet_capsule_snapshot_inspect(src))goto done;
    reason="reuse_source_core";
    core=open_pinned(src);if(!core)goto done;
    reason="reuse_request_unverified";
    if(cnet_capsule_core_ask(core,request,&check.expected)||!check.expected.verified)goto done;
    reason="reuse_selection";
    if(cnet_capsule_core_selected_directories(core,&check.expected,names,&check.selected))goto done;
    check.identities=calloc(4096,sizeof *check.identities);
    if(!check.identities||cnet_capsule_core_identities(core,check.identities,4096,&check.count))goto done;
    reason="reuse_subset_validation_or_publication";
    if(cnet_capsule_snapshot_selected(src,dst,names,check.selected,validate_subset,&check,digest,bytes))goto done;
    *reply=check.expected;rc=0;
done:
    free(check.identities);cnet_capsule_core_close(core);
    if(src>=0&&close(src)){rc=-1;reason="reuse_close";}
    if(dst>=0&&close(dst)){rc=-1;reason="reuse_close";}
    if(rc) {
        if(digest)digest[0]=0;
        if(bytes)*bytes=0;
        if(reply){memset(reply,0,sizeof *reply);snprintf(reply->reason,sizeof reply->reason,"%s",reason);}
    }
    return rc;
}
