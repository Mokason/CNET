/* proj_qat_stack — milestone 4 core: END-TO-END across MANY projections and MANY
 * layers. M1 (hermetic) and M2 (real gemma) proved per-projection reconstruction
 * for ONE projection. The open questions this answers:
 *   (a) does quantizing EVERY projection of a deep stack compound into collapse,
 *       or does data-aware keep the end-to-end output close to FP?
 *   (b) does SEQUENTIAL calibration (reconstruct layer k on the activations that
 *       the ALREADY-QUANTIZED layers 0..k-1 produce, so k compensates for
 *       upstream error) beat INDEPENDENT (each layer on FP activations)?
 *
 * A correct L-layer SwiGLU MLP stack (RMSNorm -> gate/up -> SiLU(g)*u -> down ->
 * residual), the real modern-transformer MLP block. Correlated inputs (the real
 * activation regime). Metric: held-out relative error of the final residual
 * stream vs the FP stack, for naive / data-aware-independent / data-aware-seq.
 *
 * Hermetic (synthetic weights). The reconstruction is the SAME Σ-based OBQ solver
 * proven on real gemma weights in proj_qat_gemma. Build: make proj_qat_stack
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_pass=0,g_fail=0;
#define CHECK(c,msg) do{ if(c){g_pass++;printf("  ok   %s\n",(msg));} else {g_fail++;printf("  FAIL: %s\n",(msg));} }while(0)
static unsigned long long RNG=0xDEADBEEF12345678ULL;
static double urand(void){ RNG=RNG*6364136223846793005ULL+1442695040888963407ULL; return (double)((RNG>>11)&((1ULL<<53)-1))/(double)(1ULL<<53); }
static double nrand(void){ double u1=urand(),u2=urand(); if(u1<1e-12)u1=1e-12; return sqrt(-2.0*log(u1))*cos(6.283185307179586*u2); }

static void ternize(const float* W,int in,int out,float* Weff){
    for(int o=0;o<out;o++){ double s=0; for(int i=0;i<in;i++) s+=fabs((double)W[(size_t)i*out+o]);
        float g=(float)(s/(in>0?in:1));
        for(int i=0;i<in;i++){ float w=W[(size_t)i*out+o],r=0.0f;
            if(g>0){ r=roundf(w/g); if(r>1)r=1; if(r<-1)r=-1; } Weff[(size_t)i*out+o]=g*r; } }
}
static void matmul(const float* X,const float* Wm,int n,int in,int out,float* Y){
    #pragma omp parallel for schedule(static)
    for(int r=0;r<n;r++){ const float* xr=X+(size_t)r*in; float* yr=Y+(size_t)r*out;
        for(int o=0;o<out;o++) yr[o]=0.0f;
        for(int i=0;i<in;i++){ float xv=xr[i]; const float* wi=Wm+(size_t)i*out;
            for(int o=0;o<out;o++) yr[o]+=xv*wi[o]; } }
}
static double relerr(const float* A,const float* B,size_t n){ double num=0,den=0;
    for(size_t k=0;k<n;k++){ double d=(double)A[k]-(double)B[k]; num+=d*d; den+=(double)B[k]*(double)B[k]; }
    return den>0?sqrt(num/den):0.0; }
/* Σ = XᵀX (in×in) over n rows, + GPTQ 1% mean-diag damping */
static void build_sigma(const float* X,int n,int in,double* Sig){
    #pragma omp parallel for schedule(static)
    for(int i=0;i<in;i++) for(int j=0;j<in;j++){ double s=0;
        for(int r=0;r<n;r++){ s+=(double)X[(size_t)r*in+i]*(double)X[(size_t)r*in+j]; } Sig[(size_t)i*in+j]=s; }
    double md=0; for(int i=0;i<in;i++) md+=Sig[(size_t)i*in+i]; md/=in;
    for(int i=0;i<in;i++) Sig[(size_t)i*in+i]+=0.01*md;
}
static void reconstruct(const float* W,int in,int out,const double* Sig,float* Weff){
    #pragma omp parallel
    { double* w=malloc(in*sizeof(double)),*q=malloc(in*sizeof(double)),*Sw=malloc(in*sizeof(double)),*g=malloc(in*sizeof(double)),*rr=malloc(in*sizeof(double));
      #pragma omp for schedule(dynamic,8)
      for(int o=0;o<out;o++){
        for(int i=0;i<in;i++) w[i]=(double)W[(size_t)i*out+o];
        for(int i=0;i<in;i++){ double s=0; const double* Si=Sig+(size_t)i*in; for(int j=0;j<in;j++){ s+=Si[j]*w[j]; } Sw[i]=s; }
        double gam=0; for(int i=0;i<in;i++) gam+=fabs(w[i]); gam/=(in?in:1);
        for(int i=0;i<in;i++){ double qi=gam>0?round(w[i]/gam):0; if(qi>1)qi=1; if(qi<-1)qi=-1; q[i]=qi; }
        for(int round=0;round<4;round++){
            double num=0,den=0;
            for(int i=0;i<in;i++){ double sq=0; const double* Si=Sig+(size_t)i*in; for(int j=0;j<in;j++){ sq+=Si[j]*q[j]; } num+=q[i]*Sw[i]; den+=q[i]*sq; }
            if(den>1e-12){ double gn=num/den; if(gn>0) gam=gn; }
            for(int i=0;i<in;i++) rr[i]=gam*q[i]-w[i];
            for(int i=0;i<in;i++){ double s=0; const double* Si=Sig+(size_t)i*in; for(int j=0;j<in;j++){ s+=Si[j]*rr[j]; } g[i]=s; }
            for(int sweep=0;sweep<3;sweep++) for(int i=0;i<in;i++){
                double Sii=Sig[(size_t)i*in+i],bdE=0,bd=0,bc=q[i];
                for(int cc=-1;cc<=1;cc++){ double delta=gam*((double)cc-q[i]); if(delta==0)continue;
                    double dE=2*delta*g[i]+delta*delta*Sii; if(dE<bdE){bdE=dE;bd=delta;bc=cc;} }
                if(bd!=0){ rr[i]+=bd; q[i]=bc; const double* Si=Sig+(size_t)i*in; for(int j=0;j<in;j++) g[j]+=bd*Si[j]; }
            }
        }
        for(int i=0;i<in;i++) Weff[(size_t)i*out+o]=(float)(gam*q[i]);
      }
      free(w);free(q);free(Sw);free(g);free(rr);
    }
}
static void rmsnorm(const float* X,int n,int d,float* Y){
    #pragma omp parallel for schedule(static)
    for(int r=0;r<n;r++){ const float* xr=X+(size_t)r*d; float* yr=Y+(size_t)r*d;
        double ss=0; for(int i=0;i<d;i++) ss+=(double)xr[i]*xr[i]; float inv=(float)(1.0/sqrt(ss/d+1e-6));
        for(int i=0;i<d;i++) yr[i]=xr[i]*inv; }
}
static void silu_mul(const float* G,const float* U,int n,int hid,float* A){
    #pragma omp parallel for schedule(static)
    for(size_t k=0;k<(size_t)n*hid;k++){ float g=G[k]; A[k]=(g/(1.0f+expf(-g)))*U[k]; }
}

/* One forward of the SwiGLU stack. gate[l],up[l] : [d][hid]; down[l] : [hid][d].
   If cap_nrm/cap_a != NULL, capture per-layer inputs: cap_nrm[l] = RMSNorm input
   to gate/up ([n][d]); cap_a[l] = SiLU(g)*u input to down ([n][hid]). */
static void forward(const float* H0,int n,int d,int hid,int L,
                    float** gate,float** up,float** down,
                    float** cap_nrm,float** cap_a,float* Hout){
    float* h=malloc((size_t)n*d*sizeof(float)); memcpy(h,H0,(size_t)n*d*sizeof(float));
    float* nrm=malloc((size_t)n*d*sizeof(float));
    float* G=malloc((size_t)n*hid*sizeof(float)),*U=malloc((size_t)n*hid*sizeof(float)),*A=malloc((size_t)n*hid*sizeof(float));
    float* dn=malloc((size_t)n*d*sizeof(float));
    for(int l=0;l<L;l++){
        rmsnorm(h,n,d,nrm);
        if(cap_nrm) memcpy(cap_nrm[l],nrm,(size_t)n*d*sizeof(float));
        matmul(nrm,gate[l],n,d,hid,G); matmul(nrm,up[l],n,d,hid,U);
        silu_mul(G,U,n,hid,A);
        if(cap_a) memcpy(cap_a[l],A,(size_t)n*hid*sizeof(float));
        matmul(A,down[l],n,hid,d,dn);
        for(size_t k=0;k<(size_t)n*d;k++) h[k]+=dn[k];
    }
    memcpy(Hout,h,(size_t)n*d*sizeof(float));
    free(h);free(nrm);free(G);free(U);free(A);free(dn);
}

int main(int argc,char** argv){
    int L=4,d=64,hid=256, n_tr=(argc>1)?atoi(argv[1]):2048, n_ho=(argc>2)?atoi(argv[2]):512;
    printf("proj_qat_stack: SwiGLU MLP stack L=%d d=%d hid=%d  calib=%d held-out=%d\n",L,d,hid,n_tr,n_ho);

    /* FP weights */
    float **gate=malloc(L*sizeof(float*)),**up=malloc(L*sizeof(float*)),**down=malloc(L*sizeof(float*));
    for(int l=0;l<L;l++){ gate[l]=malloc((size_t)d*hid*sizeof(float)); up[l]=malloc((size_t)d*hid*sizeof(float)); down[l]=malloc((size_t)hid*d*sizeof(float));
        for(size_t k=0;k<(size_t)d*hid;k++){ gate[l][k]=(float)(nrand()*0.10); up[l][k]=(float)(nrand()*0.10); }
        for(size_t k=0;k<(size_t)hid*d;k++) down[l][k]=(float)(nrand()*0.10); }

    /* correlated inputs H0 = Z@Lmix */
    float* Lmix=malloc((size_t)d*d*sizeof(float)); for(size_t k=0;k<(size_t)d*d;k++) Lmix[k]=(float)(nrand()/sqrt((double)d));
    int nt=n_tr+n_ho; float* Z=malloc((size_t)nt*d*sizeof(float)); for(size_t k=0;k<(size_t)nt*d;k++) Z[k]=(float)nrand();
    float* H0=malloc((size_t)nt*d*sizeof(float)); matmul(Z,Lmix,nt,d,d,H0);
    float* H0tr=H0; float* H0ho=H0+(size_t)n_tr*d;

    /* FP reference outputs + captured calibration activations (per layer) */
    float **cap_nrm=malloc(L*sizeof(float*)),**cap_a=malloc(L*sizeof(float*));
    for(int l=0;l<L;l++){ cap_nrm[l]=malloc((size_t)n_tr*d*sizeof(float)); cap_a[l]=malloc((size_t)n_tr*hid*sizeof(float)); }
    float* Hfp_ho=malloc((size_t)n_ho*d*sizeof(float));
    float* scratch_tr=malloc((size_t)n_tr*d*sizeof(float));
    forward(H0tr,n_tr,d,hid,L,gate,up,down,cap_nrm,cap_a,scratch_tr);  /* capture FP activations */
    forward(H0ho,n_ho,d,hid,L,gate,up,down,NULL,NULL,Hfp_ho);          /* FP held-out output */

    /* alloc quantized weight banks */
    float **qg=malloc(L*sizeof(float*)),**qu=malloc(L*sizeof(float*)),**qd=malloc(L*sizeof(float*));
    for(int l=0;l<L;l++){ qg[l]=malloc((size_t)d*hid*sizeof(float)); qu[l]=malloc((size_t)d*hid*sizeof(float)); qd[l]=malloc((size_t)hid*d*sizeof(float)); }
    float* Hq_ho=malloc((size_t)n_ho*d*sizeof(float));
    double* Sig_d=malloc((size_t)d*d*sizeof(double)); double* Sig_h=malloc((size_t)hid*hid*sizeof(double));

    /* --- naive post-hoc on every projection --- */
    for(int l=0;l<L;l++){ ternize(gate[l],d,hid,qg[l]); ternize(up[l],d,hid,qu[l]); ternize(down[l],hid,d,qd[l]); }
    forward(H0ho,n_ho,d,hid,L,qg,qu,qd,NULL,NULL,Hq_ho);
    double e_naive=relerr(Hq_ho,Hfp_ho,(size_t)n_ho*d);

    /* --- data-aware INDEPENDENT: each projection reconstructed on FP activations --- */
    for(int l=0;l<L;l++){
        build_sigma(cap_nrm[l],n_tr,d,Sig_d); reconstruct(gate[l],d,hid,Sig_d,qg[l]); reconstruct(up[l],d,hid,Sig_d,qu[l]);
        build_sigma(cap_a[l],n_tr,hid,Sig_h); reconstruct(down[l],hid,d,Sig_h,qd[l]);
    }
    forward(H0ho,n_ho,d,hid,L,qg,qu,qd,NULL,NULL,Hq_ho);
    double e_indep=relerr(Hq_ho,Hfp_ho,(size_t)n_ho*d);

    /* --- data-aware SEQUENTIAL: layer k calibrated on activations produced by
           the already-quantized layers 0..k-1 (compensates upstream error) --- */
    float **mg=malloc(L*sizeof(float*)),**mu=malloc(L*sizeof(float*)),**md=malloc(L*sizeof(float*));
    for(int l=0;l<L;l++){ mg[l]=gate[l]; mu[l]=up[l]; md[l]=down[l]; }  /* start all FP */
    for(int l=0;l<L;l++){
        /* capture layer-l inputs with layers 0..l-1 already quantized (mg/mu/md hold quantized for <l, FP for >=l) */
        forward(H0tr,n_tr,d,hid,L,mg,mu,md,cap_nrm,cap_a,scratch_tr);
        build_sigma(cap_nrm[l],n_tr,d,Sig_d); reconstruct(gate[l],d,hid,Sig_d,qg[l]); reconstruct(up[l],d,hid,Sig_d,qu[l]);
        build_sigma(cap_a[l],n_tr,hid,Sig_h); reconstruct(down[l],hid,d,Sig_h,qd[l]);
        mg[l]=qg[l]; mu[l]=qu[l]; md[l]=qd[l];   /* commit layer l as quantized for subsequent captures */
    }
    forward(H0ho,n_ho,d,hid,L,qg,qu,qd,NULL,NULL,Hq_ho);
    double e_seq=relerr(Hq_ho,Hfp_ho,(size_t)n_ho*d);

    printf("\n  === END-TO-END held-out output relerr vs FP (%d projections over %d layers) ===\n", 3*L, L);
    printf("    naive post-hoc         : %.4f\n", e_naive);
    printf("    data-aware independent : %.4f   (%.0f%% lower than naive)\n", e_indep, e_naive>0?100*(e_naive-e_indep)/e_naive:0);
    printf("    data-aware sequential  : %.4f   (%.0f%% lower than naive; %.0f%% lower than independent)\n",
           e_seq, e_naive>0?100*(e_naive-e_seq)/e_naive:0, e_indep>0?100*(e_indep-e_seq)/e_indep:0);
    CHECK(e_indep < e_naive, "data-aware end-to-end beats naive across the whole stack");
    CHECK(e_seq <= e_indep + 1e-4, "sequential co-adaptation >= independent (compensates upstream error)");

    printf("\nproj_qat_stack: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail?1:0;
}
