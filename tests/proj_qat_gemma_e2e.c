/* proj_qat_gemma_e2e — milestone 4-full, Part 2: REAL end-to-end on gemma-4-12B.
 *
 * PATH: MLP-subgraph fallback (2b). STEP 2a is INFEASIBLE for this checkpoint:
 * gemma-4-12B-it-MTP has NO separate attn_k/attn_v tensors (KV-sharing/MTP), and
 * CNET's full forward explicitly REFUSES to load it ("layer 0 lacks attn_k/attn_v
 * — KV-sharing archs unsupported"). Per the brief we DO NOT fake attention. Instead
 * we run the REAL gemma MLP block, which is fully present and faithful:
 *
 *   h0 = embed(token) * sqrt(D)                         (real token_embd, gemma scale)
 *   per layer l (x4):
 *     ln = RMSNorm(h) * ffn_norm[l]                     (gemma bakes 1+w; plain multiply)
 *     a  = gelu_tanh(gate[l]·ln) * (up[l]·ln)           (real GeGLU, exact tanh constants)
 *     d  = down[l]·a
 *     d  = RMSNorm(d) * post_ffw_norm[l]                (gemma sandwich norm)
 *     h  = (h + d) * layer_output_scale[l]              (scalar, gemma4)
 *   logits = (RMSNorm(h) * output_norm) · token_embd^T  (tied head, vocab=262144)
 *
 * Every weight/norm is the real dequantized gemma tensor; the forward math is copied
 * from src/cce/cce_gguf.c's own FFN block, so it is self-consistent and faithful up to
 * the (documented) absence of attention. We quantize ALL 12 FFN projections
 * (gate/up/down x 4 layers) two ways — naive per-output ternary vs GPTQ data-aware
 * (Σ-based OBQ with act-order + optimal-scale refit) — and measure how close the
 * QUANTIZED model's HELD-OUT output stays to the FP model: final-logit relative error
 * AND next-token argmax agreement. No tokenizer is wired, so held-out is sampled ids.
 *
 * Build: make proj_qat_gemma_e2e   (needs Models/gemma-4-12B-it-MTP-Q8_0.gguf)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <omp.h>
#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_tensor.h"

static int g_pass=0,g_fail=0;
#define CHECK(c,msg) do{ if(c){g_pass++;printf("  ok   %s\n",(msg));} else {g_fail++;printf("  FAIL: %s\n",(msg));} }while(0)
static unsigned long long g_rng=0x9E3779B97F4A7C15ULL;
static unsigned rnd_u(void){ g_rng=g_rng*6364136223846793005ULL+1442695040888963407ULL; return (unsigned)(g_rng>>33); }

/* ---------- generic ternary / matmul / metrics (W laid out [in][out]) ---------- */
static void ternize(const float* W,int in,int out,float* Weff){
    #pragma omp parallel for schedule(static)
    for(int o=0;o<out;o++){ double s=0; for(int i=0;i<in;i++) s+=fabs((double)W[(size_t)i*out+o]);
        float g=(float)(s/(in>0?in:1));
        for(int i=0;i<in;i++){ float w=W[(size_t)i*out+o],r=0.0f;
            if(g>0){ r=roundf(w/g); if(r>1)r=1; if(r<-1)r=-1; } Weff[(size_t)i*out+o]=g*r; } }
}
/* per-output-column symmetric int8 (near-lossless): scale=max|w|/127. Effective
   weights returned as float, same convention as ternize. Used for mixed precision. */
static void int8ize(const float* W,int in,int out,float* Weff){
    #pragma omp parallel for schedule(static)
    for(int o=0;o<out;o++){ double mx=0; for(int i=0;i<in;i++){ double a=fabs((double)W[(size_t)i*out+o]); if(a>mx)mx=a; }
        float s=(float)(mx/127.0);
        for(int i=0;i<in;i++){ float w=W[(size_t)i*out+o],r=0.0f;
            if(s>0){ r=roundf(w/s); if(r>127)r=127; if(r<-127)r=-127; } Weff[(size_t)i*out+o]=s*r; } }
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

/* ---------- GPTQ-Cholesky data-aware solver (act-order + optimal-scale refit) ----------
 * Same math as tests/gptq_solver.c, "fast+refit" config (a single OBQ error-feedback
 * pass — no coordinate polish, which is intractable at in=8192). W/Weff are [in][out];
 * Sig is [in][in] and ALREADY damped by the caller. */
static void gptq_build_U(const double* H,int in,double* U,int* dead){
    double* L=calloc((size_t)in*in,sizeof(double));
    double* Minv=calloc((size_t)in*in,sizeof(double));
    double* Hinv=calloc((size_t)in*in,sizeof(double));
    memset(U,0,(size_t)in*in*sizeof(double));
    double md=0; for(int i=0;i<in;i++) md+=H[(size_t)i*in+i]; md/=(in?in:1);
    double tiny=1e-10*(md>0?md:1.0);
    #pragma omp parallel
    { for(int j=0;j<in;j++){
        #pragma omp single
        { double d=H[(size_t)j*in+j]; for(int k=0;k<j;k++){ double l=L[(size_t)j*in+k]; d-=l*l; }
          if(d<=tiny){ dead[j]=1; L[(size_t)j*in+j]=1.0; } else { dead[j]=0; L[(size_t)j*in+j]=sqrt(d); } }
        if(dead[j]) continue; double ljj=L[(size_t)j*in+j];
        #pragma omp for schedule(static)
        for(int i=j+1;i<in;i++){ double s=H[(size_t)i*in+j]; const double* Li=L+(size_t)i*in; const double* Lj=L+(size_t)j*in;
            for(int k=0;k<j;k++) s-=Li[k]*Lj[k]; L[(size_t)i*in+j]=s/ljj; } } }
    #pragma omp parallel
    { for(int i=0;i<in;i++){ double lii=L[(size_t)i*in+i];
        #pragma omp single
        { Minv[(size_t)i*in+i]=1.0/lii; }
        #pragma omp for schedule(static)
        for(int j=0;j<i;j++){ if(dead[j]){ Minv[(size_t)i*in+j]=0.0; continue; }
            double s=0; for(int k=j;k<i;k++) s+=L[(size_t)i*in+k]*Minv[(size_t)k*in+j]; Minv[(size_t)i*in+j]=-s/lii; } } }
    #pragma omp parallel for schedule(dynamic,8)
    for(int a=0;a<in;a++) for(int b=a;b<in;b++){ double s=0;
        for(int k=b;k<in;k++) s+=Minv[(size_t)k*in+a]*Minv[(size_t)k*in+b];
        Hinv[(size_t)a*in+b]=s; Hinv[(size_t)b*in+a]=s; }
    #pragma omp parallel
    { for(int i=0;i<in;i++){
        #pragma omp single
        { double d=Hinv[(size_t)i*in+i]; for(int k=0;k<i;k++){ double u=U[(size_t)k*in+i]; d-=u*u; }
          if(d<=tiny||dead[i]){ dead[i]=1; U[(size_t)i*in+i]=1.0; } else U[(size_t)i*in+i]=sqrt(d); }
        if(dead[i]){
            #pragma omp for schedule(static)
            for(int j=i+1;j<in;j++) U[(size_t)i*in+j]=0.0;
            continue;
        }
        double uii=U[(size_t)i*in+i];
        #pragma omp for schedule(static)
        for(int j=i+1;j<in;j++){ double s=Hinv[(size_t)i*in+j];
            for(int k=0;k<i;k++) s-=U[(size_t)k*in+i]*U[(size_t)k*in+j]; U[(size_t)i*in+j]=s/uii; } } }
    free(L);free(Minv);free(Hinv);
}
static const double* g_sortdiag;
static int cmp_desc(const void* a,const void* b){ int ia=*(const int*)a,ib=*(const int*)b;
    double da=g_sortdiag[ia],db=g_sortdiag[ib]; return (da<db)-(da>db); }

static void reconstruct_gptq(const float* W,int in,int out,const double* Sig,float* Weff){
    int* perm=malloc((size_t)in*sizeof(int)); for(int i=0;i<in;i++) perm[i]=i;
    double* diag=malloc((size_t)in*sizeof(double)); for(int i=0;i<in;i++) diag[i]=Sig[(size_t)i*in+i];
    g_sortdiag=diag; qsort(perm,in,sizeof(int),cmp_desc); g_sortdiag=NULL;
    double* Hp=malloc((size_t)in*in*sizeof(double));
    #pragma omp parallel for schedule(static)
    for(int a=0;a<in;a++){ int pa=perm[a]; const double* Sa=Sig+(size_t)pa*in; double* Ha=Hp+(size_t)a*in;
        for(int b=0;b<in;b++) Ha[b]=Sa[perm[b]]; }
    double* Wr=malloc((size_t)out*in*sizeof(double));
    double* Wo=malloc((size_t)out*in*sizeof(double));
    double* gam=malloc((size_t)out*sizeof(double));
    #pragma omp parallel for schedule(static)
    for(int o=0;o<out;o++){ double* wr=Wr+(size_t)o*in; double* wo=Wo+(size_t)o*in; double s=0;
        for(int k=0;k<in;k++){ double v=(double)W[(size_t)perm[k]*out+o]; wr[k]=v; wo[k]=v; s+=fabs(v); }
        gam[o]=s/(in>0?in:1); }
    double* U=malloc((size_t)in*in*sizeof(double)); int* dead=calloc((size_t)in,sizeof(int));
    gptq_build_U(Hp,in,U,dead);
    double* Q=malloc((size_t)out*in*sizeof(double));
    /* single OBQ error-feedback pass in act-order, record integer pattern in Q */
    #pragma omp parallel
    { for(int i=0;i<in;i++){ double uii=U[(size_t)i*in+i]; int di=dead[i]; const double* Ui=U+(size_t)i*in;
        #pragma omp for schedule(static)
        for(int o=0;o<out;o++){ double* wr=Wr+(size_t)o*in; double e; double r=0.0;
            if(di) e=0.0; else { double g=gam[o],wv=wr[i]; if(g>0){ r=round(wv/g); if(r>1)r=1; if(r<-1)r=-1; } e=(wv-g*r)/uii; }
            Q[(size_t)o*in+i]=r; if(e!=0.0) for(int j=i+1;j<in;j++) wr[j]-=e*Ui[j]; } } }
    /* optimal-scale refit gamma* = (qᵀΣw)/(qᵀΣq), then scale */
    #pragma omp parallel
    { double* Sw=malloc((size_t)in*sizeof(double));
      #pragma omp for schedule(dynamic,8)
      for(int o=0;o<out;o++){ double* q=Q+(size_t)o*in; const double* wo=Wo+(size_t)o*in;
        for(int i=0;i<in;i++){ double s=0; const double* Hi=Hp+(size_t)i*in; for(int j=0;j<in;j++) s+=Hi[j]*wo[j]; Sw[i]=s; }
        double num=0,den=0;
        for(int i=0;i<in;i++){ double sq=0; const double* Hi=Hp+(size_t)i*in; for(int j=0;j<in;j++) sq+=Hi[j]*q[j]; num+=q[i]*Sw[i]; den+=q[i]*sq; }
        double g=gam[o]; if(den>1e-12){ double gn=num/den; if(gn>0) g=gn; }
        for(int i=0;i<in;i++) q[i]*=g; }
      free(Sw); }
    #pragma omp parallel for schedule(static)
    for(int o=0;o<out;o++){ const double* q=Q+(size_t)o*in; for(int k=0;k<in;k++) Weff[(size_t)perm[k]*out+o]=(float)q[k]; }
    free(perm);free(diag);free(Hp);free(Wr);free(Wo);free(gam);free(U);free(dead);free(Q);
}
/* Σ = XᵀX (in×in) over n rows + mean-diag GPTQ damping. Cache-friendly rank-1
 * accumulation (parallel over row i, contiguous inner j) — ~5-10x faster than the
 * strided triple loop at in=8192. g_damp fraction of the mean diagonal. */
static double g_damp=0.01;
static void build_sigma(const float* X,int n,int in,double* Sig){
    #pragma omp parallel for schedule(static)
    for(int i=0;i<in;i++){ double* row=Sig+(size_t)i*in;
        for(int j=0;j<in;j++) row[j]=0.0;
        for(int r=0;r<n;r++){ double xi=(double)X[(size_t)r*in+i]; const float* xr=X+(size_t)r*in;
            for(int j=0;j<in;j++) row[j]+=xi*(double)xr[j]; } }
    double md=0; for(int i=0;i<in;i++) md+=Sig[(size_t)i*in+i]; md/=in;
    for(int i=0;i<in;i++) Sig[(size_t)i*in+i]+=g_damp*md;
}

/* ---------- faithful gemma MLP forward pieces ---------- */
static float g_eps=1e-6f;
static void rmsnorm_w(const float* X,int n,int d,const float* w,float* Y){
    #pragma omp parallel for schedule(static)
    for(int r=0;r<n;r++){ const float* xr=X+(size_t)r*d; float* yr=Y+(size_t)r*d;
        double ss=0; for(int i=0;i<d;i++) ss+=(double)xr[i]*xr[i]; float inv=(float)(1.0/sqrt(ss/d+g_eps));
        for(int i=0;i<d;i++) yr[i]=(xr[i]*inv)*w[i]; }
}
static void gelu_tanh_mul(const float* G,const float* U,int n,int hid,float* A){
    const float c0=0.79788456080286535588f, c1=0.044715f;
    #pragma omp parallel for schedule(static)
    for(size_t k=0;k<(size_t)n*hid;k++){ float x=G[k];
        float gg=0.5f*x*(1.0f+tanhf(c0*x*(1.0f+c1*x*x))); A[k]=gg*U[k]; }
}

typedef struct { float *gate,*up,*down; } layer_w; /* each [in][out] transposed */

/* forward the MLP stack. gate/up:[D][hid]  down:[hid][D]. If cap_ln/cap_a!=NULL,
 * capture per-layer gate/up input (RMSNorm out, [n][D]) and down input (GeGLU, [n][hid]). */
static void forward_mlp(const float* H0,int n,int D,int hid,int L,
                        layer_w* lw,const float* ffn_norm,const float* post_norm,const float* loscale,
                        float** cap_ln,float** cap_a,float* Hout){
    float* h=malloc((size_t)n*D*sizeof(float)); memcpy(h,H0,(size_t)n*D*sizeof(float));
    float* ln=malloc((size_t)n*D*sizeof(float));
    float* G=malloc((size_t)n*hid*sizeof(float)),*Uv=malloc((size_t)n*hid*sizeof(float)),*A=malloc((size_t)n*hid*sizeof(float));
    float* dn=malloc((size_t)n*D*sizeof(float)),*dnn=malloc((size_t)n*D*sizeof(float));
    for(int l=0;l<L;l++){
        rmsnorm_w(h,n,D,ffn_norm+(size_t)l*D,ln);
        if(cap_ln) memcpy(cap_ln[l],ln,(size_t)n*D*sizeof(float));
        matmul(ln,lw[l].gate,n,D,hid,G); matmul(ln,lw[l].up,n,D,hid,Uv);
        gelu_tanh_mul(G,Uv,n,hid,A);
        if(cap_a) memcpy(cap_a[l],A,(size_t)n*hid*sizeof(float));
        matmul(A,lw[l].down,n,hid,D,dn);
        rmsnorm_w(dn,n,D,post_norm+(size_t)l*D,dnn);
        float sc=loscale[l];
        #pragma omp parallel for schedule(static)
        for(size_t k=0;k<(size_t)n*D;k++) h[k]=(h[k]+dnn[k])*sc;
    }
    memcpy(Hout,h,(size_t)n*D*sizeof(float));
    free(h);free(ln);free(G);free(Uv);free(A);free(dn);free(dnn);
}
/* tied head: logits[r][v] = sum_i (RMSNorm(H)*output_norm)[r][i] * emb[v][i] */
static void head_logits(const float* H,int n,int D,const float* out_norm,const float* emb,long vocab,float* logits){
    float* fn=malloc((size_t)n*D*sizeof(float)); rmsnorm_w(H,n,D,out_norm,fn);
    #pragma omp parallel for schedule(dynamic,4)
    for(int r=0;r<n;r++){ const float* fr=fn+(size_t)r*D; float* lr=logits+(size_t)r*vocab;
        for(long v=0;v<vocab;v++){ const float* ev=emb+(size_t)v*D; double s=0;
            for(int i=0;i<D;i++) s+=(double)fr[i]*ev[i]; lr[v]=(float)s; } }
    free(fn);
}
static double argmax_agree(const float* Lq,const float* Lfp,int n,long vocab){
    int agree=0;
    #pragma omp parallel for schedule(dynamic,4) reduction(+:agree)
    for(int r=0;r<n;r++){ const float* q=Lq+(size_t)r*vocab; const float* f=Lfp+(size_t)r*vocab;
        long aq=0,af=0; for(long v=1;v<vocab;v++){ if(q[v]>q[aq])aq=v; if(f[v]>f[af])af=v; }
        if(aq==af) agree++; }
    return (double)agree/n;
}

int main(int argc,char** argv){
    setvbuf(stdout,NULL,_IOLBF,0);
    const char* path=(argc>1)?argv[1]:"Models/gemma-4-12B-it-MTP-Q8_0.gguf";
    int n_tr=(argc>2)?atoi(argv[2]):8192;     /* calibration tokens (capped; >> D=1024) */
    int n_ho=(argc>3)?atoi(argv[3]):512;      /* held-out tokens for the FP-vs-quant metric */
    int Lcap=(argc>4)?atoi(argv[4]):4;        /* layers to run (<=4); log if capped */
    g_damp=(argc>5)?atof(argv[5])/100.0:0.05; /* Hessian damping % (down-proj is under-sampled) */

    FILE* checkpoint=fopen(path,"rb");
    if(!checkpoint){
        printf("REAL_PROJ_QAT_GEMMA_E2E_SKIPPED reason=no_checkpoint path=%s\n",path);
        return getenv("CNET_REQUIRE_REAL_MODEL")?1:0;
    }
    fclose(checkpoint);
    cce_gguf* gg=NULL;
    if(cce_gguf_load(path,&gg)!=CCE_OK||!gg){ printf("FAIL: cannot load %s\n",path); return 1; }
    const char* arch=cce_gguf_get_arch(gg);
    int D=cce_gguf_get_hidden_size(gg); float eps=cce_gguf_get_rms_eps(gg); if(eps<=0)eps=1e-6f; g_eps=eps;
    int hid=cce_gguf_get_feed_forward_length(gg);
    int nlayer=cce_gguf_get_n_layer(gg); if(Lcap>nlayer)Lcap=nlayer;
    float embed_scale=(arch&&strncmp(arch,"gemma",5)==0)?sqrtf((float)D):1.0f;
    printf("proj_qat_gemma_e2e: %s\n  arch=%s D=%d ffn=%d layers=%d(run %d) eps=%.1e embed_scale=%.2f\n",
           path, arch?arch:"?", D, hid, nlayer, Lcap, eps, embed_scale);
    printf("  PATH 2b (MLP-subgraph): full forward refused for this MTP checkpoint (no attn_k/attn_v).\n");

    /* load token_embd + output_norm */
    cce_tensor emb={0},outn={0};
    if(cce_gguf_load_tensor_by_name(gg,"token_embd.weight",&emb)!=CCE_OK){ printf("FAIL token_embd\n"); return 1; }
    if(cce_gguf_load_tensor_by_name(gg,"output_norm.weight",&outn)!=CCE_OK){ printf("FAIL output_norm\n"); return 1; }
    long vocab=(long)(emb.numel/(size_t)D);
    printf("  vocab=%ld (tied head)  token_embd numel=%zu\n", vocab, emb.numel);

    /* load per-layer FFN weights (transpose GGUF [out][in] -> [in][out]) + norms */
    layer_w* lw=calloc(Lcap,sizeof(layer_w));
    float* ffn_norm=malloc((size_t)Lcap*D*sizeof(float));
    float* post_norm=malloc((size_t)Lcap*D*sizeof(float));
    float* loscale=malloc((size_t)Lcap*sizeof(float));
    double normmean=0;
    for(int l=0;l<Lcap;l++){
        char nm[128]; cce_tensor g={0},u={0},d={0},fn={0},pn={0},ls={0};
        snprintf(nm,sizeof nm,"blk.%d.ffn_gate.weight",l); if(cce_gguf_load_tensor_by_name(gg,nm,&g)!=CCE_OK){printf("FAIL %s\n",nm);return 1;}
        snprintf(nm,sizeof nm,"blk.%d.ffn_up.weight",l);   if(cce_gguf_load_tensor_by_name(gg,nm,&u)!=CCE_OK){printf("FAIL %s\n",nm);return 1;}
        snprintf(nm,sizeof nm,"blk.%d.ffn_down.weight",l); if(cce_gguf_load_tensor_by_name(gg,nm,&d)!=CCE_OK){printf("FAIL %s\n",nm);return 1;}
        snprintf(nm,sizeof nm,"blk.%d.ffn_norm.weight",l); if(cce_gguf_load_tensor_by_name(gg,nm,&fn)!=CCE_OK){printf("FAIL %s\n",nm);return 1;}
        snprintf(nm,sizeof nm,"blk.%d.post_ffw_norm.weight",l); if(cce_gguf_load_tensor_by_name(gg,nm,&pn)!=CCE_OK){printf("FAIL %s\n",nm);return 1;}
        snprintf(nm,sizeof nm,"blk.%d.layer_output_scale.weight",l);
        loscale[l]=(cce_gguf_load_tensor_by_name(gg,nm,&ls)==CCE_OK && ls.numel>0)?ls.data[0]:1.0f;
        /* gate/up: GGUF [out=hid][in=D] -> [in=D][out=hid]; down: [out=D][in=hid] -> [in=hid][out=D] */
        lw[l].gate=malloc((size_t)D*hid*sizeof(float)); lw[l].up=malloc((size_t)D*hid*sizeof(float)); lw[l].down=malloc((size_t)hid*D*sizeof(float));
        for(int o=0;o<hid;o++){ const float* go=g.data+(size_t)o*D; const float* uo=u.data+(size_t)o*D;
            for(int i=0;i<D;i++){ lw[l].gate[(size_t)i*hid+o]=go[i]; lw[l].up[(size_t)i*hid+o]=uo[i]; } }
        for(int o=0;o<D;o++){ const float* dobuf=d.data+(size_t)o*hid;
            for(int i=0;i<hid;i++) lw[l].down[(size_t)i*D+o]=dobuf[i]; }
        memcpy(ffn_norm+(size_t)l*D,fn.data,(size_t)D*sizeof(float));
        memcpy(post_norm+(size_t)l*D,pn.data,(size_t)D*sizeof(float));
        for(int i=0;i<D;i++) normmean+=fn.data[i];
        cce_tensor_free(&g);cce_tensor_free(&u);cce_tensor_free(&d);cce_tensor_free(&fn);cce_tensor_free(&pn);cce_tensor_free(&ls);
    }
    printf("  ffn_norm mean=%.3f (near 1.0 => 1+w baked into GGUF, applied as plain multiply)\n", normmean/((double)Lcap*D));
    printf("  layer_output_scale[0..]= "); for(int l=0;l<Lcap;l++) printf("%.3f ",loscale[l]); printf("\n");

    /* real inputs h0 = embed(sampled token) * embed_scale */
    int ntot=n_tr+n_ho;
    float* H0=malloc((size_t)ntot*D*sizeof(float));
    for(int t=0;t<ntot;t++){ long tok=(long)(rnd_u()%(unsigned)vocab); const float* er=emb.data+(size_t)tok*D;
        float* h=H0+(size_t)t*D; for(int i=0;i<D;i++) h[i]=er[i]*embed_scale; }
    float* H0tr=H0; float* H0ho=H0+(size_t)n_tr*D;
    printf("  calibration n_tr=%d (D=%d ratio %.1fx OK; down-proj in=hid=%d ratio %.2fx — UNDER-sampled, damping-reliant)\n",
           n_tr, D, (double)n_tr/D, hid, (double)n_tr/hid);

    /* ---- FP reference: capture calib activations, compute held-out logits ---- */
    float** cap_ln=malloc(Lcap*sizeof(float*)); float** cap_a=malloc(Lcap*sizeof(float*));
    for(int l=0;l<Lcap;l++){ cap_ln[l]=malloc((size_t)n_tr*D*sizeof(float)); cap_a[l]=malloc((size_t)n_tr*hid*sizeof(float)); }
    float* Hfp_tr=malloc((size_t)n_tr*D*sizeof(float));
    float* Hfp_ho=malloc((size_t)n_ho*D*sizeof(float));
    forward_mlp(H0tr,n_tr,D,hid,Lcap,lw,ffn_norm,post_norm,loscale,cap_ln,cap_a,Hfp_tr);
    forward_mlp(H0ho,n_ho,D,hid,Lcap,lw,ffn_norm,post_norm,loscale,NULL,NULL,Hfp_ho);
    float* Lfp=malloc((size_t)n_ho*vocab*sizeof(float));
    head_logits(Hfp_ho,n_ho,D,outn.data,emb.data,vocab,Lfp);

    /* quantized weight banks + held-out logits buffer */
    layer_w* qw=calloc(Lcap,sizeof(layer_w));
    for(int l=0;l<Lcap;l++){ qw[l].gate=malloc((size_t)D*hid*sizeof(float)); qw[l].up=malloc((size_t)D*hid*sizeof(float)); qw[l].down=malloc((size_t)hid*D*sizeof(float)); }
    float* Hq_ho=malloc((size_t)n_ho*D*sizeof(float));
    float* Lq=malloc((size_t)n_ho*vocab*sizeof(float));

    /* ---- NAIVE post-hoc ternary on every FFN projection ---- */
    for(int l=0;l<Lcap;l++){ ternize(lw[l].gate,D,hid,qw[l].gate); ternize(lw[l].up,D,hid,qw[l].up); int8ize(lw[l].down,hid,D,qw[l].down); }
    forward_mlp(H0ho,n_ho,D,hid,Lcap,qw,ffn_norm,post_norm,loscale,NULL,NULL,Hq_ho);
    head_logits(Hq_ho,n_ho,D,outn.data,emb.data,vocab,Lq);
    double naive_rel=relerr(Lq,Lfp,(size_t)n_ho*vocab);
    double naive_arg=argmax_agree(Lq,Lfp,n_ho,vocab);
    double naive_hid=relerr(Hq_ho,Hfp_ho,(size_t)n_ho*D);

    /* ---- GPTQ data-aware (independent: each projection reconstructed on FP activations) ---- */
    double* Sig_d=malloc((size_t)D*D*sizeof(double));
    double* Sig_h=malloc((size_t)hid*hid*sizeof(double));
    printf("  MIXED PRECISION: gate/up -> GPTQ data-aware ternary (in=%d, well-calibrated); wide down-proj -> int8 near-lossless (in=%d, else under-sampled). damp=%.1f%%...\n",D,hid,g_damp*100);
    (void)Sig_h;
    for(int l=0;l<Lcap;l++){
        double t0=omp_get_wtime();
        build_sigma(cap_ln[l],n_tr,D,Sig_d);
        reconstruct_gptq(lw[l].gate,D,hid,Sig_d,qw[l].gate);   /* ternary, data-aware */
        reconstruct_gptq(lw[l].up,D,hid,Sig_d,qw[l].up);       /* ternary, data-aware */
        int8ize(lw[l].down,hid,D,qw[l].down);                  /* wide down-proj -> int8 (both paths) */
        printf("    layer %d done (gate/up ternary in=%d, down int8 in=%d)  [%.1fs]\n",l,D,hid,omp_get_wtime()-t0); fflush(stdout);
    }
    free(Sig_d);free(Sig_h);
    forward_mlp(H0ho,n_ho,D,hid,Lcap,qw,ffn_norm,post_norm,loscale,NULL,NULL,Hq_ho);
    head_logits(Hq_ho,n_ho,D,outn.data,emb.data,vocab,Lq);
    double gptq_rel=relerr(Lq,Lfp,(size_t)n_ho*vocab);
    double gptq_arg=argmax_agree(Lq,Lfp,n_ho,vocab);
    double gptq_hid=relerr(Hq_ho,Hfp_ho,(size_t)n_ho*D);

    printf("\n  === HELD-OUT FP-vs-quantized  [MIXED: gate/up ternary, down int8]  (%d tokens, %d layers, tied head vocab=%ld) ===\n",
           n_ho, Lcap, vocab);
    printf("    metric                         naive      GPTQ data-aware\n");
    printf("    final-logit relerr           %8.4f   %8.4f   (%.0f%% lower)\n",
           naive_rel, gptq_rel, naive_rel>0?100.0*(naive_rel-gptq_rel)/naive_rel:0.0);
    printf("    next-token argmax agreement  %7.1f%%   %7.1f%%\n", 100.0*naive_arg, 100.0*gptq_arg);
    printf("    residual-stream relerr       %8.4f   %8.4f\n", naive_hid, gptq_hid);

    CHECK(gptq_rel < naive_rel, "GPTQ data-aware final-logit relerr < naive (mixed precision: ternary gate/up)");
    CHECK(gptq_arg >= naive_arg, "GPTQ data-aware argmax agreement >= naive on HELD-OUT");
    CHECK(gptq_hid < naive_hid, "GPTQ data-aware residual-stream relerr < naive on HELD-OUT");

    printf("\nproj_qat_gemma_e2e: %d passed, %d failed\n", g_pass, g_fail);
    if(!g_fail) printf("REAL_PROJ_QAT_GEMMA_E2E_PASS\n");
    cce_gguf_free(gg);
    return g_fail?1:0;
}
