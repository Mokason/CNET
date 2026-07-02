/* Demo: scalable word LM that breaks the O(V^2) wall.
   Shows: exact-gradient check, training, linear param scaling, and free-running
   generation of real-word sentences via tied-embedding + class-factored softmax. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "../include/cce/cce_wordlm.h"

#define CAP_VOCAB 4000
#define WCTX 3

static char wvocab[CAP_VOCAB][24];
static int  wvsize = 0;

static void word_lower(char* w) { for (; *w; ++w) if (*w >= 'A' && *w <= 'Z') *w = (char)(*w + 32); }

static int get_or_add_word(const char* w, int add) {
    for (int i = 0; i < wvsize; i++) if (strcmp(wvocab[i], w) == 0) return i;
    if (!add || wvsize >= CAP_VOCAB) return -1;
    strncpy(wvocab[wvsize], w, 23); wvocab[wvsize][23] = 0;
    return wvsize++;
}

/* tokenize a sentence into word ids; '.'/'!'/'?' -> "." token */
static int tokenize(const char* s, int* out, int cap, int add) {
    int n = 0; char cur[24]; int cl = 0;
    for (const char* p = s; ; ++p) {
        char c = *p;
        int alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '\'';
        if (alpha) { if (cl < 23) cur[cl++] = c; }
        else {
            if (cl > 0) { cur[cl] = 0; word_lower(cur); int id = get_or_add_word(cur, add); if (id >= 0 && n < cap) out[n++] = id; cl = 0; }
            if (c == '.' || c == '!' || c == '?') { int id = get_or_add_word(".", add); if (id >= 0 && n < cap) out[n++] = id; }
            if (c == 0) break;
        }
    }
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
    "The small boat drifted down the river but came back to the boy every evening.",
    "A clever bird sang beautiful songs and told the princess where the lost crown was hidden.",
    "Fireflies lit up the warm night like tiny stars while the children watched in wonder.",
    "The girl planted seeds in spring and by summer the flowers were taller than she was.",
    "A boy found a lost puppy and after searching all day he returned it to its family.",
    "The friends built a fort from blankets and told stories with a flashlight until bedtime.",
    "A cat chased a butterfly through the garden but stopped to nap in a sunny spot.",
    "The prince learned to ride a horse and soon could gallop fast across the open fields.",
    "Little fish swam in a clear stream and jumped to catch flies on a warm day.",
    "The baker made fresh bread every morning and the whole village smelled wonderful.",
    "A squirrel gathered nuts for winter and hid them in many secret places in the tree.",
    "The children drew pictures with colored chalk on the sidewalk until the rain came.",
    "A frog jumped from lily pad to lily pad looking for the biggest fly in the pond.",
    "The artist painted a beautiful sunset using every color she could find in her box.",
    "Two friends shared a sandwich and a secret and promised to always help each other.",
    "The puppy learned to sit and stay and got a treat and a happy pat on the head.",
    "A bird built a nest in the tall tree and sang every morning to wake the children.",
    "The gardener watered the vegetables every day and soon they had tomatoes to eat.",
    "A boy flew a kite on a windy day and it soared so high it almost touched the clouds.",
    "The sisters played with dolls and made up a long story with princesses and animals.",
    "A horse galloped through the meadow with its mane flying in the wind like a flag.",
    "The boy helped his grandmother bake a cake and licked the spoon when it was done.",
    "Little ducks followed their mother in a line across the road to the big pond.",
    "The wise owl told the lost bear cub the way home and the cub hugged his mother.",
    "A dragon and a knight became friends and flew together over the green hills.",
    "The girl found a talking cat who showed her a secret door in the old library.",
    "A little boat with a red sail carried the children to a small island of birds.",
    "The children built a sandcastle on the beach and watched the waves roll in.",
    "A kind giant helped the villagers carry water and they gave him a giant pie.",
    "The star fell from the sky and turned into a friend who played until morning.",
    "Once there was a happy dog who loved to run and play with the children all day.",
    "The fox and the rabbit shared the biggest carrot they had ever seen in the field.",
    "A small turtle walked slowly to the pond and made many friends along the way.",
    "The boy read a book about pirates and dreamed of sailing the wide blue sea.",
    "The little bird was afraid to fly but the wind lifted her gently into the sky.",
    "A girl and her dog found a rainbow and followed it to a field of flowers.",
    "The children sang songs around the fire while the old man played his guitar.",
    "A mouse and a cat became friends and shared cheese under the kitchen table.",
    "The knight rode through the village and waved at the children who cheered for him.",
    "Once upon a time a curious cat climbed the tall tree to see the whole town."
};

int main(void) {
    srand(42);
    printf("=== Scalable word-LM (tied embedding + bottleneck + class softmax) ===\n");

    int nsent = (int)(sizeof(corpus)/sizeof(corpus[0]));
    /* pass 1: build vocab */
    { int tmp[256]; for (int s = 0; s < nsent; s++) tokenize(corpus[s], tmp, 256, 1); }
    int V = wvsize;
    int d = 64, hid = 128;

    /* build (WCTX prev words -> next) pairs, never crossing a sentence */
    int max_pairs = 0;
    for (int s = 0; s < nsent; s++) { int tmp[256]; int n = tokenize(corpus[s], tmp, 256, 0); if (n > WCTX) max_pairs += n - WCTX; }
    int (*ctxs)[WCTX] = malloc(sizeof(int[WCTX]) * (size_t)max_pairs);
    int *tgts = malloc(sizeof(int) * (size_t)max_pairs);
    int np = 0;
    for (int s = 0; s < nsent; s++) {
        int toks[256]; int n = tokenize(corpus[s], toks, 256, 0);
        for (int i = WCTX; i < n; i++) {
            for (int k = 0; k < WCTX; k++) ctxs[np][k] = toks[i - WCTX + k];
            tgts[np] = toks[i];
            np++;
        }
    }
    printf("corpus: %d sentences, vocab V=%d, pairs=%d, d=%d, hid=%d\n", nsent, V, np, d, hid);

    cce_wordlm* m = cce_wordlm_create(V, d, WCTX, hid, 7u);
    if (!m) { printf("create failed\n"); return 1; }
    printf("classes C=%d (~sqrt(V)=%d)\n", cce_wordlm_classes(m), (int)(sqrt((double)V)+0.5));

    /* (1) verify the exact gradient with a finite-difference check */
    double gc = cce_wordlm_gradcheck(m, ctxs[0], tgts[0]);
    printf("[caveat 1] gradient check: max relative error = %.2e %s\n",
           gc, gc < 5e-2 ? "(OK: analytic == numeric -> exact backprop)" : "(HIGH!)");

    /* (2) param scaling: factored is linear in V, dense head is quadratic */
    long fact = cce_wordlm_param_count(V, d, WCTX, hid);
    long dense = (long)WCTX * V * V;
    long fact50k = cce_wordlm_param_count(50000, d, WCTX, hid);
    double dense50k = (double)WCTX * 50000.0 * 50000.0;
    printf("[caveat 2/sizing] params @V=%d: factored=%ld vs dense one-hot head=%ld (%.0fx smaller)\n",
           V, fact, dense, (double)dense / (double)fact);
    printf("                 @V=50000:    factored=%ld vs dense=%.0f (%.0fx smaller, factored stays linear)\n",
           fact50k, dense50k, dense50k / (double)fact50k);

    /* train (exact gradient + Adam), shuffle each epoch */
    int epochs = 300; float lr = 0.01f;
    { const char* e = getenv("WLM_EPOCHS"); if (e) { int v = atoi(e); if (v > 0) epochs = v; } }
    int* order = malloc(sizeof(int) * (size_t)np);
    for (int i = 0; i < np; i++) order[i] = i;
    printf("training %d epochs, lr=%.3f ...\n", epochs, lr);
    for (int ep = 0; ep < epochs; ep++) {
        for (int i = np - 1; i > 0; i--) { int j = rand() % (i + 1); int t = order[i]; order[i] = order[j]; order[j] = t; }
        double sum = 0.0;
        for (int q = 0; q < np; q++) { int p = order[q]; sum += cce_wordlm_train_step(m, ctxs[p], tgts[p], lr); }
        if (ep == 0 || ep == epochs/2 || ep == epochs - 1)
            printf("  epoch %3d  mean CE = %.4f\n", ep, sum / np);
    }

    /* (3) generate a real-word sentence (greedy hierarchical + repetition penalty) */
    printf("\n[generated] ");
    int gctx[WCTX];
    { const char* seed[3] = {"upon", "a", "time"}; for (int k = 0; k < WCTX; k++) { int id = get_or_add_word(seed[k], 0); gctx[k] = id >= 0 ? id : 0; } }
    int dot = get_or_add_word(".", 0);
    float* pen = calloc((size_t)V, sizeof(float));
    printf("Once upon a time");
    for (int step = 0; step < 40; step++) {
        int w = cce_wordlm_predict(m, gctx, pen);
        if (w == dot) { printf("."); break; }
        printf(" %s", wvocab[w]);
        /* decay penalties, discourage just-used word */
        for (int i = 0; i < V; i++) pen[i] *= 0.85f;
        pen[w] -= 4.0f;
        for (int k = 0; k < WCTX - 1; k++) gctx[k] = gctx[k+1];
        gctx[WCTX-1] = w;
    }
    printf("\n");

    free(pen); free(order); free(ctxs); free(tgts);
    cce_wordlm_free(m);
    printf("\n=== done ===\n");
    return 0;
}
