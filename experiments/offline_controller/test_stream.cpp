#include "cce/cce_amdmath.h"
#include <hip/hip_runtime.h>
#include <cassert>
#include <cmath>
#include <climits>
#include <cstdio>
int main(){
    constexpr int N=17,In=19,Out=7,E=2;
    float x[E*N*In],w[E*Out*In],y[E*N*Out],gradient[E*N*Out],after[E*Out*In];
    for(int i=0;i<E*N*In;i++)x[i]=(i%13-6)*.01f;
    for(int i=0;i<E*Out*In;i++)w[i]=(i%17-8)*.01f;
    for(int i=0;i<E*N*Out;i++)gradient[i]=(i%11-5)*.01f;
    for(int device=0;device<2;device++){
        cce_amdmath *h=cce_amdmath_open_device(device,nullptr,0);assert(h);
        hipStream_t stream;assert(hipStreamCreateWithFlags(&stream,hipStreamNonBlocking)==hipSuccess);
        float *dx,*dw,*dy,*dg;
        assert(hipMalloc(&dx,sizeof x)==hipSuccess);assert(hipMalloc(&dw,sizeof w)==hipSuccess);
        assert(hipMalloc(&dy,sizeof y)==hipSuccess);assert(hipMalloc(&dg,sizeof gradient)==hipSuccess);
        assert(hipMemcpyAsync(dx,x,sizeof x,hipMemcpyHostToDevice,stream)==hipSuccess);
        assert(hipMemcpyAsync(dw,w,sizeof w,hipMemcpyHostToDevice,stream)==hipSuccess);
        assert(hipMemcpyAsync(dg,gradient,sizeof gradient,hipMemcpyHostToDevice,stream)==hipSuccess);
        assert(!cce_amdmath_linear_f32_stream(h,dx,dw,dy,E,N,In,Out,stream));
        assert(!cce_amdmath_sgd_f32_stream(h,dw,dx,dg,E,N,In,Out,.02f,stream));
        assert(hipMemcpyAsync(y,dy,sizeof y,hipMemcpyDeviceToHost,stream)==hipSuccess);
        assert(hipMemcpyAsync(after,dw,sizeof w,hipMemcpyDeviceToHost,stream)==hipSuccess);
        assert(hipStreamSynchronize(stream)==hipSuccess);
        for(int e=0;e<E;e++)for(int b=0;b<N;b++)for(int o=0;o<Out;o++){
            float v=0;for(int i=0;i<In;i++)v=std::fma(x[(e*N+b)*In+i],w[(e*Out+o)*In+i],v);
            assert(std::isfinite(y[(e*N+b)*Out+o])&&std::fabs(v-y[(e*N+b)*Out+o])<1e-6);
        }
        for(int e=0;e<E;e++)for(int o=0;o<Out;o++)for(int i=0;i<In;i++){
            float v=0;for(int b=0;b<N;b++)v=std::fma(x[(e*N+b)*In+i],gradient[(e*N+b)*Out+o],v);
            float expected=w[(e*Out+o)*In+i]-.02f*v;
            assert(std::isfinite(after[(e*Out+o)*In+i])&&std::fabs(expected-after[(e*Out+o)*In+i])<1e-6);
        }
        assert(cce_amdmath_linear_f32_stream(h,dx,dw,dy,1,0,In,Out,stream)!=0);
        assert(cce_amdmath_linear_f32_stream(h,dx,dw,dy,1,N,SIZE_MAX,Out,stream)!=0);
        assert(cce_amdmath_linear_f32_stream(h,dx,dw,dy,SIZE_MAX,N,In,Out,stream)!=0);
        assert(cce_amdmath_sgd_f32_stream(h,dw,dx,dg,1,N,In,0,.1f,stream)!=0);
        assert(cce_amdmath_sgd_f32_stream(h,dw,dx,dg,1,N,In,Out,NAN,stream)!=0);
        assert(cce_amdmath_sgd_f32_stream(h,dw,dx,dg,1,N,In,Out,-1,stream)!=0);
        constexpr int large_n=1048561;float *large;
        assert(hipMalloc(&large,large_n*sizeof(float))==hipSuccess);
        assert(hipMemsetAsync(large,0,large_n*sizeof(float),stream)==hipSuccess);
        assert(!cce_amdmath_sgd_f32_stream(h,dw,large,large,1,large_n,1,1,.1f,stream));
        assert(hipStreamSynchronize(stream)==hipSuccess);
        assert(!cce_amdmath_sgd_f32_dev(h,dw,large,large,1,large_n,1,1,-.1f));
        assert(hipFree(large)==hipSuccess);
        assert(hipFree(dx)==hipSuccess);assert(hipFree(dw)==hipSuccess);assert(hipFree(dy)==hipSuccess);assert(hipFree(dg)==hipSuccess);
        assert(hipStreamDestroy(stream)==hipSuccess);cce_amdmath_close(h);
        std::printf("CONTROLLER_STREAM_PASS device=%d experts=2 malformed_shapes_refused=4 invalid_lr_refused=2\n",device);
    }
}
