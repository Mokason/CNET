/* proj_qat_gpu — milestone 3: dual-GPU async dispatch of the embarrassingly-
 * parallel per-projection jobs across the two R9700s.
 *
 * Decomposed per-projection quantization is task-parallel: each projection's
 * GEMM work is independent, so N jobs split across 2 devices with concurrent
 * queues should run ~2× faster than one device. Each job is a forward GEMM
 * C = A·W (cce_clgemm's shape: small activations × a resident weight matrix,
 * uploaded once and cached device-side). We pin one handle per device with
 * cce_clgemm_open_device (the oracle-pool pattern — concurrent lanes never
 * share a queue), verify GPU==CPU, and measure single- vs dual-GPU throughput.
 *
 * Needs the OpenCL driver + >=1 discrete GPU; with none it reports and passes
 * (CPU is always the fallback). Build: make proj_qat_gpu
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include "../include/cce/cce_clgemm.h"

static int g_pass=0,g_fail=0;
#define CHECK(c,msg) do{ if(c){g_pass++;printf("  ok   %s\n",(msg));} else {g_fail++;printf("  FAIL: %s\n",(msg));} }while(0)
static unsigned long long RNG=0x51ED270B2E1250A9ULL;
static float frnd(void){ RNG=RNG*6364136223846793005ULL+1442695040888963407ULL; return (float)(((RNG>>40)&0xFFFF)/65535.0-0.5)*0.1f; }

static void cpu_gemm(const float* A,int T,int K,const float* W,int N,float* C){
    #pragma omp parallel for schedule(static)
    for(int t=0;t<T;t++){ const float* a=A+(size_t)t*K; float* c=C+(size_t)t*N;
        for(int o=0;o<N;o++) c[o]=0.0f;
        for(int i=0;i<K;i++){ float av=a[i]; const float* wi=W+(size_t)i*N;
            for(int o=0;o<N;o++) c[o]+=av*wi[o]; } }
}
static double relerr(const float* A,const float* B,size_t n){ double num=0,den=0;
    for(size_t k=0;k<n;k++){ double d=(double)A[k]-(double)B[k]; num+=d*d; den+=(double)B[k]*(double)B[k]; }
    return den>0?sqrt(num/den):0.0; }

int main(int argc,char** argv){
    int in=(argc>1)?atoi(argv[1]):4096, out=(argc>2)?atoi(argv[2]):4096;
    /* cce_clgemm targets its inference shape: few activation rows (T<=8) against a
       large resident weight. Each job = a per-projection forward GEMM. */
    int T=(argc>3)?atoi(argv[3]):8, N=(argc>4)?atoi(argv[4]):32, reps=(argc>5)?atoi(argv[5]):16;
    printf("proj_qat_gpu: %d independent projection GEMMs  A[%d x %d] . W[%d x %d]  reps=%d\n", N,T,in,in,out,reps);

    float* A=malloc((size_t)T*in*sizeof(float)); for(size_t k=0;k<(size_t)T*in;k++) A[k]=frnd();
    float** W=malloc(N*sizeof(float*)); for(int j=0;j<N;j++){ W[j]=malloc((size_t)in*out*sizeof(float));
        for(size_t k=0;k<(size_t)in*out;k++) W[j][k]=frnd(); }
    float** Ccpu=malloc(N*sizeof(float*)); float** Cg=malloc(N*sizeof(float*));
    for(int j=0;j<N;j++){ Ccpu[j]=malloc((size_t)T*out*sizeof(float)); Cg[j]=malloc((size_t)T*out*sizeof(float)); }

    /* CPU reference + CPU throughput baseline */
    double tc=omp_get_wtime();
    for(int j=0;j<N;j++) cpu_gemm(A,T,in,W[j],out,Ccpu[j]);
    double wall_cpu1=omp_get_wtime()-tc;   /* one pass, all threads */

    /* open the GPU pool just to report device count/name */
    char name[256]={0};
    cce_clgemm* probe=cce_clgemm_open(NULL,name,sizeof name);
    if(!probe){ printf("  no OpenCL GPU available — CPU is the fallback, nothing to accelerate\n");
        printf("\nproj_qat_gpu: %d passed, %d failed\n", g_pass, g_fail); return 0; }
    int ndev=(int)cce_clgemm_device_count(probe);
    printf("  GPU pool: %d device(s)  [%s]\n", ndev, name);
    cce_clgemm_close(probe);

    /* ---- single-GPU: device 0 runs all N jobs ---- */
    cce_clgemm* h0=cce_clgemm_open_device(NULL,0,NULL,0);
    if(!h0){ printf("FAIL: open device 0\n"); return 1; }
    int rc=0; for(int j=0;j<N;j++) if(cce_clgemm_matmul(h0,A,T,in,W[j],NULL,out,Cg[j])!=0) rc=1;   /* warmup: upload+cache W */
    if(rc){ printf("  FAIL: cce_clgemm_matmul rejected the shape (T=%d, in=%d, out=%d) — it targets T<=8 rows\n",T,in,out);
        g_fail++; cce_clgemm_close(h0); printf("\nproj_qat_gpu: %d passed, %d failed\n",g_pass,g_fail); return 1; }
    double maxrel=0; for(int j=0;j<N;j++){ double r=relerr(Cg[j],Ccpu[j],(size_t)T*out); if(r>maxrel)maxrel=r; }
    CHECK(maxrel<2e-3, "GPU GEMM matches CPU (max relerr < 2e-3)");
    printf("  GPU-vs-CPU max relerr = %.2e\n", maxrel);
    double ts=omp_get_wtime();
    for(int r=0;r<reps;r++) for(int j=0;j<N;j++) cce_clgemm_matmul(h0,A,T,in,W[j],NULL,out,Cg[j]);
    double wall_single=omp_get_wtime()-ts;

    printf("\n  === throughput: %d jobs x %d reps ===\n", N, reps);
    printf("    CPU (1 pass, OpenMP)   : %.3fs\n", wall_cpu1);
    printf("    single-GPU (device 0)  : %.3fs  (%.1f jobs/s)\n", wall_single, (double)N*reps/wall_single);

    /* ---- dual-GPU async: split N jobs across device 0 and 1, concurrent queues ---- */
    if(ndev>=2){
        cce_clgemm* h1=cce_clgemm_open_device(NULL,1,NULL,0);
        if(!h1){ printf("  device 1 open failed — single-GPU only\n"); }
        else {
            /* warmup device 1's share (odd j) so weights are resident before timing */
            for(int j=1;j<N;j+=2) cce_clgemm_matmul(h1,A,T,in,W[j],NULL,out,Cg[j]);
            double td=omp_get_wtime();
            #pragma omp parallel num_threads(2)
            {
                int tid=omp_get_thread_num(); cce_clgemm* h= tid? h1:h0;
                for(int r=0;r<reps;r++) for(int j=tid;j<N;j+=2)
                    cce_clgemm_matmul(h,A,T,in,W[j],NULL,out,Cg[j]);
            }
            double wall_dual=omp_get_wtime()-td;
            /* re-verify a couple outputs still match (both devices correct) */
            double mr=0; for(int j=0;j<N;j++){ double r=relerr(Cg[j],Ccpu[j],(size_t)T*out); if(r>mr)mr=r; }
            double speedup=wall_dual>0?wall_single/wall_dual:0;
            printf("    dual-GPU async (0+1)   : %.3fs  (%.1f jobs/s)  speedup %.2fx over single-GPU\n",
                   wall_dual, (double)N*reps/wall_dual, speedup);
            CHECK(mr<2e-3, "dual-GPU outputs still match CPU (both devices correct)");
            CHECK(speedup>1.5, "dual-GPU async >= 1.5x single-GPU (the two R9700s parallelize the jobs)");
            cce_clgemm_close(h1);
        }
    } else {
        printf("    (only %d discrete GPU — dual-GPU dispatch needs 2; single-GPU path validated)\n", ndev);
    }
    cce_clgemm_close(h0);

    for(int j=0;j<N;j++){ free(W[j]); free(Ccpu[j]); free(Cg[j]); }
    free(A);free(W);free(Ccpu);free(Cg);
    printf("\nproj_qat_gpu: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail?1:0;
}
