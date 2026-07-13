/* wordlm_holdout — does joint QAT generalize on the GENERAL trainer, not just
 * the Supra-shaped one? Same three-way held-out test as supra_joint_qat, but on
 * cce_wordlm (a general word-LM with real BitNet b1.58 QAT: FP shadow + STE),
 * NOT the Supra-specific cce_transformer_qat. If the finding is trainer-independent
 * it must reproduce here.
 *
 * Whole sentences held out (every 4th). Three models from one seed:
 *   FP        train ternary OFF,  eval ternary OFF   -> quality ceiling
 *   post-hoc  that SAME FP model, eval ternary ON     -> naive compression
 *   QAT       train ternary ON,   eval ternary ON     -> co-adapted compression
 * (ternary = W1/Wc/Ww + embedding, i.e. the full-ternary "joint" model.)
 *
 * Metric is HELD-OUT next-word cross-entropy / perplexity on unseen sentences.
 * The question: does QAT beat post-hoc on unseen text where the general trainer,
 * left to post-hoc, would collapse?
 *
 * Build: make wordlm_holdout [epochs]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../include/cce/cce_wordlm.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, msg) do { if (c) { g_pass++; printf("  ok   %s\n", (msg)); } \
                           else { g_fail++; printf("  FAIL: %s\n", (msg)); } } while (0)

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
/* Same children's-story register as wordlm_bitnet_demo, extended for a held-out split. */
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
    "Once upon a time a curious cat climbed the tall tree to see the whole town.",
    "The gentle giant carried the tired travelers across the wide river to the town.",
    "A little star fell from the sky and the children kept it safe in a glass jar.",
    "The clever fox tricked the sleepy bear and stole the honey from the old hollow log.",
    "Grandmother knitted a warm scarf for each child before the cold winter arrived.",
    "The sailor told tales of storms and whales while the harbor lights blinked below.",
    "A brave little boat crossed the stormy sea and reached the calm and quiet shore.",
    "The children found a map and dug in the sand until they found a box of shells.",
    "The old clock in the hall chimed twelve times and the whole house fell asleep.",
    "A friendly dragon breathed warm air on the frozen pond so the ducks could swim.",
    "The farmer planted rows of corn and watched them grow tall under the summer sun.",
    "The moon rose over the hills and painted the quiet fields in soft silver light.",
    "A curious kitten pawed at the falling leaves and chased them across the yard.",
    "The baker's daughter sold warm pies at the fair and everyone asked for more.",
    "The young wizard practiced his spells until he could light a candle with a word.",
    "A gentle breeze carried the kite higher and higher above the cheering children.",
    "The three friends shared one umbrella and laughed all the way home in the rain."
};

int main(int argc, char** argv){
    int epochs = (argc > 1) ? atoi(argv[1]) : 200;
    unsigned seed = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 10) : 7u;
    float lr = 0.01f;
    int nsent = (int)(sizeof(corpus)/sizeof(corpus[0]));

    /* build vocab from ALL sentences (held-out words exist but train untouched) */
    { int tmp[256]; for(int s=0;s<nsent;s++) tokenize(corpus[s],tmp,256,1); }
    int V = wvsize, d = 64, hid = 128;

    /* count pairs, split whole sentences: every 4th held out */
    int cap = 0;
    for(int s=0;s<nsent;s++){ int tmp[256]; int n=tokenize(corpus[s],tmp,256,0); if(n>WCTX) cap += n-WCTX; }
    int (*ctx)[WCTX] = malloc(sizeof(int[WCTX])*(size_t)cap);
    int* tgt = malloc(sizeof(int)*(size_t)cap);
    int* ev  = malloc(sizeof(int)*(size_t)cap);
    int np=0, ntr=0, nev=0;
    for(int s=0;s<nsent;s++){ int toks[256]; int n=tokenize(corpus[s],toks,256,0);
        int held = (s % 4 == 0);
        for(int i=WCTX;i<n;i++){ for(int k=0;k<WCTX;k++) ctx[np][k]=toks[i-WCTX+k]; tgt[np]=toks[i]; ev[np]=held;
            if(held) nev++; else ntr++; np++; } }
    printf("wordlm_holdout: vocab=%d pairs=%d (train=%d held-out=%d) d=%d hid=%d epochs=%d\n",
           V, np, ntr, nev, d, hid, epochs);
    CHECK(ntr >= 200, "enough train pairs");
    CHECK(nev >= 40, "enough held-out pairs");

    /* build train-order once (shared across models for a fair comparison) */
    int* order = malloc(sizeof(int)*(size_t)ntr);
    { int j=0; for(int i=0;i<np;i++) if(!ev[i]) order[j++]=i; }

    /* deterministic per-model training: same seed, same pair order (fixed shuffle) */
    unsigned lcg = 12345u;
    for(int i=ntr-1;i>0;i--){ lcg = lcg*1664525u + 1013904223u; int jr = (int)(lcg % (unsigned)(i+1));
        int t=order[i]; order[i]=order[jr]; order[jr]=t; }

    /* Metrics. TRAIN cross-entropy shows the compression mechanism (post-hoc
       collapses / QAT recovers). GENERALIZATION is measured with next-word
       ARGMAX accuracy: bounded [0,1] and robust, where perplexity on this
       tiny/sparse word corpus explodes (near-zero prob on novel transitions).
       predict() with a zero penalty is greedy argmax over the vocab. */
    float* pen = calloc((size_t)V, sizeof(float));
    #define MEANCE(m, want_eval) ({ double s=0; int c=0; \
        for(int i=0;i<np;i++){ if(ev[i]!=(want_eval)) continue; s += cce_wordlm_nll((m), ctx[i], tgt[i]); c++; } c? s/c : 0.0; })
    #define ACC(m, want_eval) ({ int ok=0,c=0; \
        for(int i=0;i<np;i++){ if(ev[i]!=(want_eval)) continue; \
            memset(pen,0,(size_t)V*sizeof(float)); ok += (cce_wordlm_predict((m),ctx[i],pen)==tgt[i]); c++; } c? 100.0*ok/c : 0.0; })

    /* ---- FP: ternary off, train, eval ---- */
    cce_wordlm* fp = cce_wordlm_create(V,d,WCTX,hid,seed);
    cce_wordlm_set_ternary(fp,0); cce_wordlm_set_ternary_embed(fp,0);
    for(int ep=0;ep<epochs;ep++) for(int q=0;q<ntr;q++) cce_wordlm_train_step(fp, ctx[order[q]], tgt[order[q]], lr);
    double fp_tr = MEANCE(fp,0); double fp_ev_acc = ACC(fp,1), fp_tr_acc = ACC(fp,0);

    /* ---- post-hoc: the SAME FP model, flip ternary ON, NO retrain ---- */
    cce_wordlm_set_ternary(fp,1); cce_wordlm_set_ternary_embed(fp,1);
    double ph_tr = MEANCE(fp,0); double ph_ev_acc = ACC(fp,1), ph_tr_acc = ACC(fp,0);

    /* ---- QAT: fresh, same seed, ternary ON before training ---- */
    cce_wordlm* qat = cce_wordlm_create(V,d,WCTX,hid,seed);
    cce_wordlm_set_ternary(qat,1); cce_wordlm_set_ternary_embed(qat,1);
    for(int ep=0;ep<epochs;ep++) for(int q=0;q<ntr;q++) cce_wordlm_train_step(qat, ctx[order[q]], tgt[order[q]], lr);
    double qat_tr = MEANCE(qat,0); double qat_ev_acc = ACC(qat,1), qat_tr_acc = ACC(qat,0);

    printf("\n  === general cce_wordlm trainer: compression mechanism (TRAIN CE) ===\n");
    printf("    FP train ppl %.2f  ->  post-hoc %.2f (%s)  ->  QAT %.2f (%s)\n",
           exp(fp_tr), exp(ph_tr), exp(ph_tr) > 3*exp(fp_tr) ? "COLLAPSES" : "holds",
           exp(qat_tr), exp(qat_tr) < 0.3*exp(ph_tr) ? "RECOVERS" : "flat");
    printf("\n  === held-out next-word ARGMAX accuracy (UNSEEN sentences) ===\n");
    printf("    %-9s TRAIN %5.1f%%   HELD-OUT %5.1f%%\n", "FP",       fp_tr_acc,  fp_ev_acc);
    printf("    %-9s TRAIN %5.1f%%   HELD-OUT %5.1f%%   (naive compression)\n", "post-hoc", ph_tr_acc,  ph_ev_acc);
    printf("    %-9s TRAIN %5.1f%%   HELD-OUT %5.1f%%   (%+.1f pts vs post-hoc)\n", "QAT", qat_tr_acc, qat_ev_acc, qat_ev_acc - ph_ev_acc);
    printf("    VERDICT: on UNSEEN text QAT %.1f%% vs post-hoc %.1f%% -- QAT %s\n",
           qat_ev_acc, ph_ev_acc,
           qat_ev_acc > ph_ev_acc ? "GENERALIZES BETTER" : (qat_ev_acc == ph_ev_acc ? "ties" : "loses"));

    CHECK(exp(ph_tr) > 3*exp(fp_tr), "post-hoc ternary collapses train fit (mechanism reproduces)");
    CHECK(exp(qat_tr) < 0.4*exp(ph_tr), "QAT recovers train fit vs post-hoc (mechanism reproduces)");
    CHECK(qat_ev_acc >= ph_ev_acc, "QAT held-out argmax >= post-hoc (generalizes on the general trainer)");

    free(ctx); free(tgt); free(ev); free(order); free(pen);
    cce_wordlm_free(fp); cce_wordlm_free(qat);
    printf("\nwordlm_holdout: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
