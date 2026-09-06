#define _GNU_SOURCE
#include "cnet_cell_capsule.h"
#include "cnet_capsule.h"
#include "cnet_capsule_core.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
int main(int argc,char **argv){
    assert(argc==3);CnetCoreCell cell;FILE *f=fopen(argv[1],"rb");assert(f);
    assert(fread(&cell,sizeof cell,1,f)==1&&fgetc(f)==EOF);assert(!fclose(f));
    double x[24],y[8];for(unsigned b=0;b<8;b++){y[b]=b!=0;for(unsigned i=0;i<3;i++)x[b*3+i]=(b>>(2-i))&1;}
    Port pin={PORT_BINARY_MSB,3,1,"LOCAL_STATE"},pout={PORT_BINARY_MSB,1,1,"REACH_FLAG"};
    BinaryTransformNetwork btn={0};Contract ct={0};CertifyReport cert;
    CnetCoreCell zero={{0}};
    assert(!cnet_core_cell_to_btn(&zero,pin,pout,&btn));
    assert(!contract_init_borrowed(&ct,"gpu_local_or",&btn,x,y,8));
    assert(btn_certify_robust(&btn,&ct,.05,&cert)!=0);contract_free(&ct);btn_free(&btn);
    zero.weight[0]=NAN;assert(cnet_core_cell_to_btn(&zero,pin,pout,&btn)!=0);
    Port wrong=pin;wrong.field_width=4;assert(cnet_core_cell_to_btn(&cell,wrong,pout,&btn)!=0);
    assert(!cnet_core_cell_to_btn(&cell,pin,pout,&btn));double max_error=0;
    for(unsigned b=0;b<8;b++){
        float features[3],score;for(unsigned i=0;i<3;i++)features[i]=(float)x[b*3+i];
        assert(!cnet_core_cell_predict(&cell,features,&score));const double *v=btn_forward(&btn,x+b*3);assert(v&&isfinite(v[0]));
        max_error=fmax(max_error,fabs(v[0]-score));
    }
    assert(max_error<1e-6);assert(!contract_init_borrowed(&ct,"gpu_local_or",&btn,x,y,8));
    assert(!btn_certify_robust(&btn,&ct,.05,&cert));
    CnetBase base,copy;HybridAi coverage,guard;CnetCapsuleReport report;
    cnb_init(&base);cnb_init(&copy);hybrid_ai_init(&coverage);hybrid_ai_init(&guard);
    assert(!cnb_add_unit(&base,&btn,&ct,NULL));
    assert(!cnb_add_oracle_desc(&base,"verified_tool","verified_tool",pin,pout));
    assert(!cnb_set_unit_provenance(&base,"gpu_local_or","verified_tool"));
    assert(!hybrid_coverage_record(&coverage,pin,pout,"gpu_local_or",x,y,8,3,1));
    assert(!mkdir(argv[2],0700));char capsule[1024],empty[1024],error[160];
    assert(snprintf(capsule,sizeof capsule,"%s/gpu_local_or",argv[2])<(int)sizeof capsule);
    assert(snprintf(empty,sizeof empty,"%s/empty",argv[2])<(int)sizeof empty);assert(!mkdir(empty,0700));
    assert(!cnet_capsule_export(&base,&coverage,"gpu_local_or",capsule,&report));
    assert(!cnet_capsule_import(&copy,&guard,capsule,&report));
    CnetCapsuleCore *old=cnet_capsule_core_open(empty,error,sizeof error);assert(old);
    CnetCapsuleCore *next=cnet_capsule_core_open_candidate(empty,capsule,error,sizeof error);assert(next);
    size_t obligations=0;assert(!cnet_capsule_core_validate_growth(old,next,&obligations));
    for(unsigned b=0;b<8;b++){char request[96];CnetCapsuleCoreReply reply;
        snprintf(request,sizeof request,"capsule LOCAL_STATE REACH_FLAG %u",b);
        assert(!cnet_capsule_core_ask(next,request,&reply)&&reply.verified&&reply.value==(b!=0));
        assert(!cnet_capsule_core_ask_cell(next,request,&cell,37,&reply)&&reply.verified&&reply.value==(b!=0));
    }
    CnetCapsuleCoreReply reply;assert(cnet_capsule_core_ask(next,"capsule LOCAL_STATE REACH_FLAG 8",&reply)!=0&&!reply.verified);
    assert(cnet_capsule_core_ask_cell(next,"capsule LOCAL_STATE REACH_FLAG 8",&cell,37,&reply)!=0&&!reply.verified);
    printf("GPU_CELL_CAPSULE_PASS method=direct_FP32_BTN_copy domain_rows=8 minimum_margin=%.9f prediction_max_abs=%.9g payload_bytes=%zu growth_obligations=%zu ood_refused=1 weak_margin_refused=1\n",cert.min_margin,max_error,report.payload_bytes,obligations);
    cnet_capsule_core_close(old);cnet_capsule_core_close(next);hybrid_ai_free(&coverage);hybrid_ai_free(&guard);cnb_free(&base);cnb_free(&copy);contract_free(&ct);btn_free(&btn);
}
