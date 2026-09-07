#include "cnet_capsule_core.h"
#include "cnet_capsule_table.h"
#include "cnet_learning_sandbox.h"
#include "../src/serve/cnet_capsule_snapshot.h"
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Native observations only, never independent reference labels or approval.
 * The parent binds these observations to its frozen capsule snapshot identity;
 * the existing snapshot API has no read-only digest-construction entry point. */
static int number(const char *text,unsigned minimum,unsigned maximum,unsigned *out) {
    if(!text[0]||(text[0]=='0'&&text[1]))return -1;
    unsigned value=0;
    for(const unsigned char *p=(const unsigned char *)text;*p;p++) {
        if(*p<'0'||*p>'9')return -1;
        unsigned digit=*p-'0';
        if(value>maximum/10||(value==maximum/10&&digit>maximum%10))return -1;
        value=value*10+digit;
    }
    if(value<minimum)return -1;
    *out=value;return 0;
}
static int absolute(const char *path) {
    return path&&path[0]=='/'&&strlen(path)<4096;
}
static int hash(const char *text) {
    if(strlen(text)!=64)return 0;
    for(unsigned i=0;i<64;i++)if(!((text[i]>='0'&&text[i]<='9')||(text[i]>='a'&&text[i]<='f')))return 0;
    return 1;
}
static int refused(const char *reason) {
    fprintf(stderr,"TABLE_VERIFY_REFUSED %s\n",reason);return 1;
}
/* Exercise the same text boundary as a live symbol query. Labels below are
 * native observations, never copied out of the source as expected answers. */
static int symbol_observation(CnetCapsuleCore *core,const char *dataset,const char *token,
                              int unknown,char *report,size_t cap,size_t *used) {
    char request[96],text[CNET_CAPSULE_SYMBOL_MAX_LABEL+1];CnetCapsuleCoreReply reply;
    int n=snprintf(request,sizeof request,"symbol %s %s",dataset,token);
    if(n<0||(size_t)n>=sizeof request)return -1;
    int rc=cnet_capsule_core_ask_text(core,request,&reply,text,sizeof text);
    if((rc==0&&reply.verified!=1)||(rc!=0&&reply.verified!=0)||
       (reply.verified&&(!text[0]||reply.value>65535)))return -1;
    for(const unsigned char *p=(const unsigned char *)text;*p;p++)if(*p<32||*p>126)return -1;
    n=snprintf(report+*used,cap-*used,"%s%s\t%d\t%s\n",unknown?"unknown ":"",token,
               reply.verified,reply.verified?text:"-");
    if(n<0||(size_t)n>=cap-*used)return -1;
    *used+=(size_t)n;return 0;
}
static int evaluate(const char *data,const char *dataset,const char *candidate,const char *snapshot) {
    CnetCapsuleTable source;
    if(cnet_capsule_table_read(data,dataset,&source,NULL,0))return refused("source");
    /* The guard cleared the launcher environment. This is the only variable
     * restored, from the source root just validated by the owner-table reader. */
    if(setenv("CNET_CAPSULE_DATA_ROOT",data,1))return refused("data_root");
    int directory=-1,cache=-1;size_t snapshot_bytes=0;
    if(snapshot) {
        cache=cnet_capsule_owner_directory(candidate,1);
        if(cache<0)return refused("snapshot_boundary");
        if(cnet_capsule_snapshot_verify(cache,snapshot,&snapshot_bytes)){
            close(cache);return refused("snapshot");
        }
        directory=openat(cache,snapshot,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    } else directory=cnet_capsule_owner_directory(candidate,1);
    if(directory<0){if(cache>=0)close(cache);return refused("candidate_boundary");}
    char pinned[64],error[160];
    int n=snprintf(pinned,sizeof pinned,"/proc/self/fd/%d",directory);
    if(n<0||(size_t)n>=sizeof pinned){close(directory);if(cache>=0)close(cache);return refused("candidate_boundary");}
    CnetCapsuleCore *core=cnet_capsule_core_open(pinned,error,sizeof error);
    if(!core){close(directory);if(cache>=0)close(cache);return refused("candidate");}

    const char *failure=NULL;
    char report[16384];const char *kind=source.symbolic?"SYMBOL":"TABLE";
    if(snapshot)n=snprintf(report,sizeof report,"CNET_%s_SNAPSHOT_EVAL_V1\nsnapshot_sha256 %s\ndataset %s\nsource_sha256 %s\nresults 256\n",
                          kind,snapshot,dataset,source.sha256);
    else n=snprintf(report,sizeof report,"CNET_%s_NATIVE_EVAL_V1\ndataset %s\nsource_sha256 %s\nresults 256\n",
                    kind,dataset,source.sha256);
    size_t used=0;
    if(n<0||(size_t)n>=sizeof report){failure="report_limit";goto done;}
    used=(size_t)n;
    for(unsigned key=0;key<256;key++) {
        char request[80];CnetCapsuleCoreReply reply;
        n=snprintf(request,sizeof request,"data %s %u",dataset,key);
        if(n<0||(size_t)n>=sizeof request){failure="request_limit";goto done;}
        int rc=cnet_capsule_core_ask(core,request,&reply);
        if((rc==0&&reply.verified!=1)||(rc!=0&&reply.verified!=0)||
           (reply.verified&&reply.value>65535)){failure="runtime_reply";goto done;}
        if(reply.verified)n=snprintf(report+used,sizeof report-used,"%u\t1\t%u\n",key,reply.value);
        else n=snprintf(report+used,sizeof report-used,"%u\t0\t-\n",key);
        if(n<0||(size_t)n>=sizeof report-used){failure="report_limit";goto done;}
        used+=(size_t)n;
    }
    if(source.symbolic){
        n=snprintf(report+used,sizeof report-used,"symbols %u\n",source.count);
        if(n<0||(size_t)n>=sizeof report-used){failure="report_limit";goto done;}
        used+=(size_t)n;
        char token[CNET_CAPSULE_SYMBOL_MAX_KEY+1];
        for(unsigned row=0;row<source.count;row++){
            if(cnet_capsule_table_key_at(&source,row,token,sizeof token)||
               symbol_observation(core,dataset,token,0,report,sizeof report,&used)){
                failure="symbol_runtime_reply_or_limit";goto done;
            }
        }
        unsigned candidate;
        for(candidate=0;candidate<=CNET_CAPSULE_TABLE_MAX_ROWS;candidate++){
            snprintf(token,sizeof token,"cnet_unknown_%u",candidate);
            unsigned unused;
            if(cnet_capsule_table_symbol_index(&source,token,&unused))break;
        }
        if(candidate>CNET_CAPSULE_TABLE_MAX_ROWS||
           symbol_observation(core,dataset,token,1,report,sizeof report,&used)){
            failure="symbol_unknown_reply_or_limit";goto done;
        }
    }
    /* No local label comparison. Source freshness here and in each native
     * query is not a transaction with a concurrent owner source publisher. */
    if(cnet_capsule_table_fresh(&source,data)){failure="source_changed";goto done;}
    if(snapshot) {
        size_t after=0;
        if(cnet_capsule_snapshot_verify(cache,snapshot,&after)||after!=snapshot_bytes){failure="snapshot_changed";goto done;}
    }
    n=snprintf(report+used,sizeof report-used,"end\n");
    if(n<0||(size_t)n>=sizeof report-used){failure="report_limit";goto done;}
    used+=(size_t)n;
done:
    cnet_capsule_core_close(core);
    if(close(directory)&&!failure)failure="candidate_boundary";
    if(cache>=0&&close(cache)&&!failure)failure="snapshot_boundary";
    if(failure)return refused(failure);
    if(fwrite(report,1,used,stdout)!=used||fflush(stdout))return refused("output");
    return 0;
}
int main(int argc,char **argv) {
    unsigned parent,cpu,memory;
    int staged=argc==10&&!strcmp(argv[1],"snapshot-worker");
    if((!staged&&(argc!=9||strcmp(argv[1],"verify-worker")))||
       !absolute(argv[2])||!cnet_capsule_table_dataset(argv[3])||!absolute(argv[4])||
       (staged&&!hash(argv[5]))||
       number(argv[6+staged],2,INT_MAX,&parent)||number(argv[7+staged],1,120,&cpu)||
       number(argv[8+staged],64,2048,&memory))return refused("worker_arguments");
    if(cnet_learning_sandbox_enter_for_parent(argv[5+staged],cpu,(size_t)memory*1024*1024,
                                            16u*1024u*1024u,(int)parent))
        return refused("worker_sandbox");
    return evaluate(argv[2],argv[3],argv[4],staged?argv[5]:NULL);
}
