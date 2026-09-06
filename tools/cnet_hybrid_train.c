/* Hybrid train: wordlm exact Adam + n-gram skip.
 * Skip a neural step when the n-gram already has the target as top-1
 * with p >= tau. Always ingest the n-gram (cheap). Not CERT.
 *
 * Reports rdtsc cycles, neural steps, train/holdout NLL vs neural-only.
 */
#include "cce/cce_wordlm.h"
#include "cce/cce_ngram.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>

#define WCTX 3
#define CAP_VOCAB 4000

static char wvocab[CAP_VOCAB][24];
static int wvsize;

static uint64_t cycles_now(void)
{
    unsigned aux;
    return __rdtscp(&aux);
}

static void word_lower(char *w)
{
    for (; *w; ++w)
        if (*w >= 'A' && *w <= 'Z')
            *w = (char)(*w + 32);
}

static int get_or_add_word(const char *w, int add)
{
    for (int i = 0; i < wvsize; i++)
        if (strcmp(wvocab[i], w) == 0)
            return i;
    if (!add || wvsize >= CAP_VOCAB)
        return -1;
    strncpy(wvocab[wvsize], w, 23);
    wvocab[wvsize][23] = 0;
    return wvsize++;
}

static int tokenize(const char *s, int *out, int cap, int add)
{
    int n = 0;
    char cur[24];
    int cl = 0;
    for (const char *p = s;; ++p) {
        char c = *p;
        int alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '\'';
        if (alpha) {
            if (cl < 23)
                cur[cl++] = c;
        } else {
            if (cl > 0) {
                cur[cl] = 0;
                word_lower(cur);
                int id = get_or_add_word(cur, add);
                if (id >= 0 && n < cap)
                    out[n++] = id;
                cl = 0;
            }
            if (c == '.' || c == '!' || c == '?') {
                int id = get_or_add_word(".", add);
                if (id >= 0 && n < cap)
                    out[n++] = id;
            }
            if (c == 0)
                break;
        }
    }
    return n;
}

static const char *corpus[] = {
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
    "The small boat drifted down the river but came back to the boy every evening.",
    "Fireflies lit up the warm night like tiny stars while the children watched in wonder.",
    "The girl planted seeds in spring and by summer the flowers were taller than she was.",
    "A boy found a lost puppy and after searching all day he returned it to its family.",
    "The friends built a fort from blankets and told stories with a flashlight until bedtime.",
    "A cat chased a butterfly through the garden but stopped to nap in a sunny spot.",
    "The baker made fresh bread every morning and the whole village smelled wonderful.",
    "A squirrel gathered nuts for winter and hid them in many secret places in the tree.",
    "Two friends shared a sandwich and a secret and promised to always help each other.",
    "The puppy learned to sit and stay and got a treat and a happy pat on the head.",
    "Little ducks followed their mother in a line across the road to the big pond.",
    "The wise owl told the lost bear cub the way home and the cub hugged his mother.",
    "A dragon and a knight became friends and flew together over the green hills.",
    "The children built a sandcastle on the beach and watched the waves roll in.",
    "The boy read a book about pirates and dreamed of sailing the wide blue sea.",
    "A girl and her dog found a rainbow and followed it to a field of flowers.",
};

static double holdout_nll(cce_wordlm *m, int (*ctxs)[WCTX], const int *tgts, int lo, int hi)
{
    double s = 0;
    int n = hi - lo;
    if (n <= 0)
        return 0;
    for (int i = lo; i < hi; i++)
        s += cce_wordlm_nll(m, ctxs[i], tgts[i]);
    return s / n;
}

static int ngram_skip(cce_ngram *ng, const int *ctx, int target, float tau)
{
    uint32_t c32[WCTX];
    for (int i = 0; i < WCTX; i++)
        c32[i] = (uint32_t)ctx[i];
    uint32_t tok[4];
    float p[4];
    int k = 0;
    if (cce_ngram_predict(ng, c32, WCTX, tok, p, 1, &k) != CCE_NGRAM_OK || k < 1)
        return 0;
    return (tok[0] == (uint32_t)target && p[0] >= tau);
}

static void ngram_observe(cce_ngram *ng, const int *ctx, int target)
{
    uint32_t c32[WCTX];
    for (int i = 0; i < WCTX; i++)
        c32[i] = (uint32_t)ctx[i];
    (void)cce_ngram_add(ng, c32, WCTX, (uint32_t)target, 1);
    (void)cce_ngram_add(ng, c32 + 1, WCTX - 1, (uint32_t)target, 1);
    (void)cce_ngram_add(ng, c32 + 2, 1, (uint32_t)target, 1);
    (void)cce_ngram_add(ng, NULL, 0, (uint32_t)target, 1);
}

int main(void)
{
    const int nsent = (int)(sizeof corpus / sizeof corpus[0]);
    int tmp[256];
    for (int s = 0; s < nsent; s++)
        tokenize(corpus[s], tmp, 256, 1);
    int V = wvsize;
    int d = 32, hid = 64;
    int max_pairs = 0;
    for (int s = 0; s < nsent; s++) {
        int n = tokenize(corpus[s], tmp, 256, 0);
        if (n > WCTX)
            max_pairs += n - WCTX;
    }
    int (*ctxs)[WCTX] = malloc(sizeof(int[WCTX]) * (size_t)max_pairs);
    int *tgts = malloc(sizeof(int) * (size_t)max_pairs);
    int np = 0;
    for (int s = 0; s < nsent; s++) {
        int toks[256];
        int n = tokenize(corpus[s], toks, 256, 0);
        for (int i = WCTX; i < n; i++) {
            for (int k = 0; k < WCTX; k++)
                ctxs[np][k] = toks[i - WCTX + k];
            tgts[np] = toks[i];
            np++;
        }
    }
    int hold = np / 5;
    if (hold < 8)
        hold = 8;
    int ntrain = np - hold;
    int epochs = 40;
    float lr = 0.01f;
    float tau = 0.70f;
    printf("hybrid train V=%d pairs=%d train=%d hold=%d d=%d hid=%d epochs=%d tau=%.2f\n",
           V, np, ntrain, hold, d, hid, epochs, tau);

    int *order = malloc(sizeof(int) * (size_t)ntrain);
    for (int i = 0; i < ntrain; i++)
        order[i] = i;

    /* --- neural only --- */
    cce_wordlm *m0 = cce_wordlm_create(V, d, WCTX, hid, 7u);
    uint64_t c0 = 0;
    long neural0 = 0;
    srand(1);
    for (int ep = 0; ep < epochs; ep++) {
        for (int i = ntrain - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int t = order[i];
            order[i] = order[j];
            order[j] = t;
        }
        for (int q = 0; q < ntrain; q++) {
            int p = order[q];
            uint64_t a = cycles_now();
            (void)cce_wordlm_train_step(m0, ctxs[p], tgts[p], lr);
            c0 += cycles_now() - a;
            neural0++;
        }
    }
    double h0 = holdout_nll(m0, ctxs, tgts, ntrain, np);

    /* --- hybrid: n-gram skip --- */
    cce_wordlm *m1 = cce_wordlm_create(V, d, WCTX, hid, 7u);
    cce_ngram *ng = cce_ngram_open(WCTX + 1, 1u << 14);
    uint64_t c1 = 0, cng = 0;
    long neural1 = 0, skipped = 0;
    srand(1);
    for (int i = 0; i < ntrain; i++)
        order[i] = i;
    for (int ep = 0; ep < epochs; ep++) {
        for (int i = ntrain - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int t = order[i];
            order[i] = order[j];
            order[j] = t;
        }
        for (int q = 0; q < ntrain; q++) {
            int p = order[q];
            uint64_t a = cycles_now();
            int skip = (ep > 0) && ngram_skip(ng, ctxs[p], tgts[p], tau);
            cng += cycles_now() - a;
            ngram_observe(ng, ctxs[p], tgts[p]);
            if (skip) {
                skipped++;
                continue;
            }
            a = cycles_now();
            (void)cce_wordlm_train_step(m1, ctxs[p], tgts[p], lr);
            c1 += cycles_now() - a;
            neural1++;
        }
    }
    double h1 = holdout_nll(m1, ctxs, tgts, ntrain, np);

    double cycle_ratio = (double)(c1 + cng) / (double)(c0 ? c0 : 1);
    double step_ratio = (double)neural1 / (double)(neural0 ? neural0 : 1);
    printf("neural-only   steps=%ld  cycles=%.3e  holdNLL=%.4f\n", neural0, (double)c0, h0);
    printf("hybrid-skip   steps=%ld  cycles=%.3e (ngram %.3e)  skipped=%ld  holdNLL=%.4f\n",
           neural1, (double)c1, (double)cng, skipped, h1);
    printf("ratios        neural_steps=%.3f  total_cycles=%.3f  (want < 1)\n", step_ratio,
           cycle_ratio);

    int pass = (neural1 < neural0) && ((c1 + cng) < c0) && (h1 < h0 * 1.35);
    cce_wordlm_free(m0);
    cce_wordlm_free(m1);
    cce_ngram_close(ng);
    free(order);
    free(ctxs);
    free(tgts);
    if (!pass) {
        printf("HYBRID_TRAIN_FAIL steps_ok=%d cycles_ok=%d nll_ok=%d\n", neural1 < neural0,
               (c1 + cng) < c0, h1 < h0 * 1.35);
        return 1;
    }
    printf("HYBRID_TRAIN_PASS\n");
    return 0;
}
