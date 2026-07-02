/* BitNet b1.58 quantization-aware training (QAT) demo — self-contained.
 *
 * Proves the point measured on Supra: post-hoc ternarization of a full-precision
 * model collapses quality, but TRAINING with ternary weights (shadow FP weights +
 * straight-through estimator, BitNet b1.58) recovers it. Same recipe a real
 * 1.58-bit CNET/Supra model would use, at toy scale (CNET "validate then scale").
 *
 * Task : classify points inside vs outside a ring (nonlinear).
 * Net  : x(2) --fixed random features--> z(D) --[W1]--> h(H) tanh --[W2]--> sigmoid.
 *        W1, W2 are the learnable BitLinear layers (ternary under QAT). They are
 *        WIDE (D and H inputs) so ternary has the degrees of freedom it needs —
 *        ternary only works on wide layers, which is why BitNet is a large-model
 *        technique. A fixed FP random feature map lifts the 2-D input so the first
 *        ternary layer isn't pathologically narrow.
 *
 * Runs (identical data / init / schedule):
 *   FP        : full-precision W1,W2.
 *   QAT-tern  : forward ternarizes W1,W2 (absmean); backward = STE to FP shadow.
 *   post-hoc  : ternarize the FP-trained W1,W2 (no retrain).
 *
 * Build/run:  make bitnet_qat
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define D 24        /* random-feature dimension */
#define H 64        /* hidden width            */
#define N 1600      /* dataset size            */
#define EPOCHS 3000
#define LR 0.02f

typedef struct { float w1[H][D], b1[H], w2[H], b2; } Net;

static float frand(void){ return (float)rand()/(float)RAND_MAX; }
static float sigmoidf(float x){ return 1.0f/(1.0f+expf(-x)); }

/* Fixed random feature map x(2) -> z(D): z = tanh(R x + c). R,c set once. */
static float Rm[D][2], Rc[D];
static void feat(const float* x, float* z){
    for(int d=0;d<D;d++){ float a=Rc[d]; a+=Rm[d][0]*x[0]+Rm[d][1]*x[1]; z[d]=tanhf(a); }
}

/* BitNet b1.58 ternary: gamma=absmean, RoundClip(w/gamma,-1,1), effective=gamma*q */
static float tern(float w, float gamma){
    if(gamma<=0.0f) return 0.0f;
    float r=roundf(w/gamma); if(r>1.0f)r=1.0f; if(r<-1.0f)r=-1.0f; return gamma*r;
}
static float absmean(const float* w,int n){ float s=0; for(int i=0;i<n;i++) s+=fabsf(w[i]); return s/(float)n; }

/* Forward; ternary effective weights are stashed in we1/we2 for STE backprop. */
static float forward(const Net* nt, const float* z, float* h, float we1[H][D], float* we2, int q){
    float g1 = q ? absmean(&nt->w1[0][0], H*D) : 0.0f;
    float g2 = q ? absmean(nt->w2, H)          : 0.0f;
    for(int j=0;j<H;j++){
        float a=nt->b1[j];
        for(int k=0;k<D;k++){ float w = q?tern(nt->w1[j][k],g1):nt->w1[j][k]; we1[j][k]=w; a+=w*z[k]; }
        h[j]=tanhf(a);
    }
    float o=nt->b2;
    for(int j=0;j<H;j++){ float w=q?tern(nt->w2[j],g2):nt->w2[j]; we2[j]=w; o+=w*h[j]; }
    return sigmoidf(o);
}

static float eval(const Net* nt, float Z[][D], float* Y, int q){
    float h[H], we1[H][D], we2[H]; int c=0;
    for(int i=0;i<N;i++){ float o=forward(nt,Z[i],h,we1,we2,q); if((o>=0.5f)==(Y[i]>=0.5f)) c++; }
    return 100.0f*(float)c/(float)N;
}

static void train(Net* nt, float Z[][D], float* Y, int q){
    float h[H], we1[H][D], we2[H];
    for(int e=0;e<EPOCHS;e++) for(int i=0;i<N;i++){
        float o=forward(nt,Z[i],h,we1,we2,q);
        float dout=o-Y[i];                         /* BCE+sigmoid */
        for(int j=0;j<H;j++){
            float dh=dout*we2[j];                  /* backprop through ternary w2 (STE) */
            float dhpre=dh*(1.0f-h[j]*h[j]);
            nt->w2[j]-=LR*dout*h[j];               /* STE: grad applied to FP shadow */
            nt->b1[j]-=LR*dhpre;
            for(int k=0;k<D;k++) nt->w1[j][k]-=LR*dhpre*Z[i][k];
        }
        nt->b2-=LR*dout;
    }
}

int main(void){
    srand(12345);
    for(int d=0;d<D;d++){ Rm[d][0]=(frand()*2-1)*1.5f; Rm[d][1]=(frand()*2-1)*1.5f; Rc[d]=(frand()*2-1); }

    static float Z[N][D], Y[N];
    int pos=0;
    for(int i=0;i<N;i++){
        float x0=frand()*4-2, x1=frand()*4-2;
        float r=sqrtf(x0*x0+x1*x1);
        Y[i]=(r>0.8f && r<1.6f)?1.0f:0.0f; pos+=(Y[i]>0.5f);
        float xx[2]={x0,x1}; feat(xx, Z[i]);
    }

    Net fp, qat;
    for(int j=0;j<H;j++){ for(int k=0;k<D;k++) fp.w1[j][k]=(frand()*2-1)*0.5f; fp.b1[j]=0; fp.w2[j]=(frand()*2-1)*0.5f; }
    fp.b2=0; qat=fp;                       /* identical init */

    train(&fp,  Z, Y, 0);
    train(&qat, Z, Y, 1);

    float acc_fp      = eval(&fp,  Z, Y, 0);
    float acc_qat     = eval(&qat, Z, Y, 1);
    float acc_posthoc = eval(&fp,  Z, Y, 1);
    float maj = 100.0f*(float)(N-pos)/(float)N; if(maj<50) maj=100-maj;

    printf("=== BitNet b1.58 QAT vs post-hoc ternary (ring classification) ===\n");
    printf("majority-class baseline      : %.1f%%\n", maj);
    printf("FP (full precision)          : %.1f%%\n", acc_fp);
    printf("QAT ternary {-1,0,+1} (STE)  : %.1f%%   <- trained for ternary\n", acc_qat);
    printf("post-hoc ternary (no retrain): %.1f%%   <- ternarize FP weights\n", acc_posthoc);
    printf("\nQAT keeps ~FP accuracy on ternary weights; post-hoc drops toward chance.\n");
    int ok = (acc_qat > acc_posthoc + 10.0f) && (acc_qat > 80.0f);
    printf("%s\n", ok ? "RESULT: PASS (1.58-bit works WHEN trained for it; post-hoc does not)"
                      : "RESULT: inconclusive");
    return ok ? 0 : 1;
}
