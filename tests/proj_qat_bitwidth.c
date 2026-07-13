/* proj_qat_bitwidth — component-dependent bit-width POLICY sweep on the real gemma
 * MLP stack. Formalizes the insight the mixed-precision e2e (proj_qat_gemma_e2e)
 * found by hand and that Colibri (GLM-5.2, 744B MoE) runs in production: NOT every
 * projection wants the same precision — the wide / sensitive ones (down-proj, heads,
 * routers) need more bits; the well-conditioned ones (gate/up) tolerate aggressive
 * ternary. Colibri forces its MTP head to int8 (int4 -> 0% draft acceptance) and
 * keeps dense components int4; we do the analogous per-component sweep here.
 *
 * Sweeps a set of (gate_bits, up_bits, down_bits) policies over the REAL gemma4 FFN
 * weights, reports each policy's held-out logit relerr + next-token argmax agreement
 * vs FP against its average bit-budget, and finds the quality-per-bit sweet spot.
 * Quantization is NAIVE per bit-width (the sweep isolates the BIT-WIDTH axis; the
 * orthogonal data-aware win is measured in proj_qat_gemma_e2e). Fast: no Σ/Cholesky.
 *
 * Build: make proj_qat_bitwidth  (needs Models/gemma-4-12B-it-MTP-Q8_0.gguf)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_tensor.h"

static int g_pass=0,g_fail=0;
#define CHECK(c,msg) do{ if(c){g_pass++;printf("  ok   %s\n",(msg));} else {g_fail++;printf("  FAIL: %s\n",(msg));} }while(0)
static unsigned long long g_rng=0x9E3779B97F4A7C15ULL;
static unsigned rnd_u(void){ g_rng=g_rng*6364136223846793005ULL+1442695040888963407ULL; return (unsigned)(g_rng>>33); }
static float g_eps=1e-6f;

/* ---- naive per-output-column quantizers (effective weights as float) ---- */
static void q_fp(const float* W,int in,int out,float* E){ memcpy(E,W,(size_t)in*out*sizeof(float)); }
/* symmetric k-level: level in [-L,L], scale = max|w|/L. ternary L=1, int4 L=7, int8 L=127 */
static void q_level(const float* W,int in,int out,int L,float* E){
    #pragma omp parallel for schedule(static)
    for(int o=0;o<out;o++){ double mx=0; for(int i=0;i<in;i++){ double a=fabs((double)W[(size_t)i*out+o]); if(a>mx)mx=a; }
        float s=(float)(mx/(double)L);
        for(int i=0;i<in;i++){ float w=W[(size_t)i*out+o],r=0.0f;
            if(s>0){ r=roundf(w/s); if(r>L)r=(float)L; if(r<-L)r=(float)-L; } E[(size_t)i*out+o]=s*r; } }
}
/* ternary using per-row absmean scale (BitNet b1.58 convention, ~1.58 bit) */
static void q_ternary(const float* W,int in,int out,float* E){
    #pragma omp parallel for schedule(static)
    for(int o=0;o<out;o++){ double su=0; for(int i=0;i<in;i++) su+=fabs((double)W[(size_t)i*out+o]);
        float g=(float)(su/(in>0?in:1));
        for(int i=0;i<in;i++){ float w=W[(size_t)i*out+o],r=0.0f;
            if(g>0){ r=roundf(w/g); if(r>1)r=1; if(r<-1)r=-1; } E[(size_t)i*out+o]=g*r; } }
}
/* bits encoding: 1=>ternary, 4=>int4, 8=>int8, 32=>FP.
   Ternary bit-cost = CNET's ACHIEVED trit-packing: 5 trits/byte = 8/5 = 1.6 bit/weight
   (not the 1.585 information-theoretic log2(3) — 1.6 is what packs on disk). */
static double bits_of(int b){ return b==1?1.6:(double)b; }
static void quantize(const float* W,int in,int out,int b,float* E){
    if(b>=32) q_fp(W,in,out,E);
    else if(b==8) q_level(W,in,out,127,E);
    else if(b==4) q_level(W,in,out,7,E);
    else q_ternary(W,in,out,E);
}

/* ---- gemma MLP forward (copied from proj_qat_gemma_e2e, faithful) ---- */
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
static void rmsnorm_w(const float* X,int n,int d,const float* w,float* Y){
    #pragma omp parallel for schedule(static)
    for(int r=0;r<n;r++){ const float* xr=X+(size_t)r*d; float* yr=Y+(size_t)r*d;
        double ss=0; for(int i=0;i<d;i++) ss+=(double)xr[i]*xr[i]; float inv=(float)(1.0/sqrt(ss/d+g_eps));
        for(int i=0;i<d;i++) yr[i]=(xr[i]*inv)*w[i]; }
}
static void gelu_tanh_mul(const float* G,const float* U,int n,int hid,float* A){
    const float c0=0.79788456080286535588f,c1=0.044715f;
    #pragma omp parallel for schedule(static)
    for(size_t k=0;k<(size_t)n*hid;k++){ float x=G[k]; A[k]=(0.5f*x*(1.0f+tanhf(c0*x*(1.0f+c1*x*x))))*U[k]; }
}
typedef struct { float *gate,*up,*down; } layer_w;
static void forward_mlp(const float* H0,int n,int D,int hid,int L,layer_w* lw,
                        const float* ffn_norm,const float* post_norm,const float* loscale,float* Hout){
    float* h=malloc((size_t)n*D*sizeof(float)); memcpy(h,H0,(size_t)n*D*sizeof(float));
    float* ln=malloc((size_t)n*D*sizeof(float));
    float* G=malloc((size_t)n*hid*sizeof(float)),*Uv=malloc((size_t)n*hid*sizeof(float)),*A=malloc((size_t)n*hid*sizeof(float));
    float* dn=malloc((size_t)n*D*sizeof(float)),*dnn=malloc((size_t)n*D*sizeof(float));
    for(int l=0;l<L;l++){
        rmsnorm_w(h,n,D,ffn_norm+(size_t)l*D,ln);
        matmul(ln,lw[l].gate,n,D,hid,G); matmul(ln,lw[l].up,n,D,hid,Uv);
        gelu_tanh_mul(G,Uv,n,hid,A);
        matmul(A,lw[l].down,n,hid,D,dn);
        rmsnorm_w(dn,n,D,post_norm+(size_t)l*D,dnn);
        float sc=loscale[l];
        for(size_t k=0;k<(size_t)n*D;k++) h[k]=(h[k]+dnn[k])*sc;
    }
    memcpy(Hout,h,(size_t)n*D*sizeof(float));
    free(h);free(ln);free(G);free(Uv);free(A);free(dn);free(dnn);
}
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
        long aq=0,af=0; for(long v=1;v<vocab;v++){ if(q[v]>q[aq])aq=v; if(f[v]>f[af])af=v; } if(aq==af) agree++; }
    return (double)agree/n;
}

int main(int argc,char** argv){
    setvbuf(stdout,NULL,_IOLBF,0);
    const char* path=(argc>1)?argv[1]:"Models/gemma-4-12B-it-MTP-Q8_0.gguf";
    int n_ho=(argc>2)?atoi(argv[2]):512;
    int Lcap=(argc>3)?atoi(argv[3]):4;

    cce_gguf* gg=NULL;
    if(cce_gguf_load(path,&gg)!=CCE_OK||!gg){ printf("FAIL: cannot load %s\n",path); return 1; }
    const char* arch=cce_gguf_get_arch(gg);
    int D=cce_gguf_get_hidden_size(gg); float eps=cce_gguf_get_rms_eps(gg); if(eps>0)g_eps=eps;
    int hid=cce_gguf_get_feed_forward_length(gg);
    int nlayer=cce_gguf_get_n_layer(gg); if(Lcap>nlayer)Lcap=nlayer;
    float embed_scale=(arch&&strncmp(arch,"gemma",5)==0)?sqrtf((float)D):1.0f;
    printf("proj_qat_bitwidth: %s\n  arch=%s D=%d ffn=%d layers=%d(run %d)  held-out=%d\n",
           path, arch?arch:"?", D, hid, nlayer, Lcap, n_ho);

    cce_tensor emb={0},outn={0};
    if(cce_gguf_load_tensor_by_name(gg,"token_embd.weight",&emb)!=CCE_OK){printf("FAIL token_embd\n");return 1;}
    if(cce_gguf_load_tensor_by_name(gg,"output_norm.weight",&outn)!=CCE_OK){printf("FAIL output_norm\n");return 1;}
    long vocab=(long)(emb.numel/(size_t)D);

    layer_w* lw=calloc(Lcap,sizeof(layer_w));
    float* ffn_norm=malloc((size_t)Lcap*D*sizeof(float)),*post_norm=malloc((size_t)Lcap*D*sizeof(float)),*loscale=malloc((size_t)Lcap*sizeof(float));
    for(int l=0;l<Lcap;l++){ char nm[128]; cce_tensor g={0},u={0},d={0},fn={0},pn={0},ls={0};
        snprintf(nm,sizeof nm,"blk.%d.ffn_gate.weight",l); if(cce_gguf_load_tensor_by_name(gg,nm,&g)!=CCE_OK){printf("FAIL %s\n",nm);return 1;}
        snprintf(nm,sizeof nm,"blk.%d.ffn_up.weight",l);   if(cce_gguf_load_tensor_by_name(gg,nm,&u)!=CCE_OK){printf("FAIL %s\n",nm);return 1;}
        snprintf(nm,sizeof nm,"blk.%d.ffn_down.weight",l); if(cce_gguf_load_tensor_by_name(gg,nm,&d)!=CCE_OK){printf("FAIL %s\n",nm);return 1;}
        snprintf(nm,sizeof nm,"blk.%d.ffn_norm.weight",l); if(cce_gguf_load_tensor_by_name(gg,nm,&fn)!=CCE_OK){printf("FAIL %s\n",nm);return 1;}
        snprintf(nm,sizeof nm,"blk.%d.post_ffw_norm.weight",l); if(cce_gguf_load_tensor_by_name(gg,nm,&pn)!=CCE_OK){printf("FAIL %s\n",nm);return 1;}
        snprintf(nm,sizeof nm,"blk.%d.layer_output_scale.weight",l);
        loscale[l]=(cce_gguf_load_tensor_by_name(gg,nm,&ls)==CCE_OK&&ls.numel>0)?ls.data[0]:1.0f;
        lw[l].gate=malloc((size_t)D*hid*sizeof(float)); lw[l].up=malloc((size_t)D*hid*sizeof(float)); lw[l].down=malloc((size_t)hid*D*sizeof(float));
        for(int o=0;o<hid;o++){ const float* go=g.data+(size_t)o*D; const float* uo=u.data+(size_t)o*D;
            for(int i=0;i<D;i++){ lw[l].gate[(size_t)i*hid+o]=go[i]; lw[l].up[(size_t)i*hid+o]=uo[i]; } }
        for(int o=0;o<D;o++){ const float* db=d.data+(size_t)o*hid; for(int i=0;i<hid;i++) lw[l].down[(size_t)i*D+o]=db[i]; }
        memcpy(ffn_norm+(size_t)l*D,fn.data,(size_t)D*sizeof(float)); memcpy(post_norm+(size_t)l*D,pn.data,(size_t)D*sizeof(float));
        cce_tensor_free(&g);cce_tensor_free(&u);cce_tensor_free(&d);cce_tensor_free(&fn);cce_tensor_free(&pn);cce_tensor_free(&ls);
    }
    printf("  layer_output_scale= "); for(int l=0;l<Lcap;l++) printf("%.3f ",loscale[l]); printf("\n");

    /* held-out inputs + FP reference logits */
    float* H0=malloc((size_t)n_ho*D*sizeof(float));
    for(int t=0;t<n_ho;t++){ long tok=(long)(rnd_u()%(unsigned)vocab); const float* er=emb.data+(size_t)tok*D;
        float* h=H0+(size_t)t*D; for(int i=0;i<D;i++) h[i]=er[i]*embed_scale; }
    float* Hfp=malloc((size_t)n_ho*D*sizeof(float)); float* Lfp=malloc((size_t)n_ho*vocab*sizeof(float));
    forward_mlp(H0,n_ho,D,hid,Lcap,lw,ffn_norm,post_norm,loscale,Hfp);
    head_logits(Hfp,n_ho,D,outn.data,emb.data,vocab,Lfp);

    /* quantized weight bank + logits buffer, reused per policy */
    layer_w* qw=calloc(Lcap,sizeof(layer_w));
    for(int l=0;l<Lcap;l++){ qw[l].gate=malloc((size_t)D*hid*sizeof(float)); qw[l].up=malloc((size_t)D*hid*sizeof(float)); qw[l].down=malloc((size_t)hid*D*sizeof(float)); }
    float* Hq=malloc((size_t)n_ho*D*sizeof(float)); float* Lq=malloc((size_t)n_ho*vocab*sizeof(float));

    struct { const char* name; int gb,ub,db; } pol[] = {
        {"FP (baseline)",        32,32,32},
        {"all int8",              8, 8, 8},
        {"all int4",              4, 4, 4},
        {"all ternary",           1, 1, 1},
        {"int4 g/u, int8 down",   4, 4, 8},
        {"tern g/u, int8 down",   1, 1, 8},   /* the M4 mixed-precision win */
        {"tern g/u, int4 down",   1, 1, 4},
    };
    int npol=(int)(sizeof(pol)/sizeof(pol[0]));

    printf("\n  === bit-width policy sweep (real gemma FFN, held-out vs FP) ===\n");
    printf("    %-22s  %6s  %7s  %10s  %8s\n","policy (gate/up/down)","bits/w","x smaller","logit relerr","argmax");
    double bw[16],rel[16],arg[16];
    for(int p=0;p<npol;p++){
        for(int l=0;l<Lcap;l++){ quantize(lw[l].gate,D,hid,pol[p].gb,qw[l].gate);
            quantize(lw[l].up,D,hid,pol[p].ub,qw[l].up); quantize(lw[l].down,hid,D,pol[p].db,qw[l].down); }
        forward_mlp(H0,n_ho,D,hid,Lcap,qw,ffn_norm,post_norm,loscale,Hq);
        head_logits(Hq,n_ho,D,outn.data,emb.data,vocab,Lq);
        double b=(bits_of(pol[p].gb)+bits_of(pol[p].ub)+bits_of(pol[p].db))/3.0;
        bw[p]=b; rel[p]=relerr(Lq,Lfp,(size_t)n_ho*vocab); arg[p]=argmax_agree(Lq,Lfp,n_ho,vocab);
        printf("    %-22s  %6.2f  %7.1fx  %10.4f  %7.1f%%\n", pol[p].name, b, 32.0/b, rel[p], 100.0*arg[p]);
    }

    /* Colibri's insight, shown where it is CLEAN — at int4, where the model still
       has structure to protect (naive ternary is dead at any policy). */
    printf("\n  Colibri's insight (protect the wide/sensitive down-proj at higher bits):\n");
    int i_int4=2, i_mix4=4;   /* all-int4  vs  int4-g/u + int8-down */
    printf("    all int4             : %.2f bits/w, relerr %.4f, argmax %.1f%%\n", bw[i_int4], rel[i_int4], 100*arg[i_int4]);
    printf("    int4 g/u + int8 down : %.2f bits/w, relerr %.4f, argmax %.1f%%  (down-proj protected)\n", bw[i_mix4], rel[i_mix4], 100*arg[i_mix4]);
    printf("    -> +%.2f bits/w cuts logit error %.0f%% and lifts argmax %.1f pts — the component-dependent win\n",
           bw[i_mix4]-bw[i_int4], rel[i_int4]>0?100*(rel[i_int4]-rel[i_mix4])/rel[i_int4]:0, 100*(arg[i_mix4]-arg[i_int4]));
    printf("  NOTE: NAIVE ternary is catastrophic at any policy (relerr ~1.0, argmax ~1%%) — 1.58-bit needs the\n");
    printf("        orthogonal DATA-AWARE axis (proj_qat_gemma_e2e: data-aware tern-g/u+int8-down = 0.92 relerr / 6.1%% argmax).\n");
    printf("  TAKEAWAY: int8 near-lossless (4x); int4 the aggressive-usable tier (~8x); protecting the wide\n");
    printf("            down-proj is a clean Pareto step; sub-int4 (ternary) requires data-aware reconstruction.\n");

    CHECK(rel[0] < 1e-4, "FP baseline reconstructs exactly (sanity)");
    CHECK(rel[1] < 0.1, "all-int8 is near-lossless");
    CHECK(rel[i_mix4] < rel[i_int4], "protecting the down-proj (int8) beats all-int4 — component-dependent bit-width wins");
    CHECK(arg[i_mix4] > arg[i_int4], "and it lifts next-token argmax agreement at modest extra bits");
    CHECK(rel[3] > 0.9 && rel[1] < 0.1, "quality/bit curve is real: int8 near-lossless, naive ternary collapses");

    printf("\nproj_qat_bitwidth: %d passed, %d failed\n", g_pass, g_fail);
    for(int l=0;l<Lcap;l++){ free(lw[l].gate);free(lw[l].up);free(lw[l].down);free(qw[l].gate);free(qw[l].up);free(qw[l].down); }
    free(lw);free(qw);free(ffn_norm);free(post_norm);free(loscale);free(H0);free(Hfp);free(Lfp);free(Hq);free(Lq);
    cce_tensor_free(&emb);cce_tensor_free(&outn); cce_gguf_free(gg);
    return g_fail?1:0;
}
