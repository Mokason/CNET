/* Word-level tokenizer, vocabulary and text scorer for the recurrent LM (see cnet_vsa_rlm.h). */
#include "cnet_vsa_rlm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

void cnet_vsa_rlm_tokenize(const char *text, cnet_vsa_rlm_token_cb cb, void *ctx) {
    char buf[CNET_VSA_RLM_TOKEN_MAX]; int bl = 0;
    if (!text) return;
    for (const unsigned char *p = (const unsigned char *)text;; ++p) {
        int c = *p; int word = c && (isalnum(c) || c == '\'' || c >= 128);
        if (word) { if (bl < CNET_VSA_RLM_TOKEN_MAX - 1) buf[bl++] = (char)tolower(c); continue; }
        if (bl) { buf[bl] = 0; cb(buf, ctx); bl = 0; }
        if (!c) break;
        if (!isspace(c)) { char t[2] = { (char)c, 0 }; cb(t, ctx); }
    }
}

typedef struct { char key[CNET_VSA_RLM_TOKEN_MAX]; int id; } VEnt;
struct cnet_vsa_rlm_vocab { char **word; int n; VEnt *map; size_t cap; };

static uint64_t fnv(const char *s) { uint64_t h = 1469598103934665603ULL; while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; } return h; }
static int *vmap_slot(cnet_vsa_rlm_vocab *v, const char *k, int insert) {
    size_t i = fnv(k) & (v->cap - 1);
    for (;;) {
        if (v->map[i].id < 0) { if (!insert) return NULL; strncpy(v->map[i].key, k, CNET_VSA_RLM_TOKEN_MAX - 1); return &v->map[i].id; }
        if (!strcmp(v->map[i].key, k)) return &v->map[i].id;
        i = (i + 1) & (v->cap - 1);
    }
}
static void vocab_index(cnet_vsa_rlm_vocab *v) {
    v->cap = 1024; while (v->cap < (size_t)v->n * 2) v->cap *= 2;
    v->map = (VEnt *)calloc(v->cap, sizeof(VEnt)); for (size_t i = 0; i < v->cap; ++i) v->map[i].id = -1;
    for (int i = 0; i < v->n; ++i) { int *id = vmap_slot(v, v->word[i], 1); if (*id < 0) *id = i; }
}
cnet_vsa_rlm_vocab *cnet_vsa_rlm_vocab_load(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return NULL;
    cnet_vsa_rlm_vocab *v = (cnet_vsa_rlm_vocab *)calloc(1, sizeof(*v)); int cap = 1024; v->word = (char **)malloc(sizeof(char *) * cap); char line[CNET_VSA_RLM_TOKEN_MAX + 8];
    while (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; if (v->n == cap) { cap *= 2; v->word = (char **)realloc(v->word, sizeof(char *) * cap); } v->word[v->n++] = strdup(line); }
    fclose(f);
    vocab_index(v);
    return v;
}
cnet_vsa_rlm_vocab *cnet_vsa_rlm_vocab_from_words(char **words, int n) {
    if (!words || n < 2) return NULL;
    cnet_vsa_rlm_vocab *v = (cnet_vsa_rlm_vocab *)calloc(1, sizeof(*v)); v->word = (char **)malloc(sizeof(char *) * n); v->n = n;
    for (int i = 0; i < n; ++i) v->word[i] = strdup(words[i]);
    vocab_index(v);
    return v;
}
int cnet_vsa_rlm_vocab_save(const cnet_vsa_rlm_vocab *v, const char *path) {
    FILE *f = fopen(path, "w"); if (!f) return -1;
    for (int i = 0; i < v->n; ++i) fprintf(f, "%s\n", v->word[i]);
    fclose(f); return 0;
}
void cnet_vsa_rlm_vocab_free(cnet_vsa_rlm_vocab *v) { if (!v) return; for (int i = 0; i < v->n; ++i) free(v->word[i]); free(v->word); free(v->map); free(v); }
int cnet_vsa_rlm_vocab_size(const cnet_vsa_rlm_vocab *v) { return v ? v->n : 0; }
int cnet_vsa_rlm_vocab_id(const cnet_vsa_rlm_vocab *v, const char *word) { int *id = vmap_slot((cnet_vsa_rlm_vocab *)v, word, 0); return id ? *id : 0; }
const char *cnet_vsa_rlm_vocab_word(const cnet_vsa_rlm_vocab *v, int id) { return (v && id >= 0 && id < v->n) ? v->word[id] : "<unk>"; }
typedef struct { const cnet_vsa_rlm_vocab *v; int *ids, n, cap, unk; } EncCtx;
static void enc_cb(const char *tok, void *ctx) { EncCtx *e = (EncCtx *)ctx; if (e->n >= e->cap) return; int id = cnet_vsa_rlm_vocab_id(e->v, tok); if (id == 0) e->unk++; e->ids[e->n++] = id; }
int cnet_vsa_rlm_vocab_encode(const cnet_vsa_rlm_vocab *v, const char *text, int *ids, int cap, int *unk_count) {
    EncCtx e = { v, ids, 0, cap, 0 }; cnet_vsa_rlm_tokenize(text, enc_cb, &e); if (unk_count) *unk_count = e.unk; return e.n;
}

struct cnet_vsa_rlm_scorer { cnet_vsa_rlm *m; cnet_vsa_rlm_vocab *v; int *ids; unsigned char *mask; int cap; };

cnet_vsa_rlm_scorer *cnet_vsa_rlm_scorer_load(const char *model_path, const char *vocab_path) {
    cnet_vsa_rlm *m = cnet_vsa_rlm_load(model_path); if (!m) return NULL;
    cnet_vsa_rlm_vocab *v = cnet_vsa_rlm_vocab_load(vocab_path); if (!v || cnet_vsa_rlm_vocab_size(v) != cnet_vsa_rlm_config(m)->vocab) { cnet_vsa_rlm_free(m); cnet_vsa_rlm_vocab_free(v); return NULL; }
    cnet_vsa_rlm_scorer *s = (cnet_vsa_rlm_scorer *)calloc(1, sizeof(*s)); s->m = m; s->v = v; s->cap = 4096; s->ids = (int *)malloc(sizeof(int) * s->cap); s->mask = (unsigned char *)malloc((size_t)s->cap);
    return s;
}
cnet_vsa_rlm_scorer *cnet_vsa_rlm_scorer_load_dir(const char *dir) {
    char mp[1024], vp[1024]; snprintf(mp, sizeof mp, "%s/model_best.rlm", dir); snprintf(vp, sizeof vp, "%s/vocab.txt", dir);
    FILE *f = fopen(mp, "rb"); if (f) fclose(f); else snprintf(mp, sizeof mp, "%s/model.rlm", dir);
    return cnet_vsa_rlm_scorer_load(mp, vp);
}
void cnet_vsa_rlm_scorer_free(cnet_vsa_rlm_scorer *s) { if (!s) return; cnet_vsa_rlm_free(s->m); cnet_vsa_rlm_vocab_free(s->v); free(s->ids); free(s->mask); free(s); }
const cnet_vsa_rlm *cnet_vsa_rlm_scorer_model(const cnet_vsa_rlm_scorer *s) { return s ? s->m : NULL; }

double cnet_vsa_rlm_scorer_nll(void *scorer, const char *prefix, const char *text, int *ntok) {
    cnet_vsa_rlm_scorer *s = (cnet_vsa_rlm_scorer *)scorer;
    if (ntok) *ntok = 0;
    if (!s || !text) return -1;
    int n = 0; s->ids[n++] = 1;
    if (prefix) n += cnet_vsa_rlm_vocab_encode(s->v, prefix, s->ids + n, s->cap - n - 2, NULL);
    int start = n;   /* index of the first text token */
    n += cnet_vsa_rlm_vocab_encode(s->v, text, s->ids + n, s->cap - n - 1, NULL);
    if (n == start) return -1;
    s->ids[n++] = 1;   /* closing <eos> is scored too */
    /* position t predicts ids[t+1]; score positions start-1 .. n-2 */
    for (int t = 0; t + 1 < n; ++t) s->mask[t] = (unsigned char)(t + 1 >= start);
    const int T = cnet_vsa_rlm_config(s->m)->chunk; double total = 0; int scored = n - start;
    cnet_vsa_rlm_reset_state(s->m);
    for (int pos = 0; pos + 1 < n; pos += T) {
        int k = (n - 1 - pos) < T ? (n - 1 - pos) : T, cnt = 0; for (int t = 0; t < k; ++t) cnt += s->mask[pos + t];
        if (cnt) total += cnet_vsa_rlm_chunk_masked(s->m, s->ids + pos, k, s->mask + pos, 0) * cnt;
        else cnet_vsa_rlm_chunk(s->m, s->ids + pos, k, 0);   /* prefix only: advance the state */
    }
    if (ntok) *ntok = scored;
    return total / scored;
}
