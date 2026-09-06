#include "resident.h"
#include <assert.h>
#include <math.h>
#include <float.h>
#include <stdio.h>
#include <string.h>
static Net reference,snapshot,after,initial;
static Scratch scratch;
static float batches[8][BATCH*INPUTS],labels[8][BATCH*ACTIONS],p[BATCH*OUTPUTS];
int main(int argc,char **argv) {
    assert(argc==1||(argc==2&&!strcmp(argv[1],"forward")));
    /* Exactly the preexisting bench_train 8-minibatch/128-update protocol.
     * Constant-batch LR=.15 is separately retained as an unstable trajectory
     * diagnostic: it diverges even between CPU and the old host-GPU backend. */
    uint32_t rng=471;
    for(int c=0;c<8;c++)for(int b=0;b<BATCH;b++){Task t;task_generate(&t,&rng);task_features(&t,t.start,batches[c]+b*INPUTS);task_teacher(&t,t.start,labels[c]+b*ACTIONS);}
    net_init(&reference,4,1,997);
    assert(!resident_open(&reference,-1));assert(!resident_open(NULL,0));
    for(int device=0;device<2;device++) {
        float *x=batches[0],*y=labels[0];
        net_init(&reference,4,1,997);initial=reference;Resident *r=resident_open(&reference,device);assert(r);
#ifdef CONTROLLER_ROCBLAS
        assert(!strcmp(resident_backend(r),"rocblas_fp32"));
#endif
        assert(!resident_snapshot(r,&snapshot));assert(!memcmp(&reference,&snapshot,sizeof snapshot));
        x=batches[0];assert(net_step(&reference,&scratch,x,NULL,BATCH,0,NULL)==0);
        assert(!resident_predict(r,x,BATCH,p));
        for(int b=0;b<BATCH;b++)for(int a=0;a<ACTIONS;a++)assert(fabsf(scratch.p[b*OUTPUTS+a]-p[b*OUTPUTS+a])<1e-4f);
        if(argc==2){printf("CONTROLLER_RESIDENT_FORWARD_PASS device=%d\n",device);resident_close(r);continue;}
        float loss=0;
        assert(resident_step(r,x,y,0,.15f,&loss)!=0);
        assert(resident_step(r,x,y,BATCH+1,.15f,&loss)!=0);
        float saved=y[0];y[0]=NAN;assert(resident_step(r,x,y,BATCH,.15f,&loss)!=0);y[0]=saved;
        assert(!resident_snapshot(r,&after));assert(!memcmp(&snapshot,&after,sizeof after));
        for(int i=0;i<128;i++) {
            x=batches[i%8];y=labels[i%8];
            float expected=net_step(&reference,&scratch,x,y,BATCH,.15f,NULL);assert(expected>0);
            assert(!resident_step(r,x,y,BATCH,.15f,&loss));
            if(fabsf(loss-expected)>=1e-4f)fprintf(stderr,"CONTROLLER_RESIDENT_PARITY_RED device=%d update=%d cpu=%.9g gpu=%.9g delta=%.9g\n",device,i+1,expected,loss,fabsf(loss-expected));
            assert(fabsf(loss-expected)<1e-4f);
        }
        assert(!resident_snapshot(r,&after));float error=0;
        for(int t=0;t<DEPTH;t++)for(int j=0;j<HIDDEN*JOINED;j++){
            assert(isfinite(reference.w[t][j])&&isfinite(after.w[t][j]));error=fmaxf(error,fabsf(reference.w[t][j]-after.w[t][j]));
        }
        for(int j=0;j<OUTPUTS*(HIDDEN+1);j++){
            assert(isfinite(reference.out[j])&&isfinite(after.out[j]));error=fmaxf(error,fabsf(reference.out[j]-after.out[j]));
        }
        assert(error<1e-4f);assert(memcmp(&snapshot,&after,sizeof after));
        assert(!memcmp(&snapshot,&initial,sizeof initial));
        x=batches[0];assert(net_step(&reference,&scratch,x,NULL,BATCH,0,NULL)==0);
        assert(!resident_predict(r,x,BATCH,p));
        for(int b=0;b<BATCH;b++)for(int a=0;a<ACTIONS;a++)assert(fabsf(scratch.p[b*OUTPUTS+a]-p[b*OUTPUTS+a])<1e-4f);
        assert(!resident_snapshot(r,&snapshot));assert(!memcmp(&snapshot,&after,sizeof after));
        printf("CONTROLLER_RESIDENT_PASS device=%d updates=128 weight_max_abs=%.9g invalid_batch_refused=1 snapshot_immutable=1\n",device,error);
        resident_close(r);
        r=resident_open(&initial,device);assert(r);
        float overflow[BATCH*INPUTS];for(int i=0;i<BATCH*INPUTS;i++)overflow[i]=FLT_MAX;
        assert(resident_predict(r,overflow,BATCH,p)!=0);
        assert(resident_error(r)[0]);
        assert(resident_snapshot(r,&after)!=0);
        assert(resident_predict(r,x,BATCH,p)!=0);
        assert(resident_step(r,x,y,BATCH,.15f,&loss)!=0);
        resident_close(r);
        printf("CONTROLLER_RESIDENT_POISON_PASS device=%d finite_overflow_refused=1 later_operations_refused=3\n",device);
        r=resident_open(&initial,device);assert(r);assert(resident_test_device_failure(r)!=0);
        assert(resident_snapshot(r,&after)!=0);assert(resident_predict(r,x,BATCH,p)!=0);assert(resident_step(r,x,y,BATCH,.15f,&loss)!=0);resident_close(r);
        for(int depth=1;depth<=4;depth++)for(int tied=0;tied<=1;tied++) {
            net_init(&reference,depth,tied,43);r=resident_open(&reference,device);assert(r);
            const int sizes[]={1,17,128};
            for(int k=0;k<3;k++) {
                float expected=net_step(&reference,&scratch,batches[0],labels[0],sizes[k],.015f,NULL);assert(expected>0);
                assert(!resident_step(r,batches[0],labels[0],sizes[k],.015f,&loss));assert(fabsf(loss-expected)<1e-4f);
                assert(!resident_snapshot(r,&after));
                for(int t=0;t<DEPTH;t++)for(int j=0;j<HIDDEN*JOINED;j++)assert(fabsf(reference.w[t][j]-after.w[t][j])<1e-4f);
                for(int j=0;j<OUTPUTS*(HIDDEN+1);j++)assert(fabsf(reference.out[j]-after.out[j])<1e-4f);
            }
            resident_close(r);
        }
        printf("CONTROLLER_RESIDENT_SHAPES_PASS device=%d depth_modes=8 batch_sizes=3 runtime_error_injection=1\n",device);
        r=resident_open(&initial,device);assert(r);float losses[8];
        assert(resident_steps(r,&batches[0][0],&labels[0][0],9,BATCH,.15f,losses)!=0);
        assert(resident_steps(r,&batches[0][0],&labels[0][0],0,BATCH,.15f,losses)!=0);
        float last=labels[7][BATCH*ACTIONS-1];labels[7][BATCH*ACTIONS-1]=NAN;
        for(int i=0;i<8;i++)losses[i]=37;
        assert(resident_steps(r,&batches[0][0],&labels[0][0],8,BATCH,.15f,losses)!=0);
        for(int i=0;i<8;i++)assert(losses[i]==37);
        labels[7][BATCH*ACTIONS-1]=last;
        assert(!resident_snapshot(r,&after));assert(!memcmp(&after,&initial,sizeof after));
        reference=initial;
        assert(!resident_steps(r,&batches[0][0],&labels[0][0],8,BATCH,.15f,losses));
        for(int i=0;i<8;i++){
            float expected=net_step(&reference,&scratch,batches[i],labels[i],BATCH,.15f,NULL);
            assert(isfinite(losses[i])&&fabsf(expected-losses[i])<1e-4f);
        }
        assert(!resident_snapshot(r,&after));
        for(int t=0;t<DEPTH;t++)for(int j=0;j<HIDDEN*JOINED;j++)assert(fabsf(reference.w[t][j]-after.w[t][j])<1e-4f);
        for(int j=0;j<OUTPUTS*(HIDDEN+1);j++)assert(fabsf(reference.out[j]-after.out[j])<1e-4f);
        resident_close(r);
        printf("CONTROLLER_RESIDENT_BATCHES_PASS device=%d batches=8 oversize_refused=1\n",device);
        r=resident_open(&initial,device);assert(r);
        assert(!resident_test_fail_after_enqueue(r));
        assert(resident_steps(r,&batches[0][0],&labels[0][0],8,BATCH,.15f,losses)!=0);
        assert(resident_test_stream_idle(r));
        assert(resident_snapshot(r,&after)!=0);resident_close(r);
        printf("CONTROLLER_RESIDENT_DRAIN_PASS device=%d pending_failure_drained=1\n",device);
    }
    return 0;
}
