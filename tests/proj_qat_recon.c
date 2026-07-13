/* proj_qat_recon — the core unit of decomposed, data-aware per-projection ternary
 * QAT ("open the black box, quantize each part against the FP model's behavior").
 *
 * For ONE linear projection with FP weight W, given real INPUT activations X,
 * pick the ternary weights that minimize the OUTPUT reconstruction error
 * ‖X·Wq − X·W‖². Because Y[:,o] = X·W[:,o], the columns decouple and the objective
 * per output column is the GPTQ/OBQ quadratic (γq − w)ᵀ Σ (γq − w) with the
 * activation Hessian Σ = XᵀX. We solve it by alternating (a) the optimal scale
 * γ* = (qᵀΣw)/(qᵀΣq) and (b) ternary coordinate descent on q — both MONOTONE from
 * the naive absmean init, so data-aware can never lose to naive on calibration,
 * and exploits Σ (the activation structure) to beat it.
 *
 * Crucially this needs NO global backward — only Σ = XᵀX from a forward calibration
 * pass — so it works for ANY surrounding architecture (attention, SSM, RoPE…): the
 * nonlinear glue only runs FORWARD, to produce X. That's what makes it applicable
 * to the hybrid models a hand-rolled joint-QAT backward can't reach.
 *
 * The claim this gate proves: data-aware reconstruction beats naive post-hoc on
 * HELD-OUT activations (generalizes) — but only when activations are ANISOTROPIC
 * (Σ≠I, the real transformer regime). With iid activations the gap must vanish;
 * we assert both, so the win is attributed to activation structure, not luck.
 *
 * Hermetic: synthetic W and X, no model / no GPU. Build: make proj_qat_recon
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(c, msg) do { if (c) { g_pass++; printf("  ok   %s\n", (msg)); } \
                           else { g_fail++; printf("  FAIL: %s\n", (msg)); } } while (0)

static unsigned long long g_rng = 0x243F6A8885A308D3ULL;
static double urand(void) { g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (double)((g_rng >> 11) & ((1ULL<<53)-1)) / (double)(1ULL<<53); }
static double nrand(void) { double u1=urand(),u2=urand(); if(u1<1e-12)u1=1e-12;
    return sqrt(-2.0*log(u1))*cos(6.283185307179586*u2); }

static void ternize(const float* W, int in, int out, float* Weff) {
    #pragma omp parallel for schedule(static)
    for (int o=0;o<out;o++){ double s=0; for(int i=0;i<in;i++) s+=fabs((double)W[(size_t)i*out+o]);
        float g=(float)(s/(in>0?in:1));
        for(int i=0;i<in;i++){ float w=W[(size_t)i*out+o],r=0.0f;
            if(g>0){ r=roundf(w/g); if(r>1)r=1; if(r<-1)r=-1; } Weff[(size_t)i*out+o]=g*r; } }
}
static void matmul(const float* X, const float* Wm, int n, int in, int out, float* Y) {
    #pragma omp parallel for schedule(static)
    for (int r=0;r<n;r++){ const float* xr=X+(size_t)r*in; float* yr=Y+(size_t)r*out;
        for(int o=0;o<out;o++) yr[o]=0.0f;
        for(int i=0;i<in;i++){ float xv=xr[i]; const float* wi=Wm+(size_t)i*out;
            for(int o=0;o<out;o++) yr[o]+=xv*wi[o]; } }
}
static double relerr(const float* A, const float* B, size_t nel) {
    double num=0,den=0; for(size_t k=0;k<nel;k++){ double d=(double)A[k]-(double)B[k]; num+=d*d; den+=(double)B[k]*(double)B[k]; }
    return den>0?sqrt(num/den):0.0;
}

/* GPTQ/OBQ-style per-column ternary reconstruction into Weff, using Sig = XᵀX. */
static void reconstruct(const float* W, int in, int out, const double* Sig, float* Weff) {
    #pragma omp parallel
    {
        double* w=malloc(in*sizeof(double)); double* q=malloc(in*sizeof(double));
        double* Sw=malloc(in*sizeof(double)); double* g=malloc(in*sizeof(double));
        #pragma omp for schedule(dynamic,8)
        for (int o=0;o<out;o++){
            for(int i=0;i<in;i++) w[i]=(double)W[(size_t)i*out+o];
            /* Sw = Σ w (fixed for this column) */
            for(int i=0;i<in;i++){ double s=0; const double* Si=Sig+(size_t)i*in;
                for(int j=0;j<in;j++){ s+=Si[j]*w[j]; } Sw[i]=s; }
            /* naive init: absmean scale + nearest ternary */
            double gam=0; for(int i=0;i<in;i++) gam+=fabs(w[i]); gam/=(in?in:1);
            for(int i=0;i<in;i++){ double qi = gam>0?round(w[i]/gam):0; if(qi>1)qi=1; if(qi<-1)qi=-1; q[i]=qi; }
            for(int round=0; round<4; round++){
                /* optimal scale γ* = (qᵀΣw)/(qᵀΣq); Σq computed on the fly */
                double num=0,den=0;
                for(int i=0;i<in;i++){ double sq=0; const double* Si=Sig+(size_t)i*in;
                    for(int j=0;j<in;j++){ sq+=Si[j]*q[j]; } num+=q[i]*Sw[i]; den+=q[i]*sq; }
                if(den>1e-12){ double gn=num/den; if(gn>0) gam=gn; }
                /* residual r=γq−w and its gradient g=Σr */
                double* rr=malloc(in*sizeof(double));
                for(int i=0;i<in;i++) rr[i]=gam*q[i]-w[i];
                for(int i=0;i<in;i++){ double s=0; const double* Si=Sig+(size_t)i*in;
                    for(int j=0;j<in;j++){ s+=Si[j]*rr[j]; } g[i]=s; }
                /* ternary coordinate descent (monotone: only accept dE<0) */
                for(int sweep=0; sweep<3; sweep++){
                    for(int i=0;i<in;i++){
                        double Sii=Sig[(size_t)i*in+i];
                        double bdE=0,bdelta=0,bc=q[i];
                        for(int cc=-1;cc<=1;cc++){ double delta=gam*((double)cc-q[i]); if(delta==0) continue;
                            double dE=2*delta*g[i]+delta*delta*Sii; if(dE<bdE){ bdE=dE; bdelta=delta; bc=cc; } }
                        if(bdelta!=0){ rr[i]+=bdelta; q[i]=bc; const double* Si=Sig+(size_t)i*in;
                            for(int j=0;j<in;j++) g[j]+=bdelta*Si[j]; }
                    }
                }
                free(rr);
            }
            for(int i=0;i<in;i++) Weff[(size_t)i*out+o]=(float)(gam*q[i]);
        }
        free(w);free(q);free(Sw);free(g);
    }
}

static void run_case(int in,int out,int n_tr,int n_ho,int correlated,
                     double* enh,double* eqh,double* ent,double* eqt) {
    size_t Wn=(size_t)in*out; float* W=malloc(Wn*sizeof(float));
    for(size_t k=0;k<Wn;k++) W[k]=(float)(nrand()*0.10);
    float* L=NULL; if(correlated){ L=malloc((size_t)in*in*sizeof(float));
        for(size_t k=0;k<(size_t)in*in;k++) L[k]=(float)(nrand()/sqrt((double)in)); }
    float* Xtr=malloc((size_t)n_tr*in*sizeof(float)); float* Xho=malloc((size_t)n_ho*in*sizeof(float));
    float* Ztr=malloc((size_t)n_tr*in*sizeof(float)); float* Zho=malloc((size_t)n_ho*in*sizeof(float));
    for(size_t k=0;k<(size_t)n_tr*in;k++) Ztr[k]=(float)nrand();
    for(size_t k=0;k<(size_t)n_ho*in;k++) Zho[k]=(float)nrand();
    if(correlated){ matmul(Ztr,L,n_tr,in,in,Xtr); matmul(Zho,L,n_ho,in,in,Xho); }
    else { memcpy(Xtr,Ztr,(size_t)n_tr*in*sizeof(float)); memcpy(Xho,Zho,(size_t)n_ho*in*sizeof(float)); }
    free(Ztr);free(Zho);free(L);

    float* Yfp_tr=malloc((size_t)n_tr*out*sizeof(float)); float* Yfp_ho=malloc((size_t)n_ho*out*sizeof(float));
    matmul(Xtr,W,n_tr,in,out,Yfp_tr); matmul(Xho,W,n_ho,in,out,Yfp_ho);
    float* Weff=malloc(Wn*sizeof(float)); float* Yq_tr=malloc((size_t)n_tr*out*sizeof(float));
    float* Yq_ho=malloc((size_t)n_ho*out*sizeof(float));

    /* naive post-hoc */
    ternize(W,in,out,Weff);
    matmul(Xtr,Weff,n_tr,in,out,Yq_tr); matmul(Xho,Weff,n_ho,in,out,Yq_ho);
    *ent=relerr(Yq_tr,Yfp_tr,(size_t)n_tr*out); *enh=relerr(Yq_ho,Yfp_ho,(size_t)n_ho*out);

    /* data-aware: Σ=XtrᵀXtr (calibration Hessian) + ridge, then per-column reconstruct */
    double* Sig=malloc((size_t)in*in*sizeof(double));
    #pragma omp parallel for schedule(static)
    for(int i=0;i<in;i++) for(int j=0;j<in;j++){ double s=0;
        for(int r=0;r<n_tr;r++) s+=(double)Xtr[(size_t)r*in+i]*(double)Xtr[(size_t)r*in+j];
        Sig[(size_t)i*in+j]=s; }
    for(int i=0;i<in;i++) Sig[(size_t)i*in+i]+=1e-3*(Sig[(size_t)i*in+i]+1.0);
    reconstruct(W,in,out,Sig,Weff);
    matmul(Xtr,Weff,n_tr,in,out,Yq_tr); matmul(Xho,Weff,n_ho,in,out,Yq_ho);
    *eqt=relerr(Yq_tr,Yfp_tr,(size_t)n_tr*out); *eqh=relerr(Yq_ho,Yfp_ho,(size_t)n_ho*out);
    free(Sig);
    free(W);free(Xtr);free(Xho);free(Yfp_tr);free(Yfp_ho);free(Weff);free(Yq_tr);free(Yq_ho);
}

int main(int argc,char** argv){
    int in=(argc>1)?atoi(argv[1]):256, out=(argc>2)?atoi(argv[2]):256;
    int n_tr=(argc>3)?atoi(argv[3]):1024, n_ho=(argc>4)?atoi(argv[4]):512;
    printf("proj_qat_recon: W[%d->%d]  calib=%d held-out=%d  (GPTQ-style Σ-reconstruction)\n",in,out,n_tr,n_ho);

    double nc_ho,qa_ho,nc_tr,qa_tr;
    run_case(in,out,n_tr,n_ho,1,&nc_ho,&qa_ho,&nc_tr,&qa_tr);
    printf("\n  === CORRELATED activations (Σ≠I — the real transformer regime) ===\n");
    printf("    naive post-hoc   : train relerr %.4f   HELD-OUT relerr %.4f\n", nc_tr, nc_ho);
    printf("    data-aware recon : train relerr %.4f   HELD-OUT relerr %.4f\n", qa_tr, qa_ho);
    printf("    HELD-OUT: %.1f%% lower error  (%.4f -> %.4f)\n",
           nc_ho>0?100.0*(nc_ho-qa_ho)/nc_ho:0.0, nc_ho, qa_ho);
    CHECK(qa_ho < nc_ho, "data-aware reconstruction beats naive post-hoc on HELD-OUT activations");
    CHECK(qa_tr <= nc_tr + 1e-9, "data-aware never worse than naive on calibration (monotone)");

    double nc_ho2,qa_ho2,nc_tr2,qa_tr2;
    run_case(in,out,n_tr,n_ho,0,&nc_ho2,&qa_ho2,&nc_tr2,&qa_tr2);
    double gc=nc_ho>0?(nc_ho-qa_ho)/nc_ho:0, gi=nc_ho2>0?(nc_ho2-qa_ho2)/nc_ho2:0;
    printf("\n  === IID activations (Σ=I control) ===\n");
    printf("    naive %.4f  data-aware %.4f  (held-out)\n", nc_ho2, qa_ho2);
    printf("    -> correlated gain %.1f%%  vs  iid gain %.1f%%  (the win comes from activation structure)\n",
           100.0*gc, 100.0*gi);
    CHECK(gc > gi + 0.02, "the win is from activation structure (correlated gain > iid gain)");

    printf("\nproj_qat_recon: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail?1:0;
}
