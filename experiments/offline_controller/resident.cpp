#include "resident.h"
#include <hip/hip_runtime.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
struct Resident {
    cce_amdmath *math;int device,depth,tied,poisoned;char error[160];
    Net *weights;Scratch *scratch;float *x,*labels,*losses;int *failed;
};
static int checked(Resident *r,hipError_t status,const char *operation) {
    if(status==hipSuccess)return 0;
    r->poisoned=1;std::snprintf(r->error,sizeof r->error,"%s: %s",operation,hipGetErrorString(status));return -1;
}
static int ready(Resident *r){return !r||r->poisoned?-1:checked(r,hipSetDevice(r->device),"select device");}
static bool finite_array(const float *p,size_t n){if(!p)return false;for(size_t i=0;i<n;i++)if(!std::isfinite(p[i]))return false;return true;}
__global__ static void join_input(const float *x,Scratch *s,int n,int t) {
    int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=n*JOINED)return;
    int b=i/JOINED,k=i%JOINED;
    s->z[t*n*JOINED+i]=k<INPUTS?x[b*INPUTS+k]:k==JOINED-1?1.f:t?s->h[((t-1)*n+b)*HIDDEN+k-INPUTS]:0.f;
}
__global__ static void activation(float *h,int size,int *failed) {
    int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=size)return;
    if(!isfinite(h[i]))atomicExch(failed,1);h[i]=tanhf(h[i]);
}
__global__ static void final_input(Scratch *s,int n,int depth) {
    int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=n*(HIDDEN+1))return;
    int b=i/(HIDDEN+1),k=i%(HIDDEN+1);s->final[i]=k==HIDDEN?1.f:s->h[((depth-1)*n+b)*HIDDEN+k];
}
__global__ static void softmax(Scratch *s,const float *y,float *losses,int n,int *failed) {
    int b=blockIdx.x*blockDim.x+threadIdx.x;if(b>=n)return;float *p=s->p+b*OUTPUTS,largest=p[0],sum=0;
    for(int a=0;a<ACTIONS;a++){if(!isfinite(p[a]))atomicExch(failed,1);largest=fmaxf(largest,p[a]);}
    for(int a=0;a<ACTIONS;a++){p[a]=expf(p[a]-largest);sum+=p[a];}
    double loss=0;
    for(int a=0;a<OUTPUTS;a++){
        s->dy[b*OUTPUTS+a]=0;if(a>=ACTIONS){p[a]=0;continue;}
        p[a]/=sum;if(!isfinite(p[a]))atomicExch(failed,1);
        if(y){loss-=y[b*ACTIONS+a]*log(fmax((double)p[a],1e-30));s->dy[b*OUTPUTS+a]=(p[a]-y[b*ACTIONS+a])/n;}
    }
    losses[b]=(float)loss;
}
static int linear(Resident *r,const float *x,const float *w,float *y,int n,int in,int out) {
    if(!cce_amdmath_linear_f32_dev(r->math,x,w,y,1,n,in,out))return 0;
    r->poisoned=1;std::snprintf(r->error,sizeof r->error,"linear: %s",cce_amdmath_last_error(r->math));return -1;
}
static int forward(Resident *r,int n,bool labels) {
    Scratch *s=r->scratch;
    for(int t=0;t<r->depth;t++) {
        join_input<<<(n*JOINED+255)/256,256>>>(r->x,s,n,t);
        if(checked(r,hipGetLastError(),"join launch"))return -1;
        if(linear(r,s->z+t*n*JOINED,r->weights->w[r->tied?0:t],s->h+t*n*HIDDEN,n,JOINED,HIDDEN))return -1;
        activation<<<(n*HIDDEN+255)/256,256>>>(s->h+t*n*HIDDEN,n*HIDDEN,r->failed);
        if(checked(r,hipGetLastError(),"activation launch"))return -1;
    }
    final_input<<<(n*(HIDDEN+1)+255)/256,256>>>(s,n,r->depth);
    if(checked(r,hipGetLastError(),"final input launch"))return -1;
    if(linear(r,s->final,r->weights->out,s->p,n,HIDDEN+1,OUTPUTS))return -1;
    softmax<<<(n+127)/128,128>>>(s,labels?r->labels:nullptr,r->losses,n,r->failed);
    return checked(r,hipGetLastError(),"forward launch");
}
extern "C" void resident_close(Resident *r) {
    if(!r)return;
    if(r->math){
        int failed=checked(r,hipSetDevice(r->device),"close device");
        for(void *p:{(void*)r->weights,(void*)r->scratch,(void*)r->x,(void*)r->labels,(void*)r->losses,(void*)r->failed})
            if(p)failed|=checked(r,hipFree(p),"release allocation");
        if(failed)std::fprintf(stderr,"CONTROLLER_RESIDENT_CLOSE_ERROR %s\n",r->error);
        cce_amdmath_close(r->math);
    }
    std::free(r);
}
extern "C" Resident *resident_open(const Net *initial,int device) {
    if(!initial||initial->depth<1||initial->depth>DEPTH||(initial->tied!=0&&initial->tied!=1)||
       !finite_array(&initial->w[0][0],DEPTH*HIDDEN*JOINED)||!finite_array(initial->out,OUTPUTS*(HIDDEN+1)))return nullptr;
    Resident *r=(Resident*)std::calloc(1,sizeof *r);if(!r)return nullptr;
    r->math=cce_amdmath_open_device(device,nullptr,0);if(!r->math){std::free(r);return nullptr;}
    r->depth=initial->depth;r->tied=initial->tied;
    if(checked(r,hipGetDevice(&r->device),"get device")||checked(r,hipMalloc(&r->weights,sizeof(Net)),"weights")||
       checked(r,hipMalloc(&r->scratch,sizeof(Scratch)),"scratch")||checked(r,hipMalloc(&r->x,BATCH*INPUTS*sizeof(float)),"features")||
       checked(r,hipMalloc(&r->labels,BATCH*ACTIONS*sizeof(float)),"labels")||checked(r,hipMalloc(&r->losses,BATCH*sizeof(float)),"losses")||
       checked(r,hipMalloc(&r->failed,sizeof(int)),"status")||checked(r,hipMemcpy(r->weights,initial,sizeof(Net),hipMemcpyHostToDevice),"initial weights")){
        resident_close(r);return nullptr;
    }
    return r;
}
extern "C" const char *resident_error(const Resident *r){return r?r->error:"null resident context";}
extern "C" int resident_snapshot(Resident *r,Net *out){return !out||ready(r)?-1:checked(r,hipMemcpy(out,r->weights,sizeof(Net),hipMemcpyDeviceToHost),"snapshot");}
extern "C" int resident_predict(Resident *r,const float *x,int n,float *p) {
    if(!p||n<1||n>BATCH||!finite_array(x,(size_t)n*INPUTS)||ready(r))return -1;
    if(checked(r,hipMemset(r->failed,0,sizeof(int)),"clear status")||checked(r,hipMemcpy(r->x,x,n*INPUTS*sizeof(float),hipMemcpyHostToDevice),"features")||forward(r,n,false))return -1;
    int failed=0;if(checked(r,hipMemcpy(&failed,r->failed,sizeof failed,hipMemcpyDeviceToHost),"status"))return -1;
    if(failed){r->poisoned=1;std::snprintf(r->error,sizeof r->error,"nonfinite forward value");return -1;}
    return checked(r,hipMemcpy(p,r->scratch->p,n*OUTPUTS*sizeof(float),hipMemcpyDeviceToHost),"probabilities");
}
__global__ static void transpose_weights(const Net *m,Scratch *s,int t,int out) {
    int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=HIDDEN*out)return;
    int h=i/out,o=i%out;
    s->transpose[i]=out==OUTPUTS?m->out[o*(HIDDEN+1)+h]:m->w[t][o*JOINED+INPUTS+h];
}
__global__ static void derivative(Scratch *s,int n,int t,int *failed) {
    int i=blockIdx.x*blockDim.x+threadIdx.x;if(i>=n*HIDDEN)return;
    float h=s->h[t*n*HIDDEN+i],v=s->dh[i]*(1-h*h);s->dz[t*n*HIDDEN+i]=v;
    if(!isfinite(v))atomicExch(failed,1);
}
__global__ static void check_weights(const float *p,int size,int *failed) {
    int i=blockIdx.x*blockDim.x+threadIdx.x;if(i<size&&!isfinite(p[i]))atomicExch(failed,1);
}
__global__ static void reduce_loss(float *values,int n,int *failed) {
    double sum=0;for(int i=0;i<n;i++)sum+=values[i];
    if(!isfinite(sum))atomicExch(failed,1);values[0]=(float)(sum/n);
}
static int update(Resident *r,float *w,const float *x,const float *dy,int n,int in,int out,float lr) {
    if(!cce_amdmath_sgd_f32_dev(r->math,w,x,dy,1,n,in,out,lr))return 0;
    r->poisoned=1;std::snprintf(r->error,sizeof r->error,"update: %s",cce_amdmath_last_error(r->math));return -1;
}
extern "C" int resident_step(Resident *r,const float *x,const float *y,int n,float lr,float *loss) {
    if(!loss||n<1||n>BATCH||!std::isfinite(lr)||lr<=0||!finite_array(x,(size_t)n*INPUTS)||!finite_array(y,(size_t)n*ACTIONS)||ready(r))return -1;
    for(int b=0;b<n;b++) {
        double sum=0;for(int a=0;a<ACTIONS;a++){float p=y[b*ACTIONS+a];if(p<0||p>1)return -1;sum+=p;}
        if(std::fabs(sum-1)>1e-5)return -1;
    }
    if(checked(r,hipMemset(r->failed,0,sizeof(int)),"clear status")||
       checked(r,hipMemcpy(r->x,x,n*INPUTS*sizeof(float),hipMemcpyHostToDevice),"features")||
       checked(r,hipMemcpy(r->labels,y,n*ACTIONS*sizeof(float),hipMemcpyHostToDevice),"labels")||forward(r,n,true))return -1;
    Scratch *s=r->scratch;
    transpose_weights<<<(HIDDEN*OUTPUTS+255)/256,256>>>(r->weights,s,0,OUTPUTS);
    if(checked(r,hipGetLastError(),"output transpose launch"))return -1;
    if(linear(r,s->dy,s->transpose,s->dh,n,OUTPUTS,HIDDEN))return -1;
    for(int t=r->depth-1;t>=0;t--) {
        derivative<<<(n*HIDDEN+255)/256,256>>>(s,n,t,r->failed);
        if(checked(r,hipGetLastError(),"derivative launch"))return -1;
        if(t){transpose_weights<<<(HIDDEN*HIDDEN+255)/256,256>>>(r->weights,s,r->tied?0:t,HIDDEN);
            if(checked(r,hipGetLastError(),"state transpose launch"))return -1;
            if(linear(r,s->dz+t*n*HIDDEN,s->transpose,s->dh,n,HIDDEN,HIDDEN))return -1;}
    }
    /* All derivatives use frozen weights. Failed numeric state is poisoned and
     * cannot be snapshotted, even if an update already executed on the device. */
    if(update(r,r->weights->out,s->final,s->dy,n,HIDDEN+1,OUTPUTS,lr))return -1;
    if(r->tied){if(update(r,r->weights->w[0],s->z,s->dz,n*r->depth,JOINED,HIDDEN,lr))return -1;}
    else for(int t=0;t<r->depth;t++)if(update(r,r->weights->w[t],s->z+t*n*JOINED,s->dz+t*n*HIDDEN,n,JOINED,HIDDEN,lr))return -1;
    check_weights<<<(DEPTH*HIDDEN*JOINED+255)/256,256>>>(&r->weights->w[0][0],DEPTH*HIDDEN*JOINED,r->failed);
    if(checked(r,hipGetLastError(),"state weight check launch"))return -1;
    check_weights<<<(OUTPUTS*(HIDDEN+1)+255)/256,256>>>(r->weights->out,OUTPUTS*(HIDDEN+1),r->failed);
    if(checked(r,hipGetLastError(),"backward launch"))return -1;
    reduce_loss<<<1,1>>>(r->losses,n,r->failed);
    if(checked(r,hipGetLastError(),"loss reduction launch"))return -1;
    int failed=0;float value=0;
    if(checked(r,hipMemcpy(&failed,r->failed,sizeof failed,hipMemcpyDeviceToHost),"status")||
       checked(r,hipMemcpy(&value,r->losses,sizeof value,hipMemcpyDeviceToHost),"loss"))return -1;
    if(failed||!std::isfinite(value)){r->poisoned=1;std::snprintf(r->error,sizeof r->error,"nonfinite training value");return -1;}
    *loss=value;return 0;
}
#ifdef CONTROLLER_TESTING
extern "C" int resident_test_device_failure(Resident *r){return ready(r)?-1:checked(r,hipErrorInvalidValue,"injected runtime failure");}
#endif
