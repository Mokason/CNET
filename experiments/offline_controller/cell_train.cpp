#include "cell_train.h"
#include "cce/cce_amdmath.h"
#include <hip/hip_runtime.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
struct CellDevice {
    CnetCoreCell model;
    float x[2048*3],y[2048],gradient[2048*41],loss[2048],prediction[2048];
    int failed;
};
struct CellGpu {cce_amdmath *math;hipStream_t stream;CellDevice *data;int device,poisoned;char error[160];};
static int checked(CellGpu *g,hipError_t status,const char *operation){
    if(status==hipSuccess)return 0;
    g->poisoned=1;std::snprintf(g->error,sizeof g->error,"%s: %s",operation,hipGetErrorString(status));return -1;
}
static int ready(CellGpu *g){return !g||g->poisoned?-1:checked(g,hipSetDevice(g->device),"select device");}
static int drain(CellGpu *g){
    hipError_t status=hipStreamSynchronize(g->stream);
    if(status!=hipSuccess)std::fprintf(stderr,"CORE_CELL_DRAIN_ERROR %s\n",hipGetErrorString(status));
    return -1;
}
__device__ static float sigmoid(float v){if(v>=0)return 1/(1+expf(-v));float e=expf(v);return e/(1+e);}
__global__ static void predict_cell(CellDevice *d,int n){
    int b=blockIdx.x*blockDim.x+threadIdx.x;if(b>=n)return;
    float z=d->model.weight[40];
    for(int j=0;j<8;j++){
        float a=d->model.weight[j*4+3];for(int i=0;i<3;i++)a=fmaf(d->x[b*3+i],d->model.weight[j*4+i],a);
        if(!isfinite(a)){atomicExch(&d->failed,1);return;}z=fmaf(sigmoid(a),d->model.weight[32+j],z);
    }
    if(!isfinite(z)){atomicExch(&d->failed,1);return;}d->prediction[b]=sigmoid(z);
}
__global__ static void gradients(CellDevice *d,int n){
    int b=blockIdx.x*blockDim.x+threadIdx.x;if(b>=n||atomicAdd(&d->failed,0))return;
    float h[8],z=d->model.weight[40];
    for(int j=0;j<8;j++){
        float a=d->model.weight[j*4+3];for(int i=0;i<3;i++)a=fmaf(d->x[b*3+i],d->model.weight[j*4+i],a);
        if(!isfinite(a)){atomicExch(&d->failed,1);return;}h[j]=sigmoid(a);z=fmaf(h[j],d->model.weight[32+j],z);
    }
    if(!isfinite(z)){atomicExch(&d->failed,1);return;}
    float p=sigmoid(z),delta=p-d->y[b];
    d->loss[b]=fmaxf(z,0)-z*d->y[b]+log1pf(expf(-fabsf(z)));
    d->gradient[b*41+40]=delta;
    for(int j=0;j<8;j++){
        d->gradient[b*41+32+j]=delta*h[j];float dh=delta*d->model.weight[32+j]*h[j]*(1-h[j]);
        for(int i=0;i<3;i++)d->gradient[b*41+j*4+i]=dh*d->x[b*3+i];d->gradient[b*41+j*4+3]=dh;
    }
}
__global__ static void update_cell(CellDevice *d,int n,float lr){
    int i=threadIdx.x;if(i>=41||atomicAdd(&d->failed,0))return;
    float gradient=0;for(int b=0;b<n;b++)gradient+=d->gradient[b*41+i];
    d->model.weight[i]-=lr*(gradient/n);
    if(!isfinite(d->model.weight[i]))atomicExch(&d->failed,1);
    if(i==0){double loss=0;for(int b=0;b<n;b++)loss+=d->loss[b];d->loss[0]=(float)(loss/n);if(!isfinite(loss))atomicExch(&d->failed,1);}
}
extern "C" void cell_gpu_close(CellGpu *g){
    if(!g)return;
    int failed=checked(g,hipSetDevice(g->device),"close device");
    if(g->stream)failed|=checked(g,hipStreamSynchronize(g->stream),"close completion");
    if(g->data)failed|=checked(g,hipFree(g->data),"release device state");
    if(g->stream)failed|=checked(g,hipStreamDestroy(g->stream),"release stream");
    if(failed)std::fprintf(stderr,"CORE_CELL_CLOSE_ERROR %s\n",g->error);
    cce_amdmath_close(g->math);std::free(g);
}
extern "C" CellGpu *cell_gpu_open(const CnetCoreCell *initial,int device){
    if(cnet_core_cell_validate(initial))return nullptr;
    CellGpu *g=(CellGpu*)std::calloc(1,sizeof *g);if(!g)return nullptr;
    g->math=cce_amdmath_open_device(device,nullptr,0);if(!g->math){std::free(g);return nullptr;}
    if(checked(g,hipGetDevice(&g->device),"get device")||checked(g,hipStreamCreateWithFlags(&g->stream,hipStreamNonBlocking),"private stream")||
       checked(g,hipMalloc(&g->data,sizeof(CellDevice)),"device state")||
       checked(g,hipMemcpy(&g->data->model,initial,sizeof *initial,hipMemcpyHostToDevice),"initial model")){cell_gpu_close(g);return nullptr;}
    return g;
}
extern "C" const char *cell_gpu_error(const CellGpu *g){return g?g->error:"null cell trainer";}
extern "C" int cell_gpu_snapshot(CellGpu *g,CnetCoreCell *out){
    if(!out||ready(g))return -1;
    if(checked(g,hipMemcpyAsync(out,&g->data->model,sizeof *out,hipMemcpyDeviceToHost,g->stream),"snapshot")||
       checked(g,hipStreamSynchronize(g->stream),"snapshot completion"))return drain(g);
    return 0;
}
extern "C" int cell_gpu_predict(CellGpu *g,const float *x,int n,float *scores){
    if(!x||!scores||n<1||n>2048||ready(g))return -1;
    for(int i=0;i<n*3;i++)if(!std::isfinite(x[i])||x[i]<0||x[i]>1)return -1;
    if(checked(g,hipMemsetAsync(&g->data->failed,0,sizeof(int),g->stream),"clear prediction status")||
       checked(g,hipMemcpyAsync(g->data->x,x,n*3*sizeof(float),hipMemcpyHostToDevice,g->stream),"prediction features"))return drain(g);
    predict_cell<<<(n+127)/128,128,0,g->stream>>>(g->data,n);
    if(checked(g,hipGetLastError(),"prediction launch"))return drain(g);
    int failed;
    if(checked(g,hipMemcpyAsync(&failed,&g->data->failed,sizeof failed,hipMemcpyDeviceToHost,g->stream),"prediction status")||
       checked(g,hipMemcpyAsync(scores,g->data->prediction,n*sizeof(float),hipMemcpyDeviceToHost,g->stream),"predictions")||
       checked(g,hipStreamSynchronize(g->stream),"prediction completion"))return drain(g);
    if(failed){g->poisoned=1;std::snprintf(g->error,sizeof g->error,"nonfinite cell prediction");return -1;}
    return 0;
}
extern "C" int cell_gpu_fit(CellGpu *g,const float *x,const float *y,int n,int epochs,float lr,float *loss){
    if(!x||!y||!loss||n<1||n>2048||epochs<1||epochs>4096||!std::isfinite(lr)||lr<=0||ready(g))return -1;
    for(int i=0;i<n*3;i++)if(!std::isfinite(x[i])||x[i]<0||x[i]>1)return -1;
    for(int i=0;i<n;i++)if(!std::isfinite(y[i])||y[i]<0||y[i]>1)return -1;
    if(checked(g,hipMemsetAsync(&g->data->failed,0,sizeof(int),g->stream),"clear status")||
       checked(g,hipMemcpyAsync(g->data->x,x,n*3*sizeof(float),hipMemcpyHostToDevice,g->stream),"features")||
       checked(g,hipMemcpyAsync(g->data->y,y,n*sizeof(float),hipMemcpyHostToDevice,g->stream),"labels"))return drain(g);
    for(int step=0;step<epochs;step++){
        gradients<<<(n+127)/128,128,0,g->stream>>>(g->data,n);
        if(checked(g,hipGetLastError(),"gradient launch"))return drain(g);
        update_cell<<<1,64,0,g->stream>>>(g->data,n,lr);
        if(checked(g,hipGetLastError(),"update launch"))return drain(g);
    }
    int failed;float value;
    if(checked(g,hipMemcpyAsync(&failed,&g->data->failed,sizeof failed,hipMemcpyDeviceToHost,g->stream),"status")||
       checked(g,hipMemcpyAsync(&value,g->data->loss,sizeof value,hipMemcpyDeviceToHost,g->stream),"loss")||
       checked(g,hipStreamSynchronize(g->stream),"fit completion"))return drain(g);
    if(failed||!std::isfinite(value)){g->poisoned=1;std::snprintf(g->error,sizeof g->error,"nonfinite cell update");return -1;}
    *loss=value;return 0;
}
#ifdef CONTROLLER_TESTING
extern "C" int cell_gpu_fail_for_test(CellGpu *g){if(ready(g))return -1;checked(g,hipErrorInvalidValue,"injected runtime failure");return 0;}
#endif
