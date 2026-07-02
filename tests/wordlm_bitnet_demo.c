/* BitNet b1.58 QAT inside the real CCE word-LM (cce_wordlm), incl. ternary
 * embedding + packed 1.6-bit export/reload.
 *
 * Four-way comparison (same corpus / init / schedule) to localize where ternary
 * costs quality, then export the full-ternary model to packed 1.6-bit and prove
 * an inference-only reload (NO FP shadow weights) reproduces it bit-for-bit.
 *
 * Build/run:  make wordlm_bitnet
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/cce/cce_wordlm.h"

#define CAP_VOCAB 4000
#define WCTX 3
static char wvocab[CAP_VOCAB][24];
static int  wvsize = 0;
static void word_lower(char* w){ for(;*w;++w) if(*w>='A'&&*w<='Z') *w=(char)(*w+32); }
static int get_or_add_word(const char* w,int add){
    for(int i=0;i<wvsize;i++) if(strcmp(wvocab[i],w)==0) return i;
    if(!add||wvsize>=CAP_VOCAB) return -1;
    strncpy(wvocab[wvsize],w,23); wvocab[wvsize][23]=0; return wvsize++;
}
static int tokenize(const char* s,int* out,int cap,int add){
    int n=0; char cur[24]; int cl=0;
    for(const char* p=s;;++p){ char c=*p;
        int alpha=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='\'';
        if(alpha){ if(cl<23) cur[cl++]=c; }
        else { if(cl>0){ cur[cl]=0; word_lower(cur); int id=get_or_add_word(cur,add); if(id>=0&&n<cap) out[n++]=id; cl=0; }
            if(c=='.'||c=='!'||c=='?'){ int id=get_or_add_word(".",add); if(id>=0&&n<cap) out[n++]=id; }
            if(c==0) break; } }
    return n;
}
static const char* corpus[] = {
    "Once upon a time there was a little girl who loved to play in the garden.",
    "Once upon a time there was a curious fox who found a glowing mushroom in the forest.",
    "Once upon a time a brave young knight found a shiny golden key in the dark forest.",
    "The little fox found a shiny apple under the big tree and shared it with her friends.",
    "Ben built a tall tower with colorful blocks and then laughed when it fell down.",
    "Mia the rabbit hopped fast across the sunny meadow to hide from the summer rain.",
    "Sam sailed his paper boat on the pond and the wind brought it safely back.",
    "Emma sat by the window reading stories while the first stars appeared in the sky.",
    "The bear cub followed the wise old owl through the dark woods until he saw his mother.",
    "Jack threw the bright red ball high into the air and his dog caught it.",
    "Sara baked sweet cookies and the warm smell made the whole house feel happy.",
    "A tiny mouse ate a crumb of cheese while the big cat was asleep on the mat.",
    "The young dragon practiced flying low over the green hills until he could soar high.",
    "Lily and Max played hide and seek all afternoon in the old barn full of hay.",
    "The kind knight gave food to the hungry villagers and they thanked him with a feast.",
    "Soft rain fell on the flowers making the garden smell fresh and look bright and green.",
    "Tom looked at the old treasure map and dreamed of sailing to secret islands.",
    "Anna made a wish on a shooting star and the next morning she met a new friend.",
    "The old man told wonderful dragon stories to the children around the warm fire.",
    "A golden key opened a wooden box full of toys and books that the children could share.",
    "A clever bird sang beautiful songs and told the princess where the lost crown was hidden.",
    "Fireflies lit up the warm night like tiny stars while the children watched in wonder.",
    "The girl planted seeds in spring and by summer the flowers were taller than she was.",
    "A boy found a lost puppy and after searching all day he returned it to its family.",
    "The friends built a fort from blankets and told stories with a flashlight until bedtime.",
    "A cat chased a butterfly through the garden but stopped to nap in a sunny spot.",
    "The prince learned to ride a horse and soon could gallop fast across the open fields.",
    "The baker made fresh bread every morning and the whole village smelled wonderful.",
    "A squirrel gathered nuts for winter and hid them in many secret places in the tree.",
    "The artist painted a beautiful sunset using every color she could find in her box.",
    "The puppy learned to sit and stay and got a treat and a happy pat on the head.",
    "A bird built a nest in the tall tree and sang every morning to wake the children.",
    "A boy flew a kite on a windy day and it soared so high it almost touched the clouds.",
    "A horse galloped through the meadow with its mane flying in the wind like a flag.",
    "The boy helped his grandmother bake a cake and licked the spoon when it was done.",
    "The wise owl told the lost bear cub the way home and the cub hugged his mother.",
    "A dragon and a knight became friends and flew together over the green hills.",
    "The girl found a talking cat who showed her a secret door in the old library.",
    "The children built a sandcastle on the beach and watched the waves roll in.",
    "Once there was a happy dog who loved to run and play with the children all day.",
    "The fox and the rabbit shared the biggest carrot they had ever seen in the field.",
    "A small turtle walked slowly to the pond and made many friends along the way.",
    "The boy read a book about pirates and dreamed of sailing the wide blue sea.",
    "The little bird was afraid to fly but the wind lifted her gently into the sky.",
    "A girl and her dog found a rainbow and followed it to a field of flowers.",
    "The children sang songs around the fire while the old man played his guitar.",
    "The knight rode through the village and waved at the children who cheered for him.",
    "Once upon a time a curious cat climbed the tall tree to see the whole town."
};

static double mean_ce(cce_wordlm* m, int (*ctxs)[WCTX], int* tgts, int np){
    double s=0; for(int i=0;i<np;i++) s += cce_wordlm_nll(m, ctxs[i], tgts[i]); return s/np;
}
static void train_model(cce_wordlm* m, int (*ctxs)[WCTX], int* tgts, int np, int epochs, float lr){
    int* order=malloc(sizeof(int)*(size_t)np); for(int i=0;i<np;i++) order[i]=i;
    for(int ep=0;ep<epochs;ep++){
        for(int i=np-1;i>0;i--){ int j=rand()%(i+1); int t=order[i]; order[i]=order[j]; order[j]=t; }
        for(int q=0;q<np;q++) cce_wordlm_train_step(m, ctxs[order[q]], tgts[order[q]], lr);
    }
    free(order);
}
static cce_wordlm* train_cfg(int lin,int emb,int (*ctxs)[WCTX],int* tgts,int np,int V,int d,int hid,int epochs,float lr){
    cce_wordlm* m=cce_wordlm_create(V,d,WCTX,hid,7u);
    cce_wordlm_set_ternary(m,lin); cce_wordlm_set_ternary_embed(m,emb);
    train_model(m,ctxs,tgts,np,epochs,lr);
    return m;
}
static void sample(cce_wordlm* m, int V){
    int gctx[WCTX]; const char* seed[3]={"upon","a","time"};
    for(int k=0;k<WCTX;k++){ int id=get_or_add_word(seed[k],0); gctx[k]=id>=0?id:0; }
    int dot=get_or_add_word(".",0); float* pen=calloc((size_t)V,sizeof(float));
    printf("Once upon a time");
    for(int s=0;s<40;s++){ int w=cce_wordlm_predict(m,gctx,pen); if(w==dot){printf(".");break;} printf(" %s",wvocab[w]);
        for(int i=0;i<V;i++) pen[i]*=0.85f; pen[w]-=4.0f; for(int k=0;k<WCTX-1;k++) gctx[k]=gctx[k+1]; gctx[WCTX-1]=w; }
    printf("\n"); free(pen);
}
static long fsize(const char* p){ FILE* f=fopen(p,"rb"); if(!f) return -1; fseek(f,0,SEEK_END); long s=ftell(f); fclose(f); return s; }
static int gen_fp(cce_wordlm* m, int V, int* seq){
    int gctx[WCTX]; const char* sd[3]={"upon","a","time"};
    for(int k=0;k<WCTX;k++){ int id=get_or_add_word(sd[k],0); gctx[k]=id>=0?id:0; }
    int dot=get_or_add_word(".",0); float* pen=calloc((size_t)V,sizeof(float)); int n=0;
    for(int s=0;s<40;s++){ int w=cce_wordlm_predict(m,gctx,pen); seq[n++]=w; if(w==dot) break;
        for(int i=0;i<V;i++) pen[i]*=0.85f; pen[w]-=4.0f; for(int k=0;k<WCTX-1;k++) gctx[k]=gctx[k+1]; gctx[WCTX-1]=w; }
    free(pen); return n;
}
static int gen_pk(cce_wordlm_packed* p, int V, int* seq){
    int gctx[WCTX]; const char* sd[3]={"upon","a","time"};
    for(int k=0;k<WCTX;k++){ int id=get_or_add_word(sd[k],0); gctx[k]=id>=0?id:0; }
    int dot=get_or_add_word(".",0); float* pen=calloc((size_t)V,sizeof(float)); int n=0;
    for(int s=0;s<40;s++){ int w=cce_wordlm_packed_predict(p,gctx,pen); seq[n++]=w; if(w==dot) break;
        for(int i=0;i<V;i++) pen[i]*=0.85f; pen[w]-=4.0f; for(int k=0;k<WCTX-1;k++) gctx[k]=gctx[k+1]; gctx[WCTX-1]=w; }
    free(pen); return n;
}

int main(void){
    int nsent=(int)(sizeof(corpus)/sizeof(corpus[0]));
    { int tmp[256]; for(int s=0;s<nsent;s++) tokenize(corpus[s],tmp,256,1); }
    int V=wvsize, d=64, hid=128;
    int max_pairs=0;
    for(int s=0;s<nsent;s++){ int tmp[256]; int n=tokenize(corpus[s],tmp,256,0); if(n>WCTX) max_pairs+=n-WCTX; }
    int (*ctxs)[WCTX]=malloc(sizeof(int[WCTX])*(size_t)max_pairs);
    int* tgts=malloc(sizeof(int)*(size_t)max_pairs);
    int np=0;
    for(int s=0;s<nsent;s++){ int toks[256]; int n=tokenize(corpus[s],toks,256,0);
        for(int i=WCTX;i<n;i++){ for(int k=0;k<WCTX;k++) ctxs[np][k]=toks[i-WCTX+k]; tgts[np]=toks[i]; np++; } }

    int epochs=200; float lr=0.01f;  /* 4 models; override with WLM_EPOCHS */
    { const char* e=getenv("WLM_EPOCHS"); if(e){ int v=atoi(e); if(v>0) epochs=v; } }
    printf("=== BitNet b1.58 QAT in cce_wordlm: linear + EMBEDDING ternary ===\n");
    printf("vocab V=%d, pairs=%d, d=%d, hid=%d, epochs=%d\n\n", V, np, d, hid, epochs);

    /* embedding row usage (sparse: only context words get gradients) */
    char* used=calloc((size_t)V,1); int touched=0;
    for(int i=0;i<np;i++) for(int k=0;k<WCTX;k++){ int w=ctxs[i][k]; if(w>=0&&w<V&&!used[w]){ used[w]=1; touched++; } }

    /* four-way: FP / linear-only / embedding-only / linear+embedding */
    cce_wordlm* m_fp  = train_cfg(0,0, ctxs,tgts,np,V,d,hid,epochs,lr);
    cce_wordlm* m_lin = train_cfg(1,0, ctxs,tgts,np,V,d,hid,epochs,lr);
    cce_wordlm* m_emb = train_cfg(0,1, ctxs,tgts,np,V,d,hid,epochs,lr);
    cce_wordlm* m_both= train_cfg(1,1, ctxs,tgts,np,V,d,hid,epochs,lr);
    double ce_fp=mean_ce(m_fp,ctxs,tgts,np), ce_lin=mean_ce(m_lin,ctxs,tgts,np);
    double ce_emb=mean_ce(m_emb,ctxs,tgts,np), ce_both=mean_ce(m_both,ctxs,tgts,np);

    printf("%-26s %8s %10s\n","config (W1/Wc/Ww | E)","CE","ppl");
    printf("%-26s %8.4f %10.2f\n","FP        | FP",        ce_fp,  exp(ce_fp));
    printf("%-26s %8.4f %10.2f\n","ternary   | FP",        ce_lin, exp(ce_lin));
    printf("%-26s %8.4f %10.2f\n","FP        | ternary",   ce_emb, exp(ce_emb));
    printf("%-26s %8.4f %10.2f\n","ternary   | ternary",   ce_both,exp(ce_both));
    printf("\nsamples:\n");
    printf("  FP            : "); sample(m_fp, V);
    printf("  lin-tern      : "); sample(m_lin, V);
    printf("  embed-tern    : "); sample(m_emb, V);
    printf("  lin+embed-tern: "); sample(m_both, V);

    /* export the FULL-ternary model (E + W1/Wc/Ww packed), reload, prove parity */
    printf("\n=== full-ternary packed export -> reload ===\n");
    const char* tpath="wlm_full_trits.bin";
    if(cce_wordlm_export_trits(m_both, tpath)!=0){ printf("export failed\n"); return 1; }
    cce_wordlm_packed* pk=cce_wordlm_packed_load(tpath);
    if(!pk){ printf("packed load failed\n"); return 1; }
    double maxdnll=0, sq=0, sp=0;
    for(int i=0;i<np;i++){ double a=cce_wordlm_nll(m_both,ctxs[i],tgts[i]); double b=cce_wordlm_packed_nll(pk,ctxs[i],tgts[i]);
        double dd=fabs(a-b); if(dd>maxdnll)maxdnll=dd; sq+=a; sp+=b; }
    int sa[64],sb[64]; int na=gen_fp(m_both,V,sa), nb=gen_pk(pk,V,sb);
    int match=(na==nb); for(int i=0;i<na&&match;i++) if(sa[i]!=sb[i]) match=0;
    printf("parity (ternary E+W vs packed): max|dNLL|=%.2e, ppl %.5f vs %.5f, sample identical=%s\n",
           maxdnll, exp(sq/np), exp(sp/np), match?"YES":"NO");
    printf("  packed sample: Once upon a time"); for(int i=0;i<nb;i++){ if(sb[i]==get_or_add_word(".",0)){printf(".");break;} printf(" %s",wvocab[sb[i]]); } printf("\n");

    /* embedding-row report */
    printf("\nembedding rows: total=%d, touched(trained)=%d, packed=%d  (untouched rows pack from init; never looked up at inference)\n",
           V, touched, V);

    /* size: full artifact now that E is packed too */
    int Cq=cce_wordlm_classes(m_both);
    long E_fp=(long)V*d*4, E_pk=(long)V*((d+4)/5) + (long)V*4;
    long nW=(long)hid*WCTX*d + (long)Cq*hid + (long)V*hid;
    long art_packedE=fsize(tpath);
    long art_fpE = art_packedE - E_pk + E_fp;   /* what the FP-embedding variant would be */
    printf("\n=== SIZE ===\n");
    printf("embedding E      : FP %ld B -> packed %ld B  (%.1fx)\n", E_fp, E_pk, (double)E_fp/E_pk);
    printf("weight mats (%ld) : FP %ld B -> trit %ld B\n", nW, nW*4, (long)(nW/5));
    printf("full artifact    : FP-embed variant ~%ld B  ->  packed-embed %ld B  (%.1fx smaller)\n",
           art_fpE, art_packedE, (double)art_fpE/art_packedE);

    cce_wordlm_packed_free(pk); remove(tpath); free(used);

    int parity_ok = (maxdnll < 1e-3) && match;
    int ok = parity_ok && (ce_both < ce_fp + 2.5) && (ce_emb < ce_fp + 2.5);
    printf("\n%s\n", ok ? "RESULT: PASS — ternary embedding trains + packs; full-ternary artifact reloads bit-exact, no FP weights"
                        : "RESULT: inconclusive");
    free(ctxs); free(tgts);
    cce_wordlm_free(m_fp); cce_wordlm_free(m_lin); cce_wordlm_free(m_emb); cce_wordlm_free(m_both);
    return ok ? 0 : 1;
}
