/* transformer_qat_joint — does JOINT ternary QAT generalize where head-only QAT can't?
 *
 * The head-only corpus gate (supra_head_qat_corpus) proved a frozen-transformer
 * head can only MEMORIZE: QAT lifts train recovery far above post-hoc but ties it
 * on held-out sentences, because the ternary head patches a FIXED representation.
 * Joint QAT removes that ceiling: ternarize AND train the whole stack (qkv, proj,
 * mlp, head, emb) so the network co-adapts a ternary-robust representation.
 *
 * Controlled from-scratch experiment (hermetic apart from pdf_corpus.txt):
 * byte-level (vocab=256) next-byte prediction on real English prose, whole
 * sentences held out. Same seed / same pairs / same step budget for every regime;
 * the ONLY difference is which groups are ternary during training.
 *
 *   FP        train all-FP,          eval FP            -> quality ceiling
 *   post-hoc  those SAME weights,    eval all-ternary   -> naive compression
 *   head-QAT  fresh (same seed),     train qat_head,    eval ternary
 *   joint     fresh (same seed),     train ALL qat,     eval ternary
 *
 * Metric is HELD-OUT next-byte accuracy on unseen text (true generalization),
 * not FP-argmax recovery. The question: does `joint` recover the FP->post-hoc
 * gap that post-hoc (and head-QAT) leave on the table?
 *
 * Build: make transformer_qat_joint [epochs] [lr] [max_pairs] [n_embd]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_transformer_qat.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(c, msg) do { if (c) { g_pass++; printf("  ok   %s\n", (msg)); } \
                           else { g_fail++; printf("  FAIL: %s\n", (msg)); } } while (0)

#define CTX 48                 /* context window (== block_size) */
#define VOCAB 256              /* byte-level */

/* one (context, target) next-byte pair, contexts packed CTX-wide */
typedef struct { int len; int target; int eval; } Pair;

static int argmax(const float* v, int n) { int b = 0; for (int i = 1; i < n; i++) if (v[i] > v[b]) b = i; return b; }

/* Hermetic fallback corpus: diverse English prose so the gate runs with NO
 * external file (pdf_corpus.txt, when present, supersedes this for a fuller run). */
static const char* EMB_CORPUS[] = {
    "The old fisherman rose before dawn and walked down to the harbor where his boat waited.",
    "Salt wind carried the smell of the sea across the empty market square at first light.",
    "She opened the heavy book and read the first page slowly, tracing each line with her finger.",
    "The children ran through the tall grass until they reached the edge of the quiet forest.",
    "Rain fell softly on the roof and the fire crackled in the small stone hearth all evening.",
    "He counted the coins twice before handing them to the merchant at the corner stall.",
    "The mountain path grew steep and narrow as the tired travelers climbed toward the pass.",
    "Morning sunlight spilled across the wooden floor and warmed the sleeping grey cat.",
    "A distant train whistle broke the long silence of the cold winter afternoon.",
    "The scientist measured the sample carefully and recorded the result in her worn notebook.",
    "Every evening the lamplighter walked the same street, touching each lantern into flame.",
    "The river wound through the valley, wide and slow beneath the old arching stone bridge.",
    "Thunder rolled over the hills and the first heavy drops struck the dusty country road.",
    "The baker pulled a tray of warm bread from the oven and set it on the wooden counter.",
    "Two horses grazed quietly in the meadow while the shepherd mended his broken fence.",
    "She folded the letter and placed it in the drawer beside the old faded photographs.",
    "The city lights flickered on one by one as the wide sky deepened into deep violet.",
    "He studied the map for a long time, then chose the winding road that led north.",
    "The garden was full of bees and the heavy sweet scent of the blooming summer roses.",
    "An old clock ticked steadily in the narrow hallway of the quiet sleeping house.",
    "The captain gave the order and the sailors hauled the ropes with steady careful hands.",
    "Frost covered the window and the small boy drew shapes in the white with his fingernail.",
    "The teacher wrote the problem on the board and waited for a single hand to rise.",
    "Beyond the last farm the golden wheat fields stretched all the way to the horizon.",
    "The dog waited by the gate, ears lifted, watching the empty road for its master.",
    "She hummed an old song while she kneaded the soft dough on the floured kitchen table.",
    "A cold current pulled the little wooden boat away from the crowded morning shore.",
    "The market filled with noise and color as the vendors called out their morning prices.",
    "He set the lantern down and studied the strange markings carved into the grey stone.",
    "The library was silent except for the turning of pages and a single distant cough.",
    "Snow began to fall as the last pale light faded behind the western mountain ridge.",
    "The blacksmith struck the glowing iron and bright sparks scattered across the dark floor.",
    "A narrow stream ran clear over the smooth stones at the bottom of the deep ravine.",
    "The traveler knocked three times and waited beneath the low wooden roof of the inn.",
    "Wind moved through the orchard and shook loose a soft rain of pale apple petals.",
    "The old woman told her stories by the fire until the youngest child fell fast asleep.",
    "He watched the ships come in and tried to guess which one carried his lost brother.",
    "The path forked at the great oak and neither branch showed a single fresh footprint.",
    "Morning mist clung to the low fields long after the sun had cleared the tall trees.",
    "The clerk stamped the papers and slid them back across the smooth polished desk.",
    "A wooden ladder leaned against the barn where the swallows built their small nests.",
    "She poured the tea and watched the steam curl slowly upward in the cold still room.",
    "The soldiers marched in silence along the muddy edge of the wide flooded road.",
    "Lightning lit the whole sky for a single instant and then the heavy dark returned.",
    "The carpenter measured the plank twice and drew a careful line with his flat pencil.",
    "Far out on the water a lone fisherman raised his dark net against the falling sun.",
    "The sleeping town lay still while the watchman walked his round with a swinging lamp.",
    "A bright red kite hung motionless in the wind above the empty green playing field.",
    "The professor closed his lecture with a quiet question that no student could answer.",
    "Wet leaves stuck to the pavement and the gutters ran fast with the cold autumn rain.",
    "He tuned the old guitar by ear and began to play a slow and familiar evening tune.",
    "A single candle burned in the tall tower window long after midnight had passed.",
    "The farmer led the cattle home as the first bright stars appeared in the eastern sky.",
    "She sketched the harbor quickly before the changing light on the water shifted again.",
    "The boy climbed the apple tree to reach the ripe red fruit near the very top.",
    "Grey ash drifted down from the chimney and settled softly on the frozen winter ground.",
};
#define EMB_N ((int)(sizeof(EMB_CORPUS)/sizeof(EMB_CORPUS[0])))

/* Train `t` for `epochs` over the train pairs (eval==0). qat flags must be set
 * by the caller BEFORE the first step so the ternary forward is live throughout.
 * Returns held-out next-byte accuracy under the CURRENT qat setting. */
static double train_and_eval(cce_transformer_qat* t, const int* ctxbuf, const Pair* pr,
                             int npairs, int epochs, float lr, int ntrain,
                             double* first_ce, double* last_ce) {
    for (int e = 0; e < epochs; e++) {
        double sum = 0;
        for (int p = 0; p < npairs; p++) {
            if (pr[p].eval) continue;
            const int* ctx = ctxbuf + (size_t)p * CTX;
            sum += cce_transformer_qat_step(t, ctx, pr[p].len, NULL, pr[p].target, lr);
        }
        if (e == 0 && first_ce) *first_ce = sum / (ntrain ? ntrain : 1);
        if (last_ce) *last_ce = sum / (ntrain ? ntrain : 1);
    }
    /* eval: held-out next-byte accuracy under current qat setting */
    int correct = 0, neval = 0;
    float lg[VOCAB];
    for (int p = 0; p < npairs; p++) {
        if (!pr[p].eval) continue;
        const int* ctx = ctxbuf + (size_t)p * CTX;
        if (cce_transformer_qat_logits(t, ctx, pr[p].len, lg) != CCE_OK) continue;
        correct += (argmax(lg, VOCAB) == pr[p].target);
        neval++;
    }
    return neval ? 100.0 * correct / neval : 0.0;
}

/* held-out accuracy for the CURRENT model+qat state, no training */
static double eval_only(cce_transformer_qat* t, const int* ctxbuf, const Pair* pr, int npairs) {
    int correct = 0, neval = 0;
    float lg[VOCAB];
    for (int p = 0; p < npairs; p++) {
        if (!pr[p].eval) continue;
        const int* ctx = ctxbuf + (size_t)p * CTX;
        if (cce_transformer_qat_logits(t, ctx, pr[p].len, lg) != CCE_OK) continue;
        correct += (argmax(lg, VOCAB) == pr[p].target);
        neval++;
    }
    return neval ? 100.0 * correct / neval : 0.0;
}

int main(int argc, char** argv) {
    int   epochs    = (argc > 1) ? atoi(argv[1]) : 10;
    float lr        = (argc > 2) ? (float)atof(argv[2]) : 0.01f;
    int   max_pairs = (argc > 3) ? atoi(argv[3]) : 3000;
    int   n_embd    = (argc > 4) ? atoi(argv[4]) : 64;
    unsigned seed   = (argc > 5) ? (unsigned)strtoul(argv[5], NULL, 10) : 1234u;
    printf("config: epochs=%d lr=%g max_pairs=%d n_embd=%d ctx=%d seed=%u\n",
           epochs, lr, max_pairs, n_embd, CTX, seed);

    /* ---- load corpus, byte-tokenize, whole-sentence held-out split ---- */
    FILE* f = fopen("pdf_corpus.txt", "rb");   /* fuller external corpus if present */
    const char* csrc = f ? "pdf_corpus.txt" : "embedded";

    int*  ctxbuf = (int*)malloc((size_t)max_pairs * CTX * sizeof(int));
    Pair* pr     = (Pair*)malloc((size_t)max_pairs * sizeof(Pair));
    char  line[8192];
    int   npairs = 0, sent = 0, ntrain = 0, neval = 0, emb_i = 0;

    while (npairs < max_pairs) {
        if (f) { if (!fgets(line, sizeof line, f)) break; line[strcspn(line, "\r\n")] = 0; }
        else   { if (emb_i >= EMB_N) break; strncpy(line, EMB_CORPUS[emb_i++], sizeof line - 1); line[sizeof line - 1] = 0; }
        int n = (int)strlen(line);
        if (n < 12) continue;
        int held = (sent % 8 == 0);              /* every 8th whole sentence held out */
        /* next-byte pairs: target = line[t+1], context = last CTX bytes up to t */
        for (int t = 0; t < n - 1 && npairs < max_pairs; t++) {
            int start = t + 1 - CTX; if (start < 0) start = 0;
            int len = t + 1 - start;             /* 1..CTX */
            int* ctx = ctxbuf + (size_t)npairs * CTX;
            for (int k = 0; k < len; k++) ctx[k] = (unsigned char)line[start + k];
            pr[npairs].len = len;
            pr[npairs].target = (unsigned char)line[t + 1];
            pr[npairs].eval = held;
            if (held) neval++; else ntrain++;
            npairs++;
        }
        sent++;
    }
    if (f) fclose(f);
    printf("corpus[%s]: %d sentences -> %d pairs (train=%d held-out=%d)\n", csrc, sent, npairs, ntrain, neval);
    CHECK(ntrain >= 500, "enough train pairs");
    CHECK(neval >= 100, "enough held-out pairs");

    cce_transformer_qat_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.n_layer = 2; cfg.n_embd = n_embd; cfg.n_head = 4;
    cfg.mlp_hidden = 4 * n_embd; cfg.vocab = VOCAB; cfg.block_size = CTX;
    cfg.seed = seed;

    /* ---- A. FP model: train all-FP, eval FP (ceiling) and post-hoc ternary ---- */
    double fp_first = 0, fp_last = 0;
    cce_transformer_qat* A = cce_transformer_qat_create(&cfg);
    cce_transformer_qat_set_qat(A, 0, 0, 0, 0, 0);
    double fp_ev = train_and_eval(A, ctxbuf, pr, npairs, epochs, lr, ntrain, &fp_first, &fp_last);
    printf("  FP        CE %.3f -> %.3f | held-out acc = %.2f%%\n", fp_first, fp_last, fp_ev);
    cce_transformer_qat_set_qat(A, 1, 1, 1, 1, 1);          /* flip ternary, NO retrain */
    double ph_ev = eval_only(A, ctxbuf, pr, npairs);
    printf("  post-hoc  (same weights, ternary eval)   | held-out acc = %.2f%%\n", ph_ev);
    cce_transformer_qat_free(A);

    /* ---- B. head-only QAT: fresh, same seed, only head ternary in training ---- */
    double h_first = 0, h_last = 0;
    cce_transformer_qat* B = cce_transformer_qat_create(&cfg);
    cce_transformer_qat_set_qat(B, 0, 0, 0, 1, 0);
    double head_ev = train_and_eval(B, ctxbuf, pr, npairs, epochs, lr, ntrain, &h_first, &h_last);
    printf("  head-QAT  CE %.3f -> %.3f | held-out acc = %.2f%%\n", h_first, h_last, head_ev);
    cce_transformer_qat_free(B);

    /* ---- C. joint QAT: fresh, same seed, ALL groups ternary in training ---- */
    double j_first = 0, j_last = 0;
    cce_transformer_qat* C = cce_transformer_qat_create(&cfg);
    cce_transformer_qat_set_qat(C, 1, 1, 1, 1, 1);
    double joint_ev = train_and_eval(C, ctxbuf, pr, npairs, epochs, lr, ntrain, &j_first, &j_last);
    printf("  joint     CE %.3f -> %.3f | held-out acc = %.2f%%\n", j_first, j_last, joint_ev);
    cce_transformer_qat_free(C);

    /* ---- verdict: the robust signal is joint-vs-post-hoc on UNSEEN text ---- */
    double gap        = fp_ev - ph_ev;             /* naive-compression loss vs FP */
    double joint_gain = joint_ev - ph_ev;          /* what joint QAT recovers over post-hoc */
    double head_gain  = head_ev  - ph_ev;
    printf("\n  === held-out next-byte accuracy (UNSEEN sentences) ===\n");
    printf("    FP (fp-trained)  %.2f%%\n", fp_ev);
    printf("    post-hoc ternary %.2f%%   (naive compression of that FP model)\n", ph_ev);
    printf("    head-only QAT    %.2f%%   (%+.2f pts vs post-hoc)\n", head_ev, head_gain);
    printf("    joint QAT        %.2f%%   (%+.2f pts vs post-hoc)\n", joint_ev, joint_gain);
    if (gap >= 1.0)
        printf("    joint QAT recovers %.0f%% of the %.2f-pt FP->post-hoc gap\n",
               100.0 * joint_gain / gap, gap);
    else
        printf("    (FP~=post-hoc here: naive ternary barely hurt; joint still %+.2f pts)\n", joint_gain);
    if (joint_ev >= fp_ev - 1e-6)
        printf("    NOTE: joint QAT >= FP -- ternary quantization acting as a regularizer\n");
    printf("    VERDICT: joint QAT %s post-hoc on unseen text (%.2f%% vs %.2f%%); %s head-only (%.2f%%)\n",
           joint_ev > ph_ev ? "BEATS" : (joint_ev == ph_ev ? "ties" : "loses to"), joint_ev, ph_ev,
           joint_ev > head_ev ? "beats" : (joint_ev == head_ev ? "ties" : "loses to"), head_ev);

    CHECK(fp_last < fp_first, "FP training reduced CE");
    CHECK(j_last < j_first, "joint QAT training reduced CE under ternary forward");
    CHECK(joint_ev >= ph_ev, "joint QAT generalizes: held-out acc >= post-hoc ternary");

    free(ctxbuf); free(pr);
    printf("\ntransformer_qat_joint: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
