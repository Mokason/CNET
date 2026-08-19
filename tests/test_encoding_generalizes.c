/* The encoding decides whether a unit can learn the rule or only memorise it.
 *
 * adapt_new_domain measured extrapolated=0 everywhere, and coverage-as-rule-domain
 * (CNET_COVERAGE_GENERALIZE=1) then showed WHY: with a one-hot pair input the mined
 * student predicted withheld rows at chance (0/3, 0/4, 2/3, 1/4) even on addition.
 * One-hot gives the substrate no pressure to find (a+b)%4 -- memorising 9 rows fits
 * perfectly, and every input activates a disjoint pair of units so nothing constrains
 * the combinations it never saw.
 *
 * A positional binary encoding shares weight across inputs, and the rule becomes
 * learnable. Measured, deterministic over repeated runs:
 *
 *     encoding      addmod   xor   affine   max   random
 *     onehot(8d)     memo    memo   memo    memo   memo
 *     binMSB(4d)     memo    memo   LEARNED LEARNED memo
 *     binLSB(4d)    LEARNED  memo   memo    LEARNED memo
 *
 * What this gate pins:
 *   1. one-hot generalises NOTHING (the baseline that motivated the change)
 *   2. binary generalises at least two structured families (the fix works)
 *   3. the STRUCTURELESS CONTROL never generalises, under ANY encoding -- this is
 *      the safety bar. A better encoding must not become a licence to invent; if
 *      random ever reads LEARNED, the proof is broken and this gate must fail.
 *
 * "LEARNED" means the unit EARNED domain coverage: trained on a subset, then
 * predicted every withheld row, so hybrid_coverage_admits grants an input it was
 * never taught. "memo" means it stayed membership-gated.
 *
 * Scale limit: 4x4 -> 4 nibble domains. Says nothing beyond that. Broader
 * competence WITHHELD.
 *
 * make encoding_generalizes -> ENCODING_GENERALIZES_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/personal_ai.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/base.h"

#define FW 4
#define FC 2
#define OUT_DIM 4
#define DOMAIN 16

typedef enum { ENC_ONEHOT=0, ENC_BINARY, ENC_BINLSB, ENC_COUNT } Enc;
static const char *ENC_NAME[ENC_COUNT]={"onehot(8d)","binMSB(4d)","binLSB(4d)"};
static const int ENC_DIM[ENC_COUNT]={FW*FC, 4, 4};
static const int ENC_FAM[ENC_COUNT]={PORT_ONEHOT, PORT_BINARY_MSB, PORT_BINARY_LSB};
static const int ENC_W[ENC_COUNT]={FW,2,2}, ENC_C[ENC_COUNT]={FC,2,2};

typedef enum { F_ADD=0,F_XOR,F_AFF,F_MAX,F_RND,F_COUNT } Fam;
static const char *F_NAME[F_COUNT]={"addmod","xor","affine","max","random"};
static const unsigned RND[DOMAIN]={2,0,3,1,1,3,0,2,3,2,1,0,0,1,2,3};
static unsigned ans(Fam f,unsigned a,unsigned b){
  switch(f){case F_ADD:return (a+b)%OUT_DIM;case F_XOR:return (a^b)%OUT_DIM;
  case F_AFF:return (3u*a+b+1u)%OUT_DIM;case F_MAX:return a>b?a:b;
  default:return RND[(a*FW+b)%DOMAIN];} }

typedef struct { size_t fw,fc,out_dim; Fam fam; Enc enc; } Ctx;

static void encode(Enc e,double*in,unsigned a,unsigned b){
  int i,d=ENC_DIM[e];
  for(i=0;i<d;i++) in[i]=0.0;
  if(e==ENC_ONEHOT){ in[a]=1.0; in[FW+b]=1.0; }
  else if(e==ENC_BINARY){ in[0]=(a>>1)&1; in[1]=a&1; in[2]=(b>>1)&1; in[3]=b&1; }
  else { in[0]=a&1; in[1]=(a>>1)&1; in[2]=b&1; in[3]=(b>>1)&1; }
}
static void decode(Enc e,const double*in,unsigned*a,unsigned*b){
  if(e==ENC_ONEHOT){ size_t i,ha=0,hb=0;
    for(i=1;i<FW;i++){ if(in[i]>in[ha])ha=i; if(in[FW+i]>in[FW+hb])hb=i; }
    *a=(unsigned)ha; *b=(unsigned)hb;
  } else if(e==ENC_BINARY){ *a=(unsigned)((in[0]>0.5?2:0)+(in[1]>0.5?1:0));
           *b=(unsigned)((in[2]>0.5?2:0)+(in[3]>0.5?1:0)); }
  else { *a=(unsigned)((in[1]>0.5?2:0)+(in[0]>0.5?1:0));
         *b=(unsigned)((in[3]>0.5?2:0)+(in[2]>0.5?1:0)); }
}
static int res(const double*in,double*out,void*c_){
  Ctx*c=(Ctx*)c_; unsigned a,b,i;
  decode(c->enc,in,&a,&b);
  for(i=0;i<OUT_DIM;i++) out[i]=0.0;
  out[ans(c->fam,a,b)%OUT_DIM]=1.0; return 0;
}
static Port PMF(const char*t,int fam,size_t w,size_t c){
  Port p; memset(&p,0,sizeof p); p.family=(PortFamily)fam;
  p.field_width=w; p.field_count=c; snprintf(p.tag,sizeof p.tag,"%s",t); return p;
}
static int heldout(size_t i){return i==3||i==6||i==9||i==12;}

static int learned[ENC_COUNT][F_COUNT];
static int checks, failures;
static void check(int ok,const char*msg,const char*det){
  checks++; printf("  %-52s %-26s %s\n",msg,det?det:"",ok?"PASS":"FAIL");
  if(!ok) failures++;
}

int main(void){
  int e,f;
  setenv("CNET_COVERAGE_GENERALIZE","1",1);
  printf("%-12s", "encoding");
  for(f=0;f<F_COUNT;f++) printf("%9s",F_NAME[f]);
  printf("\n");
  for(e=0;e<ENC_COUNT;e++){
    printf("%-12s",ENC_NAME[e]);
    for(f=0;f<F_COUNT;f++){
      PersonalAi ai; PersonalAiPolicy pol; PersonalAiReport rep; Ctx ctx;
      BinaryTransformNetwork*stu=NULL;
      Port pin=PMF("e_in",ENC_FAM[e],ENC_W[e],ENC_C[e]), pout=PMF("e_out",PORT_ONEHOT,OUT_DIM,1);
      double in[16],out[OUT_DIM]; char base[128],led[128]; size_t idx; int pass=0;
      ctx.fw=FW;ctx.fc=FC;ctx.out_dim=OUT_DIM;ctx.fam=(Fam)f;ctx.enc=(Enc)e;
      snprintf(base,sizeof base,"/tmp/enc_%d_%d.cnb",e,f);
      snprintf(led,sizeof led,"/tmp/enc_%d_%d.gaps.txt",e,f);
      remove(base); remove(led);
      personal_ai_policy_defaults(&pol);
      pol.allow_teacher=0;pol.allow_soft=0;pol.allow_residual=1;
      pol.structure_mine_on_serve=0;pol.structure_min_hits=2;
      if(personal_ai_open(&ai,base,led,NULL,&pol)!=0){printf("%9s","open!");continue;}
      ai.lane.acq.min_evidence=1;
      personal_ai_bind_residual(&ai,"e",res,&ctx);
      for(int p2=0;p2<2;p2++)
        for(idx=0;idx<DOMAIN;idx++){
          if(heldout(idx))continue;
          encode((Enc)e,in,(unsigned)(idx/FW),(unsigned)(idx%FW));
          memset(&rep,0,sizeof rep);
          personal_ai_serve(&ai,pin,pout,in,(size_t)ENC_DIM[e],out,OUT_DIM,&rep);
        }
      personal_ai_structure_mine(&ai,&stu);
      /* did it EARN domain coverage? probe a withheld input */
      encode((Enc)e,in,0,3);
      pass=hybrid_coverage_admits(personal_ai_hybrid(&ai),pin,pout,in,(size_t)ENC_DIM[e]);
      printf("%9s",pass?"LEARNED":"memo");
      learned[e][f] = pass ? 1 : 0;
      personal_ai_close(&ai); remove(base); remove(led);
    }
    printf("\n");
  }

  printf("\n");
  {
    char det[96]; int oh=0, bin=0, rnd=0, i;
    for(f=0;f<F_COUNT;f++) if(learned[ENC_ONEHOT][f]) oh++;
    snprintf(det,sizeof det,"onehot generalised %d families",oh);
    check(oh==0,"one-hot memorises: generalises nothing",det);

    for(e=ENC_BINARY;e<ENC_COUNT;e++)
      for(f=0;f<F_COUNT;f++)
        if(f!=F_RND && learned[e][f]) bin++;
    snprintf(det,sizeof det,"%d (encoding,family) cells LEARNED",bin);
    check(bin>=2,"binary encoding earns domain coverage",det);

    for(e=0;e<ENC_COUNT;e++) if(learned[e][F_RND]) rnd++;
    snprintf(det,sizeof det,"random LEARNED under %d/%d encodings",rnd,(int)ENC_COUNT);
    check(rnd==0,"SAFETY: structureless control never generalises",det);

    for(i=0;i<1;i++){ (void)i; }
    printf("\nchecks=%d fails=%d\n",checks,failures);
    if(failures==0){
      printf("ENCODING_GENERALIZES_PASS checks=%d fails=0 onehot_learned=%d "
             "binary_learned=%d control_learned=0 scale=nibble16 "
             "broader_claims=WITHHELD\n",checks,oh,bin);
      return 0;
    }
    printf("ENCODING_GENERALIZES_RED checks=%d fails=%d\n",checks,failures);
    return 1;
  }
}
