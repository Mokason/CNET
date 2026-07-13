/* proj_qat_gemma — milestone 2: the per-projection data-aware ternary
 * reconstruction (proven hermetically in proj_qat_recon) run on REAL weights and
 * REAL activations from a real model CNET reads.
 *
 * Target: a real gemma4 GGUF's layer-0 attention Q projection. Its input is
 * RMSNorm(embed(token)·√D) — computable directly from the token embedding table
 * and the attn_norm weight (gemma bakes (1+w) into the GGUF norm, so it is a
 * plain (x·ss)·w), so we get REAL activations with NO deep forward. We then
 * reconstruct the real Q weight from a calibration batch and measure the
 * HELD-OUT output-reconstruction error vs naive post-hoc ternary — on real data.
 *
 * Honest scope: calibration tokens are sampled ids (no tokenizer wired), so the
 * activation covariance is the embedding table's, not coherent text — still real,
 * anisotropic structure. Deeper layers need the full forward (later milestone).
 *
 * Build: make proj_qat_gemma   (needs Models/gemma-4-12B-it-MTP-Q8_0.gguf)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_tensor.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, msg) do { if (c) { g_pass++; printf("  ok   %s\n", (msg)); } \
                           else { g_fail++; printf("  FAIL: %s\n", (msg)); } } while (0)

static unsigned long long g_rng = 0x9E3779B97F4A7C15ULL;
static unsigned rnd_u(void){ g_rng = g_rng*6364136223846793005ULL + 1442695040888963407ULL; return (unsigned)(g_rng>>33); }

/* ---- reconstruction helpers (same math as proj_qat_recon.c) ---- */
static void ternize(const float* W,int in,int out,float* Weff){
    #pragma omp parallel for schedule(static)
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
static void reconstruct(const float* W,int in,int out,const double* Sig,float* Weff){
    #pragma omp parallel
    {
        double* w=malloc(in*sizeof(double)); double* q=malloc(in*sizeof(double));
        double* Sw=malloc(in*sizeof(double)); double* g=malloc(in*sizeof(double));
        #pragma omp for schedule(dynamic,4)
        for(int o=0;o<out;o++){
            for(int i=0;i<in;i++) w[i]=(double)W[(size_t)i*out+o];
            for(int i=0;i<in;i++){ double s=0; const double* Si=Sig+(size_t)i*in;
                for(int j=0;j<in;j++){ s+=Si[j]*w[j]; } Sw[i]=s; }
            double gam=0; for(int i=0;i<in;i++) gam+=fabs(w[i]); gam/=(in?in:1);
            for(int i=0;i<in;i++){ double qi=gam>0?round(w[i]/gam):0; if(qi>1)qi=1; if(qi<-1)qi=-1; q[i]=qi; }
            for(int round=0;round<4;round++){
                double num=0,den=0;
                for(int i=0;i<in;i++){ double sq=0; const double* Si=Sig+(size_t)i*in;
                    for(int j=0;j<in;j++){ sq+=Si[j]*q[j]; } num+=q[i]*Sw[i]; den+=q[i]*sq; }
                if(den>1e-12){ double gn=num/den; if(gn>0) gam=gn; }
                double* rr=malloc(in*sizeof(double));
                for(int i=0;i<in;i++) rr[i]=gam*q[i]-w[i];
                for(int i=0;i<in;i++){ double s=0; const double* Si=Sig+(size_t)i*in;
                    for(int j=0;j<in;j++){ s+=Si[j]*rr[j]; } g[i]=s; }
                for(int sweep=0;sweep<3;sweep++) for(int i=0;i<in;i++){
                    double Sii=Sig[(size_t)i*in+i]; double bdE=0,bd=0,bc=q[i];
                    for(int cc=-1;cc<=1;cc++){ double delta=gam*((double)cc-q[i]); if(delta==0) continue;
                        double dE=2*delta*g[i]+delta*delta*Sii; if(dE<bdE){ bdE=dE; bd=delta; bc=cc; } }
                    if(bd!=0){ rr[i]+=bd; q[i]=bc; const double* Si=Sig+(size_t)i*in;
                        for(int j=0;j<in;j++) g[j]+=bd*Si[j]; }
                }
                free(rr);
            }
            for(int i=0;i<in;i++) Weff[(size_t)i*out+o]=(float)(gam*q[i]);
        }
        free(w);free(q);free(Sw);free(g);
    }
}

int main(int argc,char** argv){
    const char* path = (argc>1)?argv[1]:"Models/gemma-4-12B-it-MTP-Q8_0.gguf";
    int n_tr=(argc>2)?atoi(argv[2]):8192, n_ho=(argc>3)?atoi(argv[3]):512;  /* n_tr >> D or Σ is rank-deficient */
    int out_cap=(argc>4)?atoi(argv[4]):512;   /* reconstruct a subset of output cols for speed */

    cce_gguf* gg=NULL;
    if(cce_gguf_load(path,&gg)!=CCE_OK || !gg){ printf("FAIL: cannot load %s\n",path); return 1; }
    const char* arch=cce_gguf_get_arch(gg);
    int D=cce_gguf_get_hidden_size(gg); float eps=cce_gguf_get_rms_eps(gg);
    if(eps<=0) eps=1e-6f;
    float embed_scale=(arch&&strncmp(arch,"gemma",5)==0)?sqrtf((float)D):1.0f;
    printf("proj_qat_gemma: %s  arch=%s D=%d eps=%.1e embed_scale=%.2f\n", path, arch?arch:"?", D, eps, embed_scale);

    /* load real tensors */
    cce_tensor emb={0}, nrm={0}, q={0};
    if(cce_gguf_load_tensor_by_name(gg,"token_embd.weight",&emb)!=CCE_OK){ printf("FAIL: token_embd\n"); return 1; }
    if(cce_gguf_load_tensor_by_name(gg,"blk.0.attn_norm.weight",&nrm)!=CCE_OK){ printf("FAIL: attn_norm\n"); return 1; }
    if(cce_gguf_load_tensor_by_name(gg,"blk.0.attn_q.weight",&q)!=CCE_OK){ printf("FAIL: attn_q\n"); return 1; }
    long vocab=(long)(emb.numel/(size_t)D);
    long out_full=(long)(q.numel/(size_t)D);      /* q stored [out][in], in=D */
    printf("  token_embd numel=%zu -> vocab=%ld, D=%d | attn_q numel=%zu -> out=%ld (in=D=%d)\n",
           emb.numel, vocab, D, q.numel, out_full, D);
    CHECK((size_t)vocab*D==emb.numel && vocab>1000, "token_embd shape [vocab,D] recovered");
    CHECK((size_t)out_full*D==q.numel && out_full>=out_cap, "attn_q shape [out,in=D] recovered");
    if(g_fail){ cce_gguf_free(gg); return 1; }
    int out=out_cap; if(out>out_full) out=(int)out_full;

    /* real activations X = RMSNorm(embed(token)·embed_scale) for sampled tokens */
    int ntot=n_tr+n_ho;
    float* X=malloc((size_t)ntot*D*sizeof(float));
    for(int t=0;t<ntot;t++){
        long tok=(long)(rnd_u()%(unsigned)vocab);
        const float* er=emb.data+(size_t)tok*D;
        double ss=0; for(int i=0;i<D;i++){ double v=(double)er[i]*embed_scale; ss+=v*v; }
        ss=1.0/sqrt(ss/D+eps);
        float* xr=X+(size_t)t*D;
        for(int i=0;i<D;i++) xr[i]=(float)((double)er[i]*embed_scale*ss)*nrm.data[i];
    }
    float* Xtr=X; float* Xho=X+(size_t)n_tr*D;

    /* real weight W as [in=D][out] (transpose from GGUF [out][in]) */
    size_t Wn=(size_t)D*out; float* W=malloc(Wn*sizeof(float));
    for(int o=0;o<out;o++){ const float* wo=q.data+(size_t)o*D; for(int i=0;i<D;i++) W[(size_t)i*out+o]=wo[i]; }

    float* Yfp_tr=malloc((size_t)n_tr*out*sizeof(float)); float* Yfp_ho=malloc((size_t)n_ho*out*sizeof(float));
    matmul(Xtr,W,n_tr,D,out,Yfp_tr); matmul(Xho,W,n_ho,D,out,Yfp_ho);
    float* Weff=malloc(Wn*sizeof(float)); float* Yq_tr=malloc((size_t)n_tr*out*sizeof(float));
    float* Yq_ho=malloc((size_t)n_ho*out*sizeof(float));

    /* naive post-hoc */
    ternize(W,D,out,Weff); matmul(Xtr,Weff,n_tr,D,out,Yq_tr); matmul(Xho,Weff,n_ho,D,out,Yq_ho);
    double nc_tr=relerr(Yq_tr,Yfp_tr,(size_t)n_tr*out), nc_ho=relerr(Yq_ho,Yfp_ho,(size_t)n_ho*out);

    /* data-aware: Σ from calibration activations */
    double* Sig=malloc((size_t)D*D*sizeof(double));
    #pragma omp parallel for schedule(static)
    for(int i=0;i<D;i++) for(int j=0;j<D;j++){ double s=0;
        for(int r=0;r<n_tr;r++) s+=(double)Xtr[(size_t)r*D+i]*(double)Xtr[(size_t)r*D+j]; Sig[(size_t)i*D+j]=s; }
    /* GPTQ-style damping: add 1% of the mean diagonal (Tikhonov — critical when
       calibration is under-sampled; without n_tr >> D, Σ is rank-deficient and
       the solver overfits calibration and hurts held-out). */
    double meandiag=0; for(int i=0;i<D;i++) meandiag+=Sig[(size_t)i*D+i]; meandiag/=D;
    double damp=0.01*meandiag; for(int i=0;i<D;i++) Sig[(size_t)i*D+i]+=damp;
    printf("  calibration: n_tr=%d for D=%d (ratio %.1f×; need >~2× or Σ rank-deficient), damping 1%% mean-diag\n",
           n_tr, D, (double)n_tr/D);
    reconstruct(W,D,out,Sig,Weff);
    matmul(Xtr,Weff,n_tr,D,out,Yq_tr); matmul(Xho,Weff,n_ho,D,out,Yq_ho);
    double qa_tr=relerr(Yq_tr,Yfp_tr,(size_t)n_tr*out), qa_ho=relerr(Yq_ho,Yfp_ho,(size_t)n_ho*out);

    printf("\n  === REAL gemma blk.0 attn_q [%d->%d cols] : output reconstruction ===\n", D, out);
    printf("    naive post-hoc   : train relerr %.4f   HELD-OUT relerr %.4f\n", nc_tr, nc_ho);
    printf("    data-aware recon : train relerr %.4f   HELD-OUT relerr %.4f\n", qa_tr, qa_ho);
    printf("    HELD-OUT: %.1f%% lower error on REAL weights+activations  (%.4f -> %.4f)\n",
           nc_ho>0?100.0*(nc_ho-qa_ho)/nc_ho:0.0, nc_ho, qa_ho);
    CHECK(qa_ho < nc_ho, "data-aware beats naive post-hoc on HELD-OUT real activations");
    CHECK(qa_tr <= nc_tr + 1e-9, "data-aware never worse than naive on calibration (monotone)");

    free(X);free(W);free(Yfp_tr);free(Yfp_ho);free(Weff);free(Yq_tr);free(Yq_ho);free(Sig);
    cce_tensor_free(&emb); cce_tensor_free(&nrm); cce_tensor_free(&q); cce_gguf_free(gg);
    printf("\nproj_qat_gemma: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail?1:0;
}
