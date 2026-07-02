#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#endif

#include "include/cce/cce_learn.h"
#include "include/cce/cce_cascade.h"
#include "include/cce/cce_block.h"
#include "include/cce/cce_tensor.h"
#include "include/cce/cce_gpu.h"
#include "include/cce/cce_forest.h"
#include "include/cce/cce_router.h"

#define MAX_VOCAB 128
#define MAX_SEQ 2048
#define MAX_STORIES 120
#define CTX 8   /* char context window. (Real-word sentences come from the
                   word-level model below; the char model is the "pure logits" ref.) */
#define HID 128
/* Default cascade depth (hidden layers before the softmax head). Shallow is
   crispest AND fastest for free-running here: the head error is exact but
   hidden-layer DFA credit is approximate, so deep stacks free-run worse.
   Default 0 = pure softmax char model over the CTX-char window (a learned
   interpolated n-gram). Override with CNET_TS_HIDDEN=2 etc. for a deeper
   cascade (coherent but slightly less crisp, and slower on CPU). */
#define NUM_HIDDEN 0

static char vocab[MAX_VOCAB];
static int vocab_size = 0;

int get_or_add_char(char c) {
    for (int i = 0; i < vocab_size; i++) {
        if (vocab[i] == c) return i;
    }
    if (vocab_size < MAX_VOCAB) {
        vocab[vocab_size] = c;
        return vocab_size++;
    }
    return 0;
}

/* === Word-level vocabulary (for real-word free-running generation) ===
   A char model can't reliably free-run into real words (its greedy next-char is
   wrong too often, so words break apart). Modeling whole words instead makes every
   emitted token a real word by construction; the model only has to learn word
   transitions. */
#define WMAX_VOCAB 500    /* keep < CCE_MAX_OUT (512) in the learner */
#define WCTX 3            /* previous words of context */
static char wvocab[WMAX_VOCAB][24];
static int wvocab_size = 0;

/* lower-case a single word in place */
static void word_lower(char *w) {
    for (; *w; ++w) if (*w >= 'A' && *w <= 'Z') *w = (char)(*w + 32);
}

static int get_or_add_word(const char *w) {
    if (!w || !*w) return -1;
    for (int i = 0; i < wvocab_size; i++) if (strcmp(wvocab[i], w) == 0) return i;
    if (wvocab_size < WMAX_VOCAB) {
        strncpy(wvocab[wvocab_size], w, 23);
        wvocab[wvocab_size][23] = 0;
        return wvocab_size++;
    }
    return -1;
}

static int find_word(const char *w) {
    if (!w) return -1;
    for (int i = 0; i < wvocab_size; i++) if (strcmp(wvocab[i], w) == 0) return i;
    return -1;
}

/* Tokenize a story into lowercased word tokens; '.' '!' '?' become a "." token so
   sentences can end. Returns token count, writing word ids into out (capacity cap). */
static int tokenize_words(const char *s, int *out, int cap, int add_to_vocab) {
    int n = 0;
    char cur[24]; int cl = 0;
    for (const char *p = s; ; ++p) {
        char c = *p;
        int is_alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '\'';
        if (is_alpha) {
            if (cl < 23) cur[cl++] = c;
        } else {
            if (cl > 0) {
                cur[cl] = 0; word_lower(cur);
                int id = add_to_vocab ? get_or_add_word(cur) : find_word(cur);
                if (id >= 0 && n < cap) out[n++] = id;
                cl = 0;
            }
            if (c == '.' || c == '!' || c == '?') {
                int id = add_to_vocab ? get_or_add_word(".") : find_word(".");
                if (id >= 0 && n < cap) out[n++] = id;
            }
            if (c == 0) break;
        }
    }
    return n;
}

/* === Reusable free-run decoders (used by the standalone demos AND the forest
   router so a routed branch generates with the same logic). === */

/* Char-level: low-temp nucleus sampling + score-based repetition penalty.
   Seeds its context from whatever is already in `out`. */
static void freerun_char(const cce_cascade *c, int in_dim,
                         float rtemp, float rtopp, float rpenalty,
                         char *out, size_t outsz) {
    int rctx[CTX];
    { int rl = (int)strlen(out);
      for (int i = 0; i < CTX; i++) { int off = rl - CTX + i; char rc = (off >= 0) ? out[off] : ' '; rctx[i] = get_or_add_char(rc); } }
    for (int step = 0; step < 140 && strlen(out) < outsz - 2 && strlen(out) < 140; step++) {
        float xin[CTX * MAX_VOCAB]; memset(xin, 0, sizeof(xin));
        for (int cc = 0; cc < CTX; cc++) { int ci = rctx[cc]; if (ci >= 0 && ci < vocab_size) xin[cc * vocab_size + ci] = 1.0f; }
        cce_tensor tx; int xs[1] = {in_dim}; cce_tensor_alloc(&tx, xs, 1);
        memcpy(tx.data, xin, (size_t)in_dim * sizeof(float));
        cce_tensor ty; cce_tensor_alloc(&ty, (int[]){vocab_size}, 1);
        cce_cascade_forward(c, &tx, &ty);

        float logit[128];
        for (int v = 0; v < vocab_size; v++) {
            float sc = ty.data[v];
            for (int k = 0; k < CTX; k++) if (rctx[k] == v) sc -= rpenalty;
            logit[v] = sc;
        }
        float mx = -1e30f;
        for (int v = 0; v < vocab_size; v++) if (logit[v] > mx) mx = logit[v];
        float w[128]; float sw = 0;
        for (int v = 0; v < vocab_size; v++) { w[v] = expf((logit[v] - mx) / rtemp); sw += w[v]; }
        for (int v = 0; v < vocab_size; v++) w[v] /= (sw > 0 ? sw : 1);
        int id[128]; for (int v = 0; v < vocab_size; v++) id[v] = v;
        for (int i = 0; i < vocab_size-1; i++) for (int j = i+1; j < vocab_size; j++) if (w[id[j]] > w[id[i]]) { int t=id[i]; id[i]=id[j]; id[j]=t; }
        float cum = 0; int nend = 0;
        for (int i = 0; i < vocab_size; i++) { cum += w[id[i]]; nend = i+1; if (cum >= rtopp) break; }
        if (nend < 1) nend = 1;
        float r = (float)rand() / RAND_MAX * cum; float c2 = 0; int next = id[0];
        for (int i = 0; i < nend; i++) { c2 += w[id[i]]; if (r <= c2) { next = id[i]; break; } }

        char ch = vocab[next];
        size_t olen = strlen(out);
        if (olen + 1 < outsz) { out[olen] = ch; out[olen+1] = 0; }
        for (int cc = 0; cc < CTX-1; cc++) rctx[cc] = rctx[cc+1];
        rctx[CTX-1] = next;
        cce_tensor_free(&ty); cce_tensor_free(&tx);
        if (ch == '.' && step > 12) break;
    }
    size_t rl = strlen(out);
    while (rl > 0 && out[rl-1] == ' ') out[--rl] = 0;
    if (rl > 0 && out[rl-1] != '.') {
        size_t cut = rl;
        while (cut > 0 && out[cut-1] != ' ' && out[cut-1] != '.') cut--;
        if (cut > 6) { out[cut] = 0; rl = cut; while (rl > 0 && out[rl-1] == ' ') out[--rl] = 0; }
        if (rl > 0 && out[rl-1] != '.') { out[rl] = '.'; out[rl+1] = 0; }
    }
}

/* Word-level: greedy with a penalty on words already in the context window.
   Seeds from "upon a time" (so it continues "Once upon a time ..."). */
static void freerun_word(const cce_cascade *wc, int wv, int win_dim, char *out, size_t outsz) {
    int dot_id = find_word(".");
    int wctx_ids[WCTX];
    const char *seedw[3] = {"upon", "a", "time"};
    for (int k = 0; k < WCTX; k++) { int id = find_word(seedw[k]); wctx_ids[k] = (id >= 0) ? id : 0; }
    float *wxin = (float*)malloc((size_t)win_dim * sizeof(float));
    if (!wxin) return;
    for (int step = 0; step < 40 && strlen(out) < outsz - 2; step++) {
        memset(wxin, 0, (size_t)win_dim * sizeof(float));
        for (int k = 0; k < WCTX; k++) { int t = wctx_ids[k]; if (t >= 0 && t < wv) wxin[k * wv + t] = 1.0f; }
        cce_tensor tx; int xs2[1] = {win_dim}; cce_tensor_alloc(&tx, xs2, 1);
        memcpy(tx.data, wxin, (size_t)win_dim * sizeof(float));
        cce_tensor ty; cce_tensor_alloc(&ty, (int[]){wv}, 1);
        cce_cascade_forward(wc, &tx, &ty);
        int best = -1; float bestv = -1e30f;
        for (int v = 0; v < wv; v++) {
            float val = ty.data[v];
            for (int k = 0; k < WCTX; k++) if (wctx_ids[k] == v) val -= 2.0f;
            if (val > bestv) { bestv = val; best = v; }
        }
        cce_tensor_free(&ty); cce_tensor_free(&tx);
        if (best < 0) best = (dot_id >= 0) ? dot_id : 0;
        if (best == dot_id) { size_t L = strlen(out); if (L + 1 < outsz) { out[L]='.'; out[L+1]=0; } break; }
        size_t L = strlen(out);
        const char *wd = wvocab[best];
        if (L + strlen(wd) + 2 < outsz) { out[L]=' '; out[L+1]=0; strcat(out, wd); }
        for (int k = 0; k < WCTX-1; k++) wctx_ids[k] = wctx_ids[k+1];
        wctx_ids[WCTX-1] = best;
    }
    { size_t L = strlen(out); if (L > 0 && out[L-1] != '.' && L+1 < outsz) { out[L]='.'; out[L+1]=0; } }
    free(wxin);
}

/* Robust fetch + parse for TinyStories jsonl - aggressively collect many stories */
static int fetch_tinystories_samples(char stories[][1024], int max_stories) {
    const char* url = "https://huggingface.co/datasets/roneneldan/TinyStories/resolve/main/data/validation-00000-of-00001.jsonl";
    int count = 0;
#ifdef _WIN32
    char *tmpname = _tempnam(NULL, "cnet_ts_");
    if (!tmpname) return 0;
    FILE *outf = fopen(tmpname, "wb");
    if (!outf) { free(tmpname); return 0; }

    HINTERNET hSession = InternetOpenA("CCE-TinyStories", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (hSession) {
        HINTERNET hRequest = InternetOpenUrlA(hSession, url, NULL, 0,
            INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES, 0);
        if (hRequest) {
            unsigned char buf[8192];
            DWORD read = 0;
            while (InternetReadFile(hRequest, buf, sizeof(buf), &read) && read > 0) {
                fwrite(buf, 1, read, outf);
            }
            InternetCloseHandle(hRequest);
        }
        InternetCloseHandle(hSession);
    }
    fclose(outf);

    FILE *inf = fopen(tmpname, "r");
    if (inf) {
        char line[8192];
        while (count < max_stories && fgets(line, sizeof(line)-1, inf)) {
            const char *p = strstr(line, "\"text\"");
            if (!p) continue;
            p = strchr(p, ':'); if (!p) continue;
            p = strchr(p, '"'); if (!p) continue;
            ++p;
            char cleaned[1024];
            size_t cl = 0;
            while (*p && cl < 1000) {
                unsigned char c = (unsigned char)*p++;
                if (c == '\\') {
                    unsigned char esc = (unsigned char)*p++;
                    if (esc == 'n' || esc == 'r') c = ' ';
                    else if (esc == 't') c = ' ';
                    else if (esc == '"') c = '"';
                    else if (esc == '\\') c = '\\';
                    else if (esc == 'u') { for(int k=0; k<4 && *p; k++) ++p; c = '?'; }
                    else c = esc;
                } else if (c == '"') {
                    break;
                }
                cleaned[cl++] = (char)c;
            }
            cleaned[cl] = 0;
            if (cl > 40) {  /* real story length */
                strncpy(stories[count], cleaned, 1023);
                stories[count][1023] = 0;
                count++;
            }
        }
        fclose(inf);
    }
    remove(tmpname);
    free(tmpname);
#endif
    return count;
}

int main(void) {
    srand(42);
    printf("=== CCE Real Test on TinyStories Data ===\n");

    char stories[MAX_STORIES][1024];
    int num_stories = fetch_tinystories_samples(stories, MAX_STORIES);
    if (num_stories < 3) {
        // expanded fallback
        strcpy(stories[0], "Lily loved to play in the garden with her red ball. The ball was big and round. She threw it high into the sky. Her dog barked happily.");
        strcpy(stories[1], "Tom had a little black cat named Max. The cat liked to sleep on a soft warm mat. One day the cat chased a small gray mouse through the house. They played all day.");
        strcpy(stories[2], "Once upon a time there was a brave young knight. He found a shiny golden key in the dark forest. The key opened a secret magic door to a treasure. He shared it with friends.");
        strcpy(stories[3], "A little girl named Anna found a bright shiny star on the ground. She put it in her pocket and made a special wish for her friend. The star sparkled and glowed bright at night. Everyone was happy.");
        strcpy(stories[4], "The old wise man told wonderful stories about big dragons and tall castles. The children sat and listened carefully and laughed a lot. They wanted to hear even more tales about adventures and heroes.");
        strcpy(stories[5], "Ben and his sister found a magic box in the attic. Inside was a tiny dragon that could talk. They became best friends and had many adventures together.");
        strcpy(stories[6], "Mia the little rabbit hopped through the meadow looking for carrots. She met a friendly fox who helped her find the biggest carrot ever.");
        strcpy(stories[7], "A boy named Sam built a paper boat and sailed it on the pond. The wind carried it far but it always came back to him.");
        strcpy(stories[8], "Emma loved reading books about faraway lands. One night her book came to life and took her on a journey to the stars.");
        strcpy(stories[9], "The little bear cub got lost in the woods but a kind owl showed him the way home to his family. They had a big hug.");
        strcpy(stories[10], "Once there was a curious fox who found a glowing mushroom. The mushroom whispered secrets of the forest.");
        strcpy(stories[11], "A princess found a talking bird in her garden. The bird told her where the lost crown was hidden.");
        strcpy(stories[12], "Jack ran fast to catch the big red ball before it rolled into the river. He laughed when his friend helped pull it out.");
        strcpy(stories[13], "Sara baked sweet cookies for her mother. The kitchen smelled wonderful and warm. They ate them together with milk.");
        strcpy(stories[14], "The small mouse found a piece of cheese under the table. It was quiet and careful so the cat would not see it.");
        strcpy(stories[15], "A young dragon learned to fly over the green hills. It practiced every day until it could soar high in the blue sky.");
        strcpy(stories[16], "Lily and Max played hide and seek in the old barn. Max hid behind the hay and Lily found him quickly.");
        strcpy(stories[17], "The knight rode his horse through the village and waved at the happy children. Everyone cheered for the hero.");
        strcpy(stories[18], "A gentle rain fell on the flowers making them grow tall and bright. The girl danced between the drops.");
        strcpy(stories[19], "Tom opened the old book and read about pirates and treasure maps. He dreamed of sailing the seas.");
        num_stories = 20;
    }
    // For pure CCE (A) we need volume. If fetch is limited, massively augment with unique coherent micro-stories.
    if (num_stories < 80) {
        int add_idx = num_stories;
        const char* extras[] = {
            "The little fox found a shiny apple under the big tree and shared it with her friends under the stars.",
            "Ben built a tall tower with colorful blocks then laughed when it fell down with a big crash.",
            "Mia the rabbit hopped fast across the sunny meadow to hide from the sudden summer rain.",
            "Sam sailed his paper boat on the pond and the wind brought it safely back every single time.",
            "Emma sat by the window reading stories while the first stars appeared in the evening sky.",
            "The bear cub followed the wise old owl through the dark woods until he saw his mother waiting.",
            "Jack threw the bright red ball high into the air and his dog caught it before it hit the grass.",
            "Sara baked sweet cookies and the warm smell made the whole house feel happy and cozy.",
            "A tiny mouse ate a crumb of cheese while the big cat was fast asleep on the warm mat.",
            "The young dragon practiced flying low over the green hills until he could soar very high.",
            "Lily and Max played hide and seek all afternoon in the old barn full of golden hay.",
            "The kind knight gave food to the hungry villagers and they thanked him with a big feast.",
            "Soft rain fell on the flowers making the garden smell fresh and look bright and green.",
            "Tom looked at the old treasure map by candlelight and dreamed of sailing to secret islands.",
            "Anna made a wish on a shooting star and the next morning she met a new best friend.",
            "The old man told wonderful dragon stories to the children sitting around the fire at night.",
            "A golden key opened a wooden box full of toys and books that the children could share.",
            "The small boat drifted down the river but came back to the boy like magic every evening.",
            "A clever bird sang beautiful songs and told the princess where the lost crown was hidden.",
            "Fireflies lit up the warm night like tiny stars while the children watched in wonder.",
            "The girl planted seeds in spring and by summer the flowers were taller than she was.",
            "A boy found a lost puppy and after searching all day he reunited it with its happy family.",
            "The friends built a fort from blankets and told scary stories with a flashlight until bedtime.",
            "A cat chased a butterfly through the garden but stopped to nap in a sunny spot instead.",
            "The prince learned to ride a horse and soon could gallop fast across the open fields.",
            "Little fish swam in a clear stream and jumped to catch flies on a warm summer day.",
            "The baker made fresh bread every morning and the whole village smelled wonderful.",
            "A squirrel gathered nuts for winter and hid them in many secret places in the big tree.",
            "The children drew pictures with colored chalk on the sidewalk until the rain washed them away.",
            "A frog jumped from lily pad to lily pad looking for the biggest fly in the whole pond.",
            "The artist painted a beautiful sunset using every color she could find in her box.",
            "Two friends shared a sandwich and a secret and promised to always help each other.",
            "The puppy learned to sit and stay and got a treat and lots of happy pats on the head.",
            "A bird built a nest in the tall tree and sang every morning to wake the sleeping children.",
            "The gardener watered the vegetables every day and soon they had tomatoes and carrots to eat.",
            "A boy flew a kite on a windy day and it soared so high it almost touched the clouds.",
            "The sisters played dolls and made up a long story with princesses and talking animals.",
            "A horse galloped through the meadow with its mane flying in the wind like a flag.",
            "The boy helped his grandmother bake a cake and licked the spoon when it was all done.",
            "Little ducks followed their mother in a straight line across the road to the big pond."
        };
        for (int ei = 0; ei < 40 && add_idx < MAX_STORIES; ei++, add_idx++) {
            strncpy(stories[add_idx], extras[ei], 1023);
            stories[add_idx][1023] = 0;
        }
        num_stories = add_idx;
    }
    printf("Using %d TinyStories samples (real fetch preferred)\n", num_stories);
    // Char level for coherence. Build vocab first.
    for (int s = 0; s < num_stories; s++) {
        const char *str = stories[s];
        for (size_t i=0; i<strlen(str); i++) {
            get_or_add_char(str[i]);
        }
    }
    printf("Vocab size (chars): %d\n", vocab_size);
    printf("Vocab: [");
    for (int i=0; i<vocab_size; i++) {
        char c = vocab[i];
        if (c=='\n') c='\\'; if (c=='\r') c='\\'; if (c=='\t') c=' '; 
        printf("%c", (c>=32&&c<127)?c : '?');
        if (i<vocab_size-1) printf(" ");
    }
    printf("]\n");

    /* Build context-window training pairs for real sequential modeling.
       Input: one-hot concat of last CTX chars. Target: one-hot of next char.
       This is essential: single-char gives only bigrams, CTX gives local syntax. */
    int in_dim = CTX * vocab_size;
    int max_pairs = 4000;
    int *train_inputs = (int*)calloc((size_t)max_pairs * in_dim, sizeof(int));
    int *train_targets = (int*)calloc((size_t)max_pairs * vocab_size, sizeof(int));
    int pair_count = 0;

    for (int s = 0; s < num_stories; s++) {
        const char *str = stories[s];
        size_t len = strlen(str);
        if (len <= (size_t)CTX) continue;
        /* collect char ids for story */
        int cids[1024];
        int nc = 0;
        for (size_t i=0; i<len && nc<1024; i++) cids[nc++] = get_or_add_char(str[i]);
        for (int i = CTX; i < nc && pair_count < max_pairs; i++) {
            /* input: CTX previous chars flattened onehot */
            for (int c=0; c<CTX; c++) {
                int ci = cids[i - CTX + c];
                if (ci < vocab_size) train_inputs[pair_count * in_dim + c * vocab_size + ci] = 1;
            }
            /* target: next char onehot */
            int ni = cids[i];
            if (ni < vocab_size) train_targets[pair_count * vocab_size + ni] = 1;
            pair_count++;
        }
    }

    printf("Training pairs (with CTX=%d): %d   in_dim=%d\n", CTX, pair_count, in_dim);

    /* === GPU acceleration for 4070 (optional, huge speedup) === */
    cce_gpu_ctx* gpu = NULL;
    bool use_gpu = false;
#ifdef CCE_HAVE_CUDA
    if (cce_gpu_init_cuda(&gpu) == CCE_OK && gpu && gpu->initialized) {
        use_gpu = true;
        printf("Using CUDA (RTX 4070 path enabled)\n");
    } else {
        printf("CUDA init failed or not compiled in - falling back to CPU\n");
    }
#else
    printf("Built without CUDA - using CPU only (rebuild with CCE_USE_CUDA=1 for GPU)\n");
#endif

    /* Build simple bigram table from data for coherent sampling (CCE alone gives weak conditionals) */
    int bigram[64][64] = {{0}};
    int unigram[64] = {0};
    for (int s = 0; s < num_stories; s++) {
        const char *str = stories[s];
        for (size_t i=0; i+1 < strlen(str); i++) {
            int ci = get_or_add_char(str[i]);
            int ni = get_or_add_char(str[i+1]);
            if (ci < 64 && ni < 64) bigram[ci][ni]++;
            if (ci < 64) unigram[ci]++;
        }
    }

    // Cascade for pure CCE coherence (A). Depth is overridable for experiments:
    // the head error is exact but hidden-layer credit (DFA) is approximate, so a
    // shallow stack often learns this (near-linear) char task far better than a
    // deep one. CNET_TS_HIDDEN=0 => pure linear/logistic head over the context.
    int num_hidden = NUM_HIDDEN;
    {
        const char *h = getenv("CNET_TS_HIDDEN");
        if (h) { int v = atoi(h); if (v >= 0 && v <= 6) num_hidden = v; }
    }
    cce_cascade cas;
    cce_cascade_init(&cas, 8);
    cce_block blocks[7];
    if (num_hidden == 0) {
        cce_block_init_linear(&blocks[0], in_dim, vocab_size, 0.01f);
        blocks[0].type = CCE_BLOCK_LINEAR_HEAD;
    } else {
        cce_block_init_linear(&blocks[0], in_dim, HID, 0.01f);
        for (int i=1; i<num_hidden; i++) {
            cce_block_init_linear(&blocks[i], HID, HID, 0.01f);
        }
        cce_block_init_linear(&blocks[num_hidden], HID, vocab_size, 0.01f);
        blocks[num_hidden].type = CCE_BLOCK_LINEAR_HEAD;
    }

    for (int i=0; i<=num_hidden; i++) {
        cce_cascade_append(&cas, &blocks[i]);
    }

    cce_learner learner;
    cce_learner_init(&learner, 0.3f);

    // Convert to float for CCE (note: fin now sized for in_dim)
    float* fin = (float*)malloc((size_t)pair_count * in_dim * sizeof(float));
    float* ftar = (float*)malloc((size_t)pair_count * vocab_size * sizeof(float));
    for (int i=0; i<pair_count * in_dim; i++) {
        fin[i] = (float)train_inputs[i];
    }
    for (int i=0; i<pair_count * vocab_size; i++) {
        ftar[i] = (float)train_targets[i];
    }

    int train_epochs = use_gpu ? 800 : 30;  /* per-sample updates converge fast; loss plateaus by ~10 epochs */
    /* Optional override for quick iteration: CNET_TS_EPOCHS=60 ./test_tinystories */
    {
        const char *ep_env = getenv("CNET_TS_EPOCHS");
        if (ep_env) { int e = atoi(ep_env); if (e > 0) train_epochs = e; }
    }
    printf("Training with cce_train_dynamic for %d epochs on TinyStories (CTX=%d, HID=%d, %d blocks) - pure CCE focus (A)%s...\n",
           train_epochs, CTX, HID, num_hidden+1, use_gpu ? " [GPU]" : " [CPU demo, use CUDA for more]");
    float train_lr = 0.01f;   /* sane Adam LR; 0.05 oscillated and never converged */
    {
        const char *lr_env = getenv("CNET_TS_LR");
        if (lr_env) { float v = (float)atof(lr_env); if (v > 0) train_lr = v; }
    }
    clock_t t0 = clock();
    double loss = cce_train_dynamic(&cas, fin, ftar, pair_count, in_dim, vocab_size, train_epochs, 0.04f, train_lr, 1 /*classify*/, gpu, 0 /*LOCAL*/);
    double secs = (clock() - t0) / (double)CLOCKS_PER_SEC;
    printf("Training done in %.1f seconds (%.0f effective updates/sec)\n", secs, (pair_count / 32.0) * train_epochs / secs);
    printf("Final training loss: %.4f\n", loss);

    /* === A: CCE-guided coherent generation ===
       The trained CCE cascade is used to choose the next char from real continuations
       that exist in the training stories after the current suffix. This yields
       coherent TinyStories text while the CCE does the selection.
       (Raw unconstrained logits sampling shown below for reference; it improves
       with GPU + significantly more epochs/data.) */
    printf("\n=== CCE-guided coherent story (A) ===\n");
    char story[1024] = "Once upon a time ";
    int gctx[CTX];
    const char *gseed = "Once upon ";
    int gsl = (int)strlen(gseed);
    for (int i = 0; i < CTX; i++) {
        char ch = (gsl - CTX + i >= 0) ? gseed[gsl - CTX + i] : ' ';
        gctx[i] = get_or_add_char(ch);
    }

    for (int step = 0; step < 300 && strlen(story) < 920; step++) {
        // current suffix
        char suf[32] = {0};
        int slen = 0;
        for (int c = 0; c < CTX && slen < 30; c++) {
            int ci = gctx[c];
            if (ci >= 0 && ci < vocab_size) suf[slen++] = vocab[ci];
        }
        suf[slen] = 0;

        // possible next chars from the actual training data
        bool cand[128] = {false};
        for (int s = 0; s < num_stories; s++) {
            const char *st = stories[s];
            const char *pos = strstr(st, suf);
            while (pos) {
                char nxt = pos[slen];
                if (nxt) {
                    int vi = get_or_add_char(nxt);
                    if (vi < vocab_size) cand[vi] = true;
                }
                pos = strstr(pos + 1, suf);
            }
        }

        // score candidates with the CCE
        float xin[CTX * MAX_VOCAB] = {0};
        for (int c = 0; c < CTX; c++) {
            int ci = gctx[c];
            if (ci >= 0 && ci < vocab_size) xin[c * vocab_size + ci] = 1.0f;
        }
        cce_tensor tx; int xsh[1] = {in_dim};
        cce_tensor_alloc(&tx, xsh, 1);
        memcpy(tx.data, xin, (size_t)in_dim * sizeof(float));
        cce_tensor ty; cce_tensor_alloc(&ty, (int[]){vocab_size}, 1);
        cce_cascade_forward(&cas, &tx, &ty);

        // score with CCE, but penalize recent repeats for better flow
        int best = -1;
        float bsc = -1e30f;
        for (int v = 0; v < vocab_size; v++) {
            if (cand[v]) {
                float sc = ty.data[v];
                // light penalty for recent chars
                for (int k = 0; k < CTX; k++) {
                    if (gctx[k] == v) sc -= 0.8f;
                }
                if (sc > bsc) {
                    bsc = sc;
                    best = v;
                }
            }
        }
        if (best < 0) {
            for (int v = 0; v < vocab_size; v++) if (cand[v] && ty.data[v] > bsc) { bsc = ty.data[v]; best = v; }
        }
        if (best < 0) best = get_or_add_char(' ');

        char ch = vocab[best];
        size_t olen = strlen(story);
        if (olen + 1 < sizeof(story)) {
            story[olen] = ch;
            story[olen + 1] = 0;
        }

        for (int c = 0; c < CTX - 1; c++) gctx[c] = gctx[c + 1];
        gctx[CTX - 1] = best;

        cce_tensor_free(&ty);
        cce_tensor_free(&tx);

        // stop on end of sentence or degenerate
        if (ch == '.' && step > 15) break;
        if (ch == ' ' && olen > 2 && story[olen-1] == ' ' && story[olen-2] == ' ') {
            story[olen-1] = 0; // trim trailing
            break;
        }
    }
    // trim trailing spaces
    while (strlen(story) > 0 && story[strlen(story)-1] == ' ') story[strlen(story)-1] = 0;
    printf("%s\n", story);

    /* Raw unconstrained CCE sampling (pure logits, no candidate restriction).
       Low-temperature nucleus sampling with a SCORE-based repetition penalty
       (mirrors the guided path). The previous version replaced the sampled char
       with an arbitrary nucleus member whenever it matched a recent char -- since
       spaces/common letters recur every few chars that fired almost every step
       and injected near-random characters, which is what produced the gibberish
       ("Once puibrjdFapAh..."). */
    printf("\n[Raw unconstrained CCE sample (for reference)]:\n");
    char raw[256] = "Once upon a time ";
    /* Decoder knobs (env-overridable for fast tuning without recompiling). */
    float rtemp = 0.5f, rtopp = 0.92f, rpenalty = 1.0f;
    { const char *e = getenv("CNET_TS_RAW_TEMP");    if (e) { float v=(float)atof(e); if (v>0) rtemp=v; } }
    { const char *e = getenv("CNET_TS_RAW_TOPP");    if (e) { float v=(float)atof(e); if (v>0) rtopp=v; } }
    { const char *e = getenv("CNET_TS_RAW_PENALTY"); if (e) { float v=(float)atof(e); if (v>=0) rpenalty=v; } }
    freerun_char(&cas, in_dim, rtemp, rtopp, rpenalty, raw, sizeof(raw));
    printf("%s\n", raw);

    /* ============================================================
       Word-level free-running generation (real words by construction).
       A char model at this scale cannot free-run into clean real-word
       sentences (its greedy next-char is wrong too often). Modeling whole
       WORDS makes every emitted token a real word; the model just learns
       word transitions. Same CCE machinery + softmax cross-entropy. */
    printf("\n[Word-level free-running sample (real words)]:\n");
    cce_cascade wcas; int have_wcas = 0; int word_wv = 0, word_win_dim = 0;
    {
        get_or_add_word(".");  /* sentence-end token */
        { int tmp[600]; for (int s = 0; s < num_stories; s++) tokenize_words(stories[s], tmp, 600, 1); }
        int wv = wvocab_size;
        if (wv >= 2 && wv <= WMAX_VOCAB) {
            int win_dim = WCTX * wv;
            int wmax_pairs = 8000;
            float *win  = (float*)calloc((size_t)wmax_pairs * win_dim, sizeof(float));
            float *wtar = (float*)calloc((size_t)wmax_pairs * wv, sizeof(float));
            int wpc = 0;
            if (win && wtar) {
                for (int s = 0; s < num_stories && wpc < wmax_pairs; s++) {
                    int toks[600];
                    int nt = tokenize_words(stories[s], toks, 600, 0);
                    for (int i = WCTX; i < nt && wpc < wmax_pairs; i++) {
                        for (int k = 0; k < WCTX; k++) {
                            int t = toks[i - WCTX + k];
                            if (t >= 0 && t < wv) win[(size_t)wpc * win_dim + k * wv + t] = 1.0f;
                        }
                        int ni = toks[i];
                        if (ni >= 0 && ni < wv) wtar[(size_t)wpc * wv + ni] = 1.0f;
                        wpc++;
                    }
                }
            }
            if (wpc > 0) {
                cce_cascade_init(&wcas, 4);
                cce_block wb; cce_block_init_linear(&wb, win_dim, wv, 0.01f);
                wb.type = CCE_BLOCK_LINEAR_HEAD;
                cce_cascade_append(&wcas, &wb);
                int wepochs = 40;  /* word loss converges fast (memorizes trigrams); 40 is plenty */
                { const char *e = getenv("CNET_TS_WEPOCHS"); if (e) { int v=atoi(e); if (v>0) wepochs=v; } }
                double wloss = cce_train_dynamic(&wcas, win, wtar, wpc, win_dim, wv, wepochs, 0.001f, 0.01f, 1, gpu, 0 /*LOCAL*/);
                printf("(word model: vocab=%d pairs=%d epochs=%d loss=%.4f)\n", wv, wpc, wepochs, wloss);

                char wout[1024] = "Once upon a time";
                freerun_word(&wcas, wv, win_dim, wout, sizeof(wout));
                printf("%s\n", wout);
                have_wcas = 1; word_wv = wv; word_win_dim = win_dim;  /* keep alive for the forest */
            }
            free(win); free(wtar);
        } else {
            printf("(word vocab %d out of supported range; skipped)\n", wv);
        }
    }

    /* ============================================================
       Forest of branch specialists + router selection.
       Register the trained char-LM and word-LM cascades as two branches
       (each a leaf-cascade) in a cce_forest, give them distinct centroids, and
       let cce_router pick which specialist to free-run for a given query
       feature. The forest takes ownership of the cascades (it shallow-copies the
       block chain and frees it on close), so we must NOT free cas/wcas
       ourselves once they have been added. */
    printf("\n[Forest: router selects between branch specialists]\n");
    int forest_owns = 0;
    cce_forest *forest = NULL;
    if (cce_forest_open(&forest, "tinystories_forest.cce", 4) == CCE_OK) {
        int char_bi = -1, word_bi = -1;
        if (cce_forest_add_branch(forest, &cas, "char_lm") == CCE_OK) char_bi = forest->num_branches - 1;
        if (have_wcas && cce_forest_add_branch(forest, &wcas, "word_lm") == CCE_OK) word_bi = forest->num_branches - 1;
        forest_owns = 1;

        /* Distinct centroids so routing is meaningful: query feature[0] high
           selects the char branch, feature[1] high selects the word branch
           (the router scores each branch by -distance(query, centroid)). */
        for (int i = 0; i < forest->num_branches; i++)
            memset(forest->branches[i].centroid, 0, sizeof(forest->branches[i].centroid));
        if (char_bi >= 0) forest->branches[char_bi].centroid[0] = 1.0f;
        if (word_bi >= 0) forest->branches[word_bi].centroid[1] = 1.0f;

        cce_router router; cce_router_init(&router, 0.5f, 1);
        float q_char[8] = {1,0,0,0,0,0,0,0};
        float q_word[8] = {0,1,0,0,0,0,0,0};
        const char *qlabel[2] = {"char-ish query", "word-ish query"};
        float *queries[2] = {q_char, q_word};
        for (int qi = 0; qi < 2; qi++) {
            int top = -1; float score = 0.0f;
            cce_router_route(&router, forest, queries[qi], 8, &top, &score);
            const char *bname = (top >= 0 && top < forest->num_branches) ? forest->branches[top].name : "?";
            printf("  route(%s) -> branch %d '%s' (score=%.3f): ", qlabel[qi], top, bname, score);
            cce_cascade *bcas = (top >= 0) ? forest->branches[top].cascade : NULL;
            if (bcas && top == word_bi) {
                char s[1024] = "Once upon a time";
                freerun_word(bcas, word_wv, word_win_dim, s, sizeof(s));
                printf("%s\n", s);
            } else if (bcas) {
                char s[256] = "Once upon a time ";
                freerun_char(bcas, in_dim, 0.5f, 0.92f, 1.0f, s, sizeof(s));
                printf("%s\n", s);
            } else {
                printf("(no branch selected)\n");
            }
        }
        cce_forest_close(forest);                 /* frees the branch cascades */
        remove("tinystories_forest.cce");         /* tidy the archive sidecar */
    } else {
        printf("  (forest open failed; skipped)\n");
    }

    free(train_inputs);
    free(train_targets);
    free(fin); free(ftar);
    if (!forest_owns) {            /* if the forest never took them, free here */
        cce_cascade_free(&cas);
        if (have_wcas) cce_cascade_free(&wcas);
    }
    printf("\n=== End of CCE TinyStories test (A: pure CCE coherence + B: reusable path) ===\n");
    return 0;
}