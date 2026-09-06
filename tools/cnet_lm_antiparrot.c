/* Anti-parrot check: hold out WHOLE sentences (unseen stories).
 * n-gram vs wordlm vs hybrid-mix (skip 80% of easy, always train misses).
 * Not CERT. Neural LM is the speaker; n-gram is only a cycle gate.
 */
#include "cce/cce_wordlm.h"
#include "cce/cce_ngram.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#define WCTX 3
#define CAP_VOCAB 4000
static char wvocab[CAP_VOCAB][24];
static int wvsize;

static void word_lower(char *w)
{
    for (; *w; ++w)
        if (*w >= 'A' && *w <= 'Z')
            *w = (char)(*w + 32);
}
static int get_or_add_word(const char *w, int add)
{
    size_t length;
    for (int i = 0; i < wvsize; i++)
        if (strcmp(wvocab[i], w) == 0)
            return i;
    if (!add || wvsize >= CAP_VOCAB)
        return -1;
    length = strlen(w);
    if (length >= sizeof wvocab[wvsize])
        length = sizeof wvocab[wvsize] - 1;
    memcpy(wvocab[wvsize], w, length);
    wvocab[wvsize][length] = 0;
    return wvsize++;
}
static int tokenize(const char *s, int *out, int cap, int add)
{
    int n = 0, cl = 0;
    char cur[24];
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
            if (!c)
                break;
        }
    }
    return n;
}

static const char *corpus[] = {
    "Once upon a time there was a little girl who loved to play in the garden.",
    "Once upon a time there was a curious fox who found a glowing mushroom in the forest.",
    "The little fox found a shiny apple under the big tree and shared it with her friends.",
    "Ben built a tall tower with colorful blocks and then laughed when it fell down.",
    "Mia the rabbit hopped fast across the sunny meadow to hide from the summer rain.",
    "Sam sailed his paper boat on the pond and the wind brought it safely back.",
    "Emma sat by the window reading stories while the first stars appeared in the sky.",
    "Jack threw the bright red ball high into the air and his dog caught it.",
    "Sara baked sweet cookies and the warm smell made the whole house feel happy.",
    "A tiny mouse ate a crumb of cheese while the big cat was asleep on the mat.",
    "Lily and Max played hide and seek all afternoon in the old barn full of hay.",
    "Soft rain fell on the flowers making the garden smell fresh and look bright and green.",
    "Tom looked at the old treasure map and dreamed of sailing to secret islands.",
    "Anna made a wish on a shooting star and the next morning she met a new friend.",
    "The baker made fresh bread every morning and the whole village smelled wonderful.",
    "Two friends shared a sandwich and a secret and promised to always help each other.",
    "Little ducks followed their mother in a line across the road to the big pond.",
    "The children built a sandcastle on the beach and watched the waves roll in.",
    "The boy read a book about pirates and dreamed of sailing the wide blue sea.",
    "A girl and her dog found a rainbow and followed it to a field of flowers.",
    /* held-out stories: different plots, same simple English */
    "The musician played a quiet song on the piano while snow fell outside.",
    "A teacher opened the classroom windows because the air felt too warm.",
    "The farmer carried a heavy bucket of water to the thirsty horses.",
    "Children whispered secrets under the table during the long dinner.",
};

static int ngram_top1(cce_ngram *ng, const int *ctx, int target, float *pout)
{
    uint32_t c32[WCTX], tok[4];
    float p[4];
    int k = 0;
    for (int i = 0; i < WCTX; i++)
        c32[i] = (uint32_t)ctx[i];
    if (cce_ngram_predict(ng, c32, WCTX, tok, p, 1, &k) != 0 || k < 1)
        return 0;
    if (pout)
        *pout = p[0];
    return tok[0] == (uint32_t)target;
}

static void ng_obs(cce_ngram *ng, const int *ctx, int target)
{
    uint32_t c32[WCTX];
    for (int i = 0; i < WCTX; i++)
        c32[i] = (uint32_t)ctx[i];
    cce_ngram_add(ng, c32, WCTX, (uint32_t)target, 1);
    cce_ngram_add(ng, c32 + 1, 2, (uint32_t)target, 1);
    cce_ngram_add(ng, c32 + 2, 1, (uint32_t)target, 1);
    cce_ngram_add(ng, NULL, 0, (uint32_t)target, 1);
}

int main(void)
{
    const int ns = (int)(sizeof corpus / sizeof corpus[0]);
    const int n_hold_sent = 4;
    const int n_train_sent = ns - n_hold_sent;
    int tmp[256];
    for (int s = 0; s < ns; s++)
        tokenize(corpus[s], tmp, 256, 1);
    int V = wvsize;
    int (*ctxs)[WCTX] = malloc(sizeof(int[WCTX]) * 4000);
    int *tgts = malloc(sizeof(int) * 4000);
    int *sid = malloc(sizeof(int) * 4000);
    int np = 0;
    for (int s = 0; s < ns; s++) {
        int toks[256];
        int n = tokenize(corpus[s], toks, 256, 0);
        for (int i = WCTX; i < n; i++) {
            for (int k = 0; k < WCTX; k++)
                ctxs[np][k] = toks[i - WCTX + k];
            tgts[np] = toks[i];
            sid[np] = s;
            np++;
        }
    }
    int ntr = 0, nho = 0;
    int tr[4000], ho[4000];
    for (int i = 0; i < np; i++) {
        if (sid[i] < n_train_sent)
            tr[ntr++] = i;
        else
            ho[nho++] = i;
    }
    printf("anti-parrot  V=%d pairs=%d train_pairs=%d hold_pairs=%d  hold=last %d stories\n",
           V, np, ntr, nho, n_hold_sent);

    cce_ngram *ng = cce_ngram_open(4, 1u << 14);
    for (int i = 0; i < ntr; i++)
        ng_obs(ng, ctxs[tr[i]], tgts[tr[i]]);
    int ng_tr = 0, ng_ho = 0;
    for (int i = 0; i < ntr; i++)
        ng_tr += ngram_top1(ng, ctxs[tr[i]], tgts[tr[i]], NULL);
    for (int i = 0; i < nho; i++)
        ng_ho += ngram_top1(ng, ctxs[ho[i]], tgts[ho[i]], NULL);
    printf("ngram-only   train_top1=%.1f%%  HOLD_top1=%.1f%%  (parrot if train>>hold)\n",
           100.0 * ng_tr / ntr, nho ? 100.0 * ng_ho / nho : 0);

    int epochs = 50;
    float lr = 0.01f;
    cce_wordlm *m = cce_wordlm_create(V, 48, WCTX, 96, 7u);
    srand(2);
    for (int ep = 0; ep < epochs; ep++) {
        for (int i = ntr - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            int t = tr[i];
            tr[i] = tr[j];
            tr[j] = t;
        }
        for (int i = 0; i < ntr; i++) {
            float p = 0;
            int easy = ngram_top1(ng, ctxs[tr[i]], tgts[tr[i]], &p) && p >= 0.70f;
            /* mix: always train misses; train 20% of easy (anti-parrot replay) */
            if (easy && (rand() % 5 != 0))
                continue;
            (void)cce_wordlm_train_step(m, ctxs[tr[i]], tgts[tr[i]], lr);
        }
    }
    double nll_tr = 0, nll_ho = 0;
    int acc_tr = 0, acc_ho = 0;
    for (int i = 0; i < ntr; i++) {
        nll_tr += cce_wordlm_nll(m, ctxs[tr[i]], tgts[tr[i]]);
        if (cce_wordlm_predict(m, ctxs[tr[i]], NULL) == tgts[tr[i]])
            acc_tr++;
    }
    for (int i = 0; i < nho; i++) {
        nll_ho += cce_wordlm_nll(m, ctxs[ho[i]], tgts[ho[i]]);
        if (cce_wordlm_predict(m, ctxs[ho[i]], NULL) == tgts[ho[i]])
            acc_ho++;
    }
    printf("wordlm-mix   train_top1=%.1f%%  HOLD_top1=%.1f%%  trainNLL=%.3f holdNLL=%.3f  (lnV=%.3f)\n",
           100.0 * acc_tr / ntr, nho ? 100.0 * acc_ho / nho : 0, nll_tr / ntr,
           nho ? nll_ho / nho : 0, log((double)V));

    int parrot = (100.0 * ng_tr / ntr > 70) && (100.0 * ng_ho / nho < 35);
    int neural_better = nho && (100.0 * acc_ho / nho > 100.0 * ng_ho / nho);
    printf("ngram_is_parrot=%s  neural_hold_beats_ngram=%s\n", parrot ? "YES" : "no",
           neural_better ? "YES" : "no");
    cce_wordlm_free(m);
    cce_ngram_close(ng);
    free(ctxs);
    free(tgts);
    free(sid);
    printf("ANTIPARROT_DONE\n");
    return 0;
}
