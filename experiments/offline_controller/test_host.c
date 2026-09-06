#define _GNU_SOURCE
#include "cnet_core_host.h"
#include "selector_fixture.h"
#include "cnet_cell_capsule.h"
#include "cnet_capsule.h"
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static void *request_loop(void *arg){CnetCoreLease *p=arg;for(unsigned i=0;i<100;i++){CnetCapsuleCoreReply r;assert(!cnet_core_host_ask(p,"capsule LOCAL_STATE REACH_FLAG 3",&r)&&r.verified&&r.value==1);}return NULL;}
static void split_coverage(const CnetCoreCell *cell,const char *root){
    assert(!mkdir(root,0700));
    for(unsigned part=0;part<2;part++){
        double x[12],y[4];for(unsigned row=0;row<4;row++){unsigned b=part*4+row;y[row]=b!=0;for(unsigned i=0;i<3;i++)x[row*3+i]=(b>>(2-i))&1;}
        char name[16],path[512];snprintf(name,sizeof name,"part%u",part);assert(snprintf(path,sizeof path,"%s/%s",root,name)<(int)sizeof path);
        Port pin={PORT_BINARY_MSB,3,1,"LOCAL_STATE"},pout={PORT_BINARY_MSB,1,1,"REACH_FLAG"};BinaryTransformNetwork btn={0};Contract ct={0};
        assert(!cnet_core_cell_to_btn(cell,pin,pout,&btn));assert(!contract_init_borrowed(&ct,name,&btn,x,y,4));
        CnetBase b;HybridAi cov;CnetCapsuleReport report;cnb_init(&b);hybrid_ai_init(&cov);
        assert(!cnb_add_unit(&b,&btn,&ct,NULL));assert(!hybrid_coverage_record(&cov,pin,pout,name,x,y,4,3,1));
        assert(!cnet_capsule_export(&b,&cov,name,path,&report));cnb_free(&b);hybrid_ai_free(&cov);contract_free(&ct);btn_free(&btn);
    }
}
int main(int argc,char **argv){
    assert(argc==4);CnetCoreCandidate c={0};FILE *file=fopen(argv[1],"rb");assert(file);
    assert(fread(&c.cell,sizeof c.cell,1,file)==1&&fgetc(file)==EOF);assert(!fclose(file));
    memset(c.training_sha256,'1',64);
    CnetSelectorGraph graphs[128];for(unsigned i=0;i<128;i++)selector_stress_generate(graphs+i,i%5,8u<<(i%4),910011+i*777,i%8<4);
    CnetCoreShadowCase shadow[9]={0};for(unsigned i=0;i<9;i++){
        snprintf(shadow[i].request,sizeof shadow[i].request,"capsule LOCAL_STATE REACH_FLAG %u",i);shadow[i].verified=i<8;shadow[i].expected=i<8&&(i!=0);
    }
    assert(!cnet_core_gate_digest(graphs,128,shadow,9,c.evaluation_sha256));
    char dir[]="/tmp/cnet-host-XXXXXX";assert(mkdtemp(dir));int fd=open(dir,O_RDONLY|O_DIRECTORY);assert(fd>=0);
    assert(!cnet_core_candidate_save_at(fd,"good.core",&c));
    CnetCoreHost *host=cnet_core_host_open(argv[2]);assert(host);CnetCoreLease *old=cnet_core_host_pin(host);assert(old);
    assert(cnet_core_host_close(host)!=0);uint64_t first=cnet_core_host_generation(old),id=0;
    assert(!cnet_core_host_stage(host,fd,"good.core",argv[3],&id));assert(cnet_core_host_activate(host,id)!=0);
    CnetCoreGateReport report;shadow[0].expected=1;assert(cnet_core_host_check(host,id,graphs,128,shadow,9,&report)!=0);shadow[0].expected=0;
    assert(!cnet_core_host_check(host,id,graphs,128,shadow,9,&report));assert(report.completed==64&&report.abstained==64);
    assert(!cnet_core_host_activate(host,id));CnetCoreLease *newer=cnet_core_host_pin(host);assert(newer&&cnet_core_host_generation(newer)==id&&id!=first);
    assert(cnet_core_host_generation(old)==first);CnetCapsuleCoreReply r;
    CnetCoreLease *leases[62];for(unsigned i=0;i<62;i++){leases[i]=cnet_core_host_pin(host);assert(leases[i]);}
    assert(!cnet_core_host_pin(host));for(unsigned i=0;i<62;i++)cnet_core_host_unpin(leases[i]);
    assert(cnet_core_host_ask(old,"capsule LOCAL_STATE REACH_FLAG 3",&r)!=0&&!r.verified);
    pthread_t thread;assert(!pthread_create(&thread,NULL,request_loop,newer));
    assert(!cnet_core_host_rollback(host));CnetCoreLease *rolled=cnet_core_host_pin(host);assert(rolled&&cnet_core_host_generation(rolled)==first);
    assert(!pthread_join(thread,NULL));assert(!cnet_core_host_ask(newer,"capsule LOCAL_STATE REACH_FLAG 3",&r)&&r.verified);
    assert(!cnet_core_host_stage(host,fd,"good.core",argv[3],&id));assert(!cnet_core_host_check(host,id,graphs,128,shadow,9,&report));
    assert(!cnet_core_host_rollback(host));assert(cnet_core_host_activate(host,id)!=0);assert(!cnet_core_host_discard(host,id));
    assert(!cnet_core_host_rollback(host));
    cnet_core_host_unpin(old);cnet_core_host_unpin(rolled);
    CnetCoreShadowCase sparse[]={shadow[0],shadow[1],shadow[8]};
    assert(!cnet_core_gate_digest(graphs,128,sparse,3,c.evaluation_sha256));
    assert(!cnet_core_candidate_save_at(fd,"split.core",&c));char split[256];snprintf(split,sizeof split,"%s/split",dir);split_coverage(&c.cell,split);
    assert(!cnet_core_host_stage(host,fd,"split.core",split,&id));
    assert(cnet_core_host_check(host,id,graphs,128,sparse,3,&report)!=0);assert(cnet_core_host_activate(host,id)!=0);assert(!cnet_core_host_discard(host,id));
    assert(!cnet_core_gate_digest(graphs,128,shadow,9,c.evaluation_sha256));
    c.cell.weight[40]=-100;assert(!cnet_core_candidate_save_at(fd,"bad.core",&c));
    assert(!cnet_core_host_stage(host,fd,"bad.core",argv[3],&id));assert(cnet_core_host_check(host,id,graphs,128,shadow,9,&report)!=0);
    assert(cnet_core_host_activate(host,id)!=0);assert(!cnet_core_host_discard(host,id));
    cnet_core_host_unpin(newer);assert(!cnet_core_host_close(host));
    assert(!unlinkat(fd,"good.core",0)&&!unlinkat(fd,"bad.core",0)&&!unlinkat(fd,"split.core",0));
    for(unsigned part=0;part<2;part++){char path[256];snprintf(path,sizeof path,"split/part%u/unit.cnb",part);assert(!unlinkat(fd,path,0));snprintf(path,sizeof path,"split/part%u/manifest.cknow",part);assert(!unlinkat(fd,path,0));snprintf(path,sizeof path,"split/part%u",part);assert(!unlinkat(fd,path,AT_REMOVEDIR));}
    assert(!unlinkat(fd,"split",AT_REMOVEDIR));close(fd);assert(!rmdir(dir));
    puts("CORE_HOST_PASS unchecked_bad_stale_evidence_refused=1 pinned_model_and_registry=1 explicit_activation_rollback=1 concurrent_pinned_request=1");
}
