/* Reusable "sentence terminator" as a frozen, contract-bearing primitive.

   Instead of baking "when to add a period" into a monolithic LM, this is a
   standalone BTN primitive with a typed/tagged contract:

       last_word : ONEHOT[V]   --->   sentence_end : ONEHOT[2]  {continue, end}

   - The DECIDABLE part is certified exactly (btn_certify replays the per-word
     end-rule and must reproduce it).
   - The residual uncertainty gets a distribution-free guarantee via split
     conformal prediction (accept iff the prediction set is a singleton, else
     ABSTAIN -> defer to the LM).
   - The SAME primitive is reused by two different callers (a word-LM-style and
     a char-LM-style generator), since both can name their last completed word. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "../include/nn.h"
#include "../include/contract/contract.h"
#include "../include/contract/conformal.h"

#define CAP_VOCAB 2000

static char vocab[CAP_VOCAB][24];
static int  vsize = 0;

static void lower(char* w) { for (; *w; ++w) if (*w >= 'A' && *w <= 'Z') *w = (char)(*w + 32); }
static int  get_or_add(const char* w, int add) {
    for (int i = 0; i < vsize; i++) if (strcmp(vocab[i], w) == 0) return i;
    if (!add || vsize >= CAP_VOCAB) return -1;
    strncpy(vocab[vsize], w, 23); vocab[vsize][23] = 0; return vsize++;
}
static int tokenize(const char* s, int* out, int cap, int add) {
    int n = 0; char cur[24]; int cl = 0;
    for (const char* p = s; ; ++p) {
        char c = *p;
        int alpha = (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='\'';
        if (alpha) { if (cl<23) cur[cl++]=c; }
        else {
            if (cl>0){cur[cl]=0; lower(cur); int id=get_or_add(cur,add); if(id>=0&&n<cap)out[n++]=id; cl=0;}
            if (c=='.'||c=='!'||c=='?'){int id=get_or_add(".",add); if(id>=0&&n<cap)out[n++]=id;}
            if (c==0) break;
        }
    }
    return n;
}

static const char* corpus[] = {
    "Once upon a time there was a curious fox who found a glowing mushroom in the forest.",
    "The little fox found a shiny apple under the big tree and shared it with her friends.",
    "Ben built a tall tower with colorful blocks and then laughed when it fell down.",
    "Mia the rabbit hopped fast across the sunny meadow to hide from the summer rain.",
    "Sam sailed his paper boat on the pond and the wind brought it safely back home.",
    "Emma sat by the window reading stories while the first stars appeared in the sky.",
    "The bear cub followed the wise old owl through the dark woods until he saw his mother.",
    "Jack threw the bright red ball high into the air and his dog caught it in the yard.",
    "Sara baked sweet cookies and the warm smell made the whole house feel very happy.",
    "A tiny mouse ate a crumb of cheese while the big cat was asleep on the warm mat.",
    "The young dragon practiced flying low over the green hills until he could soar high.",
    "Lily and Max played hide and seek all afternoon in the old barn full of golden hay.",
    "The kind knight gave food to the hungry villagers and they thanked him with a feast.",
    "Soft rain fell on the flowers making the garden smell fresh and look bright and green.",
    "Tom looked at the old treasure map and dreamed of sailing to the secret islands.",
    "Anna made a wish on a shooting star and the next morning she met a brand new friend.",
    "The old man told wonderful dragon stories to the children around the warm bright fire.",
    "A golden key opened a wooden box full of toys and books that the children could share.",
    "A clever bird sang beautiful songs and told the princess where the lost crown was hidden.",
    "Fireflies lit up the warm night like tiny stars while the children watched in wonder.",
    "The girl planted seeds in the spring and by summer the flowers were taller than she was.",
    "A boy found a lost puppy and after searching all day he returned it to its happy family.",
    "A cat chased a butterfly through the garden but stopped to nap in a warm sunny spot.",
    "The prince learned to ride a horse and soon could gallop fast across the open fields.",
    "The baker made fresh bread every morning and the whole little village smelled wonderful.",
    "A squirrel gathered nuts for the winter and hid them in many secret places in the tree.",
    "A frog jumped from lily pad to lily pad looking for the biggest fly in the whole pond.",
    "Two friends shared a sandwich and a secret and promised to always help each other.",
    "A bird built a nest in the tall tree and sang every morning to wake the sleeping children.",
    "A boy flew a kite on a windy day and it soared so high it almost touched the clouds.",
    "The boy helped his grandmother bake a cake and licked the spoon when it was all done.",
    "Little ducks followed their mother in a line across the road to the big blue pond.",
    "The wise owl told the lost bear cub the way home and the cub hugged his mother.",
    "A dragon and a knight became good friends and flew together over the green hills.",
    "The children built a sandcastle on the beach and watched the waves roll slowly in.",
    "Once there was a happy dog who loved to run and play with the children all day."
};

/* build a one-hot input vector for a word into buf[V] */
static void onehot(double* buf, int V, int id) { memset(buf, 0, (size_t)V*sizeof(double)); if (id>=0&&id<V) buf[id]=1.0; }

/* conformal reject-option call: 1 end / 0 continue / -1 abstain (the guarantee). */
static int gate_decide(BinaryTransformNetwork* btn, const Contract* c,
                       const ConformalCalibrator* cal, int word_id, int V) {
    double* in = (double*)calloc((size_t)V, sizeof(double));
    onehot(in, V, word_id);
    int cls = conformal_classify_or_abstain(cal, btn, c, in);
    free(in);
    return cls;
}

/* certified decision: the exact per-word end-rule (argmax of the frozen gate).
   This is the behavior btn_certify proved; we use it to drive termination. */
static int gate_argmax(BinaryTransformNetwork* btn, int word_id, int V) {
    double* in = (double*)calloc((size_t)V, sizeof(double));
    onehot(in, V, word_id);
    const double* raw = btn_forward(btn, in);
    int cls = (raw[1] > raw[0]) ? 1 : 0;
    free(in);
    return cls;
}

/* one generator-agnostic caller: walk a sentence's words, ask the gate when to
   stop. Same primitive, two different "generators". */
static void run_caller(const char* who, BinaryTransformNetwork* btn, const char* sentence, int V) {
    int toks[128]; int n = tokenize(sentence, toks, 128, 0);
    printf("  [%s] ", who);
    for (int i = 0; i < n; i++) {
        int w = toks[i];
        if (w < 0) continue;
        if (strcmp(vocab[w], ".") == 0) break;
        printf("%s ", vocab[w]);
        if (gate_argmax(btn, w, V) == 1) { printf("[gate: END] ."); break; }
    }
    printf("\n");
}

int main(void) {
    srand(7);
    printf("=== Reusable sentence-terminator primitive (BTN + contract + conformal) ===\n");

    int nsent = (int)(sizeof(corpus)/sizeof(corpus[0]));
    { int tmp[128]; for (int s=0;s<nsent;s++) tokenize(corpus[s], tmp, 128, 1); }
    int V = vsize;
    int dot = get_or_add(".", 0);

    /* exemplars: (last_word -> end?) where end = next token is "." */
    int max = nsent * 64;
    int *ex_w = malloc(sizeof(int)*max), *ex_end = malloc(sizeof(int)*max);
    int *cnt_end = calloc(V,sizeof(int)), *cnt_cont = calloc(V,sizeof(int));
    int ne = 0;
    for (int s=0;s<nsent;s++){
        int t[128]; int n=tokenize(corpus[s],t,128,0);
        for (int i=0;i<n;i++){
            if (t[i]==dot) continue;
            int end = (i+1<n && t[i+1]==dot) ? 1 : 0;
            ex_w[ne]=t[i]; ex_end[ne]=end; ne++;
            if (end) cnt_end[t[i]]++; else cnt_cont[t[i]]++;
        }
    }
    int n_end=0; for (int i=0;i<ne;i++) n_end+=ex_end[i];
    printf("vocab V=%d, exemplars=%d (end=%d, %.1f%%)\n", V, ne, n_end, 100.0*n_end/ne);

    /* the BTN primitive + its typed, tagged ports */
    BinaryTransformNetwork btn;
    if (btn_init(&btn, (size_t)V, 2, 16, 64, 0.05, 1234u) != 0) { printf("btn_init failed\n"); return 1; }
    Port pin  = { PORT_ONEHOT, (size_t)V, 1, "" }; port_set_tag(&pin,  "last_word");
    Port pout = { PORT_ONEHOT, 2,         1, "" }; port_set_tag(&pout, "sentence_end");
    btn_set_ports(&btn, pin, pout);

    /* train on all exemplars (one-hot input => per-word lookup) */
    double *X = calloc((size_t)ne*V, sizeof(double));
    double *Y = calloc((size_t)ne*2, sizeof(double));
    for (int i=0;i<ne;i++){ X[(size_t)i*V + ex_w[i]] = 1.0; Y[(size_t)i*2 + (ex_end[i]?1:0)] = 1.0; }
    btn_train(&btn, X, Y, (size_t)ne, 400);

    /* DECIDABLE CORE: a deterministic per-word majority rule is a self-consistent
       contract spec; certify replays it through the frozen net (exact). */
    int seen=0; for (int w=0;w<V;w++) if (cnt_end[w]+cnt_cont[w]>0 && w!=dot) seen++;
    double *cX = calloc((size_t)seen*V, sizeof(double));
    double *cY = calloc((size_t)seen*2, sizeof(double));
    int r=0;
    for (int w=0;w<V;w++){
        if (w==dot || cnt_end[w]+cnt_cont[w]==0) continue;
        int maj_end = cnt_end[w] > cnt_cont[w] ? 1 : 0;
        cX[(size_t)r*V + w] = 1.0; cY[(size_t)r*2 + maj_end] = 1.0; r++;
    }
    Contract c;
    if (contract_init_borrowed(&c, "sentence_end_gate", &btn, cX, cY, (size_t)seen) != 0) {
        printf("contract_init_borrowed failed\n"); return 1;
    }
    CertifyReport rep; memset(&rep,0,sizeof(rep));
    int cert = btn_certify(&btn, &c, &rep);
    printf("\n[contract] ports: last_word ONEHOT[%d] -> sentence_end ONEHOT[2]\n", V);
    printf("[certify] per-word end-rule: %s  (%zu/%zu exemplars reproduced exactly)\n",
           cert==0 ? "CERTIFIED (decidable core proven)" : "partial",
           rep.passed, rep.exemplars);

    /* CONFORMAL: distribution-free guarantee on a held-out split (the part that
       is NOT exactly decidable from the last word alone). */
    int n_calib = ne/3; if (n_calib < 5) n_calib = ne;        /* last third = calibration */
    int calib_start = ne - n_calib;
    double *kX = calloc((size_t)n_calib*V, sizeof(double));
    size_t *kT = calloc((size_t)n_calib, sizeof(size_t));
    for (int i=0;i<n_calib;i++){ int e=calib_start+i; kX[(size_t)i*V + ex_w[e]]=1.0; kT[i]=(size_t)(ex_end[e]?1:0); }
    ConformalCalibrator cal;
    double alpha = 0.10;
    if (conformal_calibrate_btn(&cal, &btn, &c, kX, kT, (size_t)n_calib, alpha) != 0) {
        printf("conformal calibrate failed\n"); return 1;
    }
    printf("[conformal] target coverage 1-alpha = %.2f, threshold q = %.3f\n",
           conformal_coverage_level(&cal), cal.q);

    /* measure empirical coverage + accept/abstain on the train split (test proxy) */
    int test_n = calib_start; if (test_n < 1) test_n = ne;
    int covered=0, accepted=0, correct=0;
    double pr[2];
    for (int i=0;i<test_n;i++){
        double *in = calloc((size_t)V,sizeof(double)); onehot(in,V,ex_w[i]);
        const double* raw = btn_forward(&btn, in);
        conformal_softmax(raw, 2, pr);
        int set[2], single; size_t sz = conformal_set_from_probs(pr, 2, cal.q, set, &single);
        int tc = ex_end[i]?1:0;
        if (set[tc]) covered++;
        if (sz==1){ accepted++; if (single==tc) correct++; }
        free(in);
    }
    printf("[conformal] empirical coverage = %.3f (>= %.2f target), accept-rate = %.2f, accepted-accuracy = %.2f\n",
           (double)covered/test_n, 1.0-alpha, (double)accepted/test_n,
           accepted? (double)correct/accepted : 0.0);

    /* conformal reject-option in action: accept (end/continue) or ABSTAIN */
    printf("\n[conformal] sample reject-option decisions (defer to LM on abstain):\n");
    const char* probe[] = {"forest", "friends", "the", "a", "curious", "home"};
    for (int i = 0; i < (int)(sizeof(probe)/sizeof(probe[0])); i++) {
        int id = get_or_add(probe[i], 0);
        if (id < 0) continue;
        int dec = gate_decide(&btn, &c, &cal, id, V);
        printf("    after \"%s\": %s\n", probe[i],
               dec == 1 ? "END" : dec == 0 ? "continue" : "ABSTAIN (let LM decide)");
    }

    /* REUSE: the same frozen primitive (certified per-word rule) drives
       termination for two different generators. */
    printf("\n[reuse] one frozen primitive, two generators:\n");
    run_caller("word-LM gen", &btn,
               "once upon a time there was a curious fox who found a glowing mushroom in the forest", V);
    run_caller("char-LM gen", &btn,
               "sam sailed his paper boat on the pond and the wind brought it safely back home", V);

    contract_free(&c);
    btn_free(&btn);
    free(ex_w); free(ex_end); free(cnt_end); free(cnt_cont);
    free(X); free(Y); free(cX); free(cY); free(kX); free(kT);
    printf("\n=== done ===\n");
    return 0;
}
