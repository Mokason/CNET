#include "cce/cce_amdmath.h"
#include <hip/hip_runtime.h>
#include <hip/hip_fp16.h>
#include <hip/hip_bfloat16.h>
#include <rocblas/rocblas.h>
#include <cblas.h>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
static double seconds(){return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static float difference(const std::vector<float>&a,const std::vector<float>&b){
    assert(a.size()==b.size());float error=0;
    for(size_t i=0;i<a.size();i++){assert(std::isfinite(a[i])&&std::isfinite(b[i]));error=std::max(error,std::fabs(a[i]-b[i]));}return error;
}
template<class T> static void upload(T **dst,const std::vector<float>&src,hipStream_t stream){
    std::vector<T> converted(src.size());for(size_t i=0;i<src.size();i++)converted[i]=T(src[i]);
    assert(hipMalloc(dst,converted.size()*sizeof(T))==hipSuccess);
    assert(hipMemcpyAsync(*dst,converted.data(),converted.size()*sizeof(T),hipMemcpyHostToDevice,stream)==hipSuccess);
    assert(hipStreamSynchronize(stream)==hipSuccess);
}
int main(int argc,char **argv){
    assert(argc==2);char *end;long device=std::strtol(argv[1],&end,10);assert(!*end&&device>=0&&device<2);
    cce_amdmath *h=cce_amdmath_open_device((int)device,nullptr,0);assert(h);
    hipStream_t stream;assert(hipStreamCreateWithFlags(&stream,hipStreamNonBlocking)==hipSuccess);
    rocblas_handle blas;assert(rocblas_create_handle(&blas)==rocblas_status_success);assert(rocblas_set_stream(blas,stream)==rocblas_status_success);
    const int shapes[][3]={{1,65,16},{32,209,64},{128,209,64},{512,209,64},{512,512,512},{2048,512,512}};
    for(const auto &shape:shapes){
        int n=shape[0],in=shape[1],out=shape[2];
        std::vector<float>x(n*in),w(out*in),y(n*out),scalar(n*out),cpu(n*out),dy(n*out),updated(out*in),actual(out*in);
        uint32_t rng=43;
        auto value=[&](){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return (float)(int(rng%20001)-10000)/20000;};
        for(float &v:x)v=value();for(float &v:w)v=value();for(float &v:dy)v=value();
        double begin=seconds();
        for(int b=0;b<n;b++)for(int o=0;o<out;o++){float sum=0;for(int i=0;i<in;i++)sum+=x[b*in+i]*w[o*in+i];scalar[b*out+o]=sum;}
        double scalar_seconds=seconds()-begin,cpu_seconds[2];
        for(int t=0;t<2;t++){
            openblas_set_num_threads(t?4:1);
            cblas_sgemm(CblasRowMajor,CblasNoTrans,CblasTrans,n,out,in,1,x.data(),in,w.data(),in,0,cpu.data(),out);
            begin=seconds();for(int repeat=0;repeat<20;repeat++)cblas_sgemm(CblasRowMajor,CblasNoTrans,CblasTrans,n,out,in,1,x.data(),in,w.data(),in,0,cpu.data(),out);
            cpu_seconds[t]=(seconds()-begin)/20;assert(difference(scalar,cpu)<1e-4f);
        }
        openblas_set_num_threads(1);updated=w;
        cblas_sgemm(CblasRowMajor,CblasTrans,CblasNoTrans,out,in,n,-.02f,dy.data(),out,x.data(),in,1,updated.data(),in);
        float *dx,*dw,*dg,*output;upload(&dx,x,stream);upload(&dw,w,stream);upload(&dg,dy,stream);
        assert(hipMalloc(&output,y.size()*sizeof(float))==hipSuccess);
        for(int mode=0;mode<4;mode++){
            const char *name=mode==0?"naive_fp32":mode==1?"rocblas_fp32":mode==2?"rocblas_fp16":"rocblas_bf16";
            rocblas_datatype type=mode<2?rocblas_datatype_f32_r:mode==2?rocblas_datatype_f16_r:rocblas_datatype_bf16_r;
            void *qx=dx,*qw=dw,*qg=dg;
            if(mode==2){__half *a,*b,*c;upload(&a,x,stream);upload(&b,w,stream);upload(&c,dy,stream);qx=a;qw=b;qg=c;}
            if(mode==3){hip_bfloat16 *a,*b,*c;upload(&a,x,stream);upload(&b,w,stream);upload(&c,dy,stream);qx=a;qw=b;qg=c;}
            assert(hipMemcpyAsync(dw,w.data(),w.size()*sizeof(float),hipMemcpyHostToDevice,stream)==hipSuccess);
            float one=1,zero=0,rate=-.02f;
            auto linear=[&](){
                if(mode==0)return cce_amdmath_linear_f32_stream(h,dx,dw,output,1,n,in,out,stream)==0;
                return rocblas_gemm_ex(blas,rocblas_operation_transpose,rocblas_operation_none,out,n,in,&one,qw,type,in,qx,type,in,&zero,output,rocblas_datatype_f32_r,out,output,rocblas_datatype_f32_r,out,rocblas_datatype_f32_r,rocblas_gemm_algo_standard,0,0)==rocblas_status_success;
            };
            bool supported=linear();assert(hipStreamSynchronize(stream)==hipSuccess);
            if(supported){
                begin=seconds();for(int repeat=0;repeat<100;repeat++)assert(linear());assert(hipStreamSynchronize(stream)==hipSuccess);
                double gpu_seconds=(seconds()-begin)/100;
                assert(hipMemcpy(y.data(),output,y.size()*sizeof(float),hipMemcpyDeviceToHost)==hipSuccess);float error=difference(scalar,y);
                if(mode==0)assert(!cce_amdmath_sgd_f32_stream(h,dw,dx,dg,1,n,in,out,.02f,stream));
                else assert(rocblas_gemm_ex(blas,rocblas_operation_none,rocblas_operation_transpose,in,out,n,&rate,qx,type,in,qg,type,out,&one,dw,rocblas_datatype_f32_r,in,dw,rocblas_datatype_f32_r,in,rocblas_datatype_f32_r,rocblas_gemm_algo_standard,0,0)==rocblas_status_success);
                assert(hipStreamSynchronize(stream)==hipSuccess);assert(hipMemcpy(actual.data(),dw,actual.size()*sizeof(float),hipMemcpyDeviceToHost)==hipSuccess);
                float update_error=difference(updated,actual);
                if(mode<2)assert(error<1e-4f&&update_error<1e-4f);
                std::printf("{\"kind\":\"matrix_benchmark\",\"device\":%ld,\"backend\":\"%s\",\"n\":%d,\"in\":%d,\"out\":%d,\"scalar_seconds\":%.9f,\"openblas_t1_seconds\":%.9f,\"openblas_t4_seconds\":%.9f,\"gpu_seconds\":%.9f,\"forward_abs\":%.9g,\"master_update_abs\":%.9g,\"numeric_1e4_pass\":%s,\"conversion_in_timing\":false,\"shared_machine\":true}\n",device,name,n,in,out,scalar_seconds,cpu_seconds[0],cpu_seconds[1],gpu_seconds,error,update_error,error<1e-4f&&update_error<1e-4f?"true":"false");
            }else std::printf("{\"kind\":\"matrix_unsupported\",\"device\":%ld,\"backend\":\"%s\",\"n\":%d,\"in\":%d,\"out\":%d}\n",device,name,n,in,out);
            if(mode>=2){assert(hipFree(qx)==hipSuccess);assert(hipFree(qw)==hipSuccess);assert(hipFree(qg)==hipSuccess);}
        }
        assert(hipFree(dx)==hipSuccess);assert(hipFree(dw)==hipSuccess);assert(hipFree(dg)==hipSuccess);assert(hipFree(output)==hipSuccess);
    }
    assert(rocblas_destroy_handle(blas)==rocblas_status_success);assert(hipStreamDestroy(stream)==hipSuccess);cce_amdmath_close(h);
}
