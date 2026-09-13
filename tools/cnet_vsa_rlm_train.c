/* cnet_vsa_rlm: train / evaluate / generate with the recurrent LM trainer (Gated DeltaNet, RWKV-7, SSD).
 *
 *   cnet_vsa_rlm train --train-dir <dir of *_corpus.txt> --test <corpora.tsv> --out <model dir>
 *                      [--mixer deltanet|rwkv7|ssd] [--d 256] [--heads 4] [--layers 2] [--ff 512] [--chunk 64]
 *                      [--streams 16] [--epochs 4] [--lr 1e-3] [--wd 0.01] [--clip 1.0] [--vocab 8192] [--seed 1]
 *   cnet_vsa_rlm eval --model <dir> --test <corpora.tsv>
 *   cnet_vsa_rlm ngram --train-dir <dir> --test <corpora.tsv> [--vocab 8192]        interpolated trigram baseline
 *   cnet_vsa_rlm generate --model <dir> --prompt "<text>" [--max 60] [--temp 0.8] [--topk 40] [--seed 1]
 *   cnet_vsa_rlm score --model <dir> --text "<text>"                                 per-token log-loss of a text
 *   cnet_vsa_rlm generate-batch --model <dir> --in <id\tprompt tsv> [--max 60] [--temp 0.8] [--topk 40] [--seed 7]
 *                               [--prime <corpus dir> --prime-max 512]   feed <dir>/<id>_corpus.txt into the state first
 *   cnet_vsa_rlm score-batch --model <dir> --in <id\ttext tsv>                        id, tokens, mean loss, perplexity per line
 *
 * Word-level vocabulary: lowercase words [a-z0-9'] plus single punctuation tokens; <unk> = 0, <eos> = 1.
 * Held-out perplexity: the test rows of corpora.tsv, concatenated per corpus, state reset per corpus. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _OPENMP
#include <omp.h>
#endif
#include "cnet_vsa_rlm.h"

#define MAXTOK CNET_VSA_RLM_TOKEN_MAX
typedef struct { char key[MAXTOK]; int id; } HEnt;
typedef struct { HEnt *e; size_t cap; } HMap;

static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; }
static uint64_t fnv(const char *s) { uint64_t h = 1469598103934665603ULL; while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; } return h; }
static void hmap_init(HMap *m, size_t cap) { m->cap = cap; m->e = (HEnt *)calloc(cap, sizeof(HEnt)); for (size_t i = 0; i < cap; ++i) m->e[i].id = -1; }
static int *hmap_slot(HMap *m, const char *k, int insert) {
    size_t i = fnv(k) & (m->cap - 1);
    for (;;) { if (m->e[i].id < 0) { if (!insert) return NULL; strncpy(m->e[i].key, k, MAXTOK - 1); return &m->e[i].id; } if (!strcmp(m->e[i].key, k)) return &m->e[i].id; i = (i + 1) & (m->cap - 1); }
}
/* ---- counting for the vocabulary (the tokenizer itself is the library's) ---- */
typedef struct { HMap map; char **words; long *counts; int n, cap; } Counter;
static void count_cb(const char *tok, void *ctx) {
    Counter *c = (Counter *)ctx; int *id = hmap_slot(&c->map, tok, 1);
    if (*id < 0) { if (c->n == c->cap) { c->cap = c->cap ? c->cap * 2 : 4096; c->words = (char **)realloc(c->words, sizeof(char *) * c->cap); c->counts = (long *)realloc(c->counts, sizeof(long) * c->cap); } *id = c->n; c->words[c->n] = strdup(tok); c->counts[c->n] = 0; c->n++; }
    c->counts[*id]++;
}
static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL; fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1); if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; } b[n] = 0; fclose(f); if (len) *len = (size_t)n; return b;
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static char **list_dir(const char *dir, int *count) {
    DIR *d = opendir(dir); if (!d) return NULL; char **v = NULL; int n = 0, cap = 0; struct dirent *e;
    while ((e = readdir(d))) { size_t L = strlen(e->d_name); if (L > 4 && !strcmp(e->d_name + L - 4, ".txt")) { if (n == cap) { cap = cap ? cap * 2 : 256; v = (char **)realloc(v, sizeof(char *) * cap); } char *p = (char *)malloc(strlen(dir) + L + 2); sprintf(p, "%s/%s", dir, e->d_name); v[n++] = p; } }
    closedir(d); qsort(v, (size_t)n, sizeof(char *), cmp_str); *count = n; return v;
}
static int cmp_count(const void *a, const void *b, void *ctx) { const long *c = (const long *)ctx; long d = c[*(const int *)b] - c[*(const int *)a]; return d > 0 ? 1 : d < 0 ? -1 : (*(const int *)a - *(const int *)b); }
typedef cnet_vsa_rlm_vocab Vocabulary;
static int vocab_id(const Vocabulary *v, const char *w) { return cnet_vsa_rlm_vocab_id(v, w); }
static Vocabulary *vocab_build(char **files, int nf, int size) {
    Counter c; memset(&c, 0, sizeof(c)); hmap_init(&c.map, 1u << 18);
    for (int i = 0; i < nf; ++i) { char *t = read_file(files[i], NULL); if (t) { cnet_vsa_rlm_tokenize(t, count_cb, &c); free(t); } }
    int *order = (int *)malloc(sizeof(int) * c.n); for (int i = 0; i < c.n; ++i) order[i] = i;
    qsort_r(order, (size_t)c.n, sizeof(int), cmp_count, c.counts);
    char **words = (char **)malloc(sizeof(char *) * size); int n = 0;
    words[n++] = "<unk>"; words[n++] = "<eos>";
    long covered = 0, total = 0; for (int i = 0; i < c.n; ++i) total += c.counts[i];
    for (int i = 0; i < c.n && n < size; ++i) { words[n++] = c.words[order[i]]; covered += c.counts[order[i]]; }
    fprintf(stderr, "vocab: %d types in training text, kept %d, token coverage %.2f%%\n", c.n, n, 100.0 * covered / total);
    Vocabulary *v = cnet_vsa_rlm_vocab_from_words(words, n);
    free(words); free(order); for (int i = 0; i < c.n; ++i) free(c.words[i]); free(c.words); free(c.counts); free(c.map.e);
    return v;
}
static int vocab_save(const Vocabulary *v, const char *path) { return cnet_vsa_rlm_vocab_save(v, path); }
static Vocabulary *vocab_load(const char *path) { return cnet_vsa_rlm_vocab_load(path); }
/* ---- token streams ---- */
typedef struct { int *ids; long n, cap; Vocabulary *v; long unk; } Ids;
static void ids_push(Ids *s, int id) { if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 65536; s->ids = (int *)realloc(s->ids, sizeof(int) * s->cap); } s->ids[s->n++] = id; }
static void ids_cb(const char *tok, void *ctx) { Ids *s = (Ids *)ctx; int id = vocab_id(s->v, tok); if (id == 0) s->unk++; ids_push(s, id); }
static void ids_text(Ids *s, const char *text) { cnet_vsa_rlm_tokenize(text, ids_cb, s); }
/* held-out: corpora.tsv rows "name\ttest\tsentence", grouped per corpus (rows of one corpus are adjacent) */
typedef struct { char name[160]; Ids ids; } TestCorpus;
static TestCorpus *load_test(const char *path, Vocabulary *v, int *count) {
    char *t = read_file(path, NULL); if (!t) return NULL; TestCorpus *tc = NULL; int n = 0, cap = 0; char *save = NULL;
    for (char *line = strtok_r(t, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *tab1 = strchr(line, '\t'); if (!tab1) continue; char *tab2 = strchr(tab1 + 1, '\t'); if (!tab2) continue;
        if (strncmp(tab1 + 1, "test", 4)) continue;
        *tab1 = 0;
        if (n == 0 || strcmp(tc[n - 1].name, line)) { if (n == cap) { cap = cap ? cap * 2 : 256; tc = (TestCorpus *)realloc(tc, sizeof(TestCorpus) * cap); } memset(&tc[n], 0, sizeof(TestCorpus)); strncpy(tc[n].name, line, 159); tc[n].ids.v = v; n++; }
        ids_text(&tc[n - 1].ids, tab2 + 1);
    }
    for (int i = 0; i < n; ++i) ids_push(&tc[i].ids, 1);
    free(t); *count = n; return tc;
}
/* ---- perplexity of a model on the held-out corpora ---- */
static double eval_ppl(cnet_vsa_rlm *m, TestCorpus *tc, int n, long *ntok_out, double *acc_out) {
    const cnet_vsa_rlm_cfg *cfg = cnet_vsa_rlm_config(m); int T = cfg->chunk;
    double total = 0; long ntok = 0, correct = 0;
    for (int c = 0; c < n; ++c) {
        cnet_vsa_rlm_reset_state(m); Ids *s = &tc[c].ids; if (s->n < 2) continue;
        /* eos context first so the first word is predicted from a boundary */
        int *buf = (int *)malloc(sizeof(int) * (s->n + 1)); buf[0] = 1; memcpy(buf + 1, s->ids, sizeof(int) * s->n); long L = s->n + 1;
        for (long pos = 0; pos + 1 < L; pos += T) { int k = (int)((L - 1 - pos) < T ? (L - 1 - pos) : T); double l = cnet_vsa_rlm_chunk(m, buf + pos, k, 0); total += l * k; ntok += k; }
        (void)correct; free(buf);
    }
    if (ntok_out) *ntok_out = ntok;
    if (acc_out) *acc_out = 0;
    return exp(total / ntok);
}
/* ---- interpolated trigram baseline (absolute discounting, D = 0.75, uniform floor) ---- */
typedef struct { uint64_t key; long count; } NEnt;
typedef struct { NEnt *e; size_t cap, n; } NMap;
static void nmap_init(NMap *m, size_t cap) { m->cap = cap; m->n = 0; m->e = (NEnt *)calloc(cap, sizeof(NEnt)); }
static long *nmap_get(NMap *m, uint64_t key, int insert) {
    uint64_t h = key * 0x9E3779B97F4A7C15ULL; size_t i = (size_t)(h >> 20) & (m->cap - 1);
    for (;;) { if (m->e[i].count == 0 && m->e[i].key == 0) { if (!insert) return NULL; m->e[i].key = key; m->n++; return &m->e[i].count; } if (m->e[i].key == key) return &m->e[i].count; i = (i + 1) & (m->cap - 1); }
}
#define K1(a) ((uint64_t)(a) + 1)
#define K2(a, b) ((K1(a) << 20) | ((uint64_t)(b) + 1))
#define K3(a, b, c) ((K2(a, b) << 20) | ((uint64_t)(c) + 1))
static double ngram_baseline(Ids *train, TestCorpus *tc, int ntc, int V) {
    NMap uni, bi, tri, bictx, trictx; nmap_init(&uni, 1u << 14); nmap_init(&bi, 1u << 22); nmap_init(&tri, 1u << 24); nmap_init(&bictx, 1u << 14); nmap_init(&trictx, 1u << 22);
    const int *x = train->ids; long n = train->n;
    for (long i = 0; i < n; ++i) { (*nmap_get(&uni, K1(x[i]), 1))++; if (i >= 1) { (*nmap_get(&bi, K2(x[i - 1], x[i]), 1))++; (*nmap_get(&bictx, K1(x[i - 1]), 1))++; } if (i >= 2) { (*nmap_get(&tri, K3(x[i - 2], x[i - 1], x[i]), 1))++; (*nmap_get(&trictx, K2(x[i - 2], x[i - 1]), 1))++; } }
    /* distinct-continuation counts for the discount mass: approximate by counting distinct followers per context */
    NMap bidist, tridist; nmap_init(&bidist, 1u << 14); nmap_init(&tridist, 1u << 22);
    for (size_t i = 0; i < bi.cap; ++i) if (bi.e[i].count) (*nmap_get(&bidist, bi.e[i].key >> 20, 1))++;
    for (size_t i = 0; i < tri.cap; ++i) if (tri.e[i].count) (*nmap_get(&tridist, tri.e[i].key >> 20, 1))++;
    const double D = 0.75; double total = 0; long ntok = 0;
    for (int c = 0; c < ntc; ++c) {
        Ids *s = &tc[c].ids; int prev2 = 1, prev1 = 1;   /* eos context */
        for (long i = 0; i < s->n; ++i) {
            int w = s->ids[i];
            long *cu = nmap_get(&uni, K1(w), 0); double p1 = ((cu ? *cu : 0) + 1.0) / (n + V);   /* add-one unigram */
            long *cb = nmap_get(&bictx, K1(prev1), 0), *cbw = nmap_get(&bi, K2(prev1, w), 0), *nb = nmap_get(&bidist, K1(prev1), 0);
            double p2 = cb ? ((cbw ? *cbw : 0) - D > 0 ? (*cbw - D) / *cb : 0) + (D * (nb ? *nb : 0) / *cb) * p1 : p1;
            long *ct = nmap_get(&trictx, K2(prev2, prev1), 0), *ctw = nmap_get(&tri, K3(prev2, prev1, w), 0), *nt = nmap_get(&tridist, K2(prev2, prev1), 0);
            double p3 = ct ? ((ctw ? *ctw : 0) - D > 0 ? (*ctw - D) / *ct : 0) + (D * (nt ? *nt : 0) / *ct) * p2 : p2;
            total += -log(p3); ntok++; prev2 = prev1; prev1 = w;
        }
    }
    free(uni.e); free(bi.e); free(tri.e); free(bictx.e); free(trictx.e); free(bidist.e); free(tridist.e);
    return exp(total / ntok);
}
/* ---- generation ---- */
static uint64_t grng = 88172645463325252ULL;
static double urand(void) { grng ^= grng << 13; grng ^= grng >> 7; grng ^= grng << 17; return (double)(grng >> 11) / 9007199254740992.0; }
static int sample(rlm_real *logits, int V, double temp, int topk) {
    int *idx = (int *)malloc(sizeof(int) * V); for (int i = 0; i < V; ++i) idx[i] = i;
    logits[0] = -1e30f;   /* never emit <unk> */
    /* partial selection of the top-k by repeated max (k small) */
    if (topk < 1 || topk > V) topk = V;
    for (int k = 0; k < topk; ++k) { int best = k; for (int i = k + 1; i < V; ++i) if (logits[idx[i]] > logits[idx[best]]) best = i; int t = idx[k]; idx[k] = idx[best]; idx[best] = t; }
    double mx = logits[idx[0]], sum = 0; double *pr = (double *)malloc(sizeof(double) * topk);
    for (int k = 0; k < topk; ++k) { pr[k] = exp((logits[idx[k]] - mx) / temp); sum += pr[k]; }
    double r = urand() * sum, acc = 0; int pick = idx[topk - 1];
    for (int k = 0; k < topk; ++k) { acc += pr[k]; if (r <= acc) { pick = idx[k]; break; } }
    free(idx); free(pr); return pick;
}
static void print_tokens(Vocabulary *v, const int *ids, int n, FILE *out) {
    int prev_hyphen = 0;
    for (int i = 0; i < n; ++i) { const char *w = cnet_vsa_rlm_vocab_word(v, ids[i]); int punct = strlen(w) == 1 && ispunct((unsigned char)w[0]); if (i && !punct && !prev_hyphen) fputc(' ', out); fputs(w, out); prev_hyphen = !strcmp(w, "-"); }
}
static const char *mixer_name(int m) { return m == 0 ? "deltanet" : m == 1 ? "rwkv7" : "ssd"; }
static int mixer_id(const char *s) { return !strcmp(s, "deltanet") ? 0 : !strcmp(s, "rwkv7") ? 1 : !strcmp(s, "ssd") ? 2 : -1; }
static const char *arg(int argc, char **argv, const char *k, const char *def) { for (int i = 2; i + 1 < argc; ++i) if (!strcmp(argv[i], k)) return argv[i + 1]; return def; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: see header of tools/cnet_vsa_rlm_train.c\n"); return 2; }
    const char *cmd = argv[1];
    int streams = 16;
    if (!strcmp(cmd, "train")) {
        const char *value = arg(argc, argv, "--streams", "16"); char *end = NULL;
        long parsed = strtol(value, &end, 10);
        if (end == value || *end || parsed < 1 || parsed > 256) {
            fprintf(stderr, "streams must be an integer in [1,256]\n"); return 2;
        }
        streams = (int)parsed;
    }
    const char *train_dir = arg(argc, argv, "--train-dir", "var/arena_cache/cnet/train"), *test = arg(argc, argv, "--test", "benchmarks/vsa_routing_arena_20260911/corpora.tsv");
    const char *model_dir = arg(argc, argv, "--model", NULL), *out = arg(argc, argv, "--out", NULL);
    int vocab_size = atoi(arg(argc, argv, "--vocab", "8192"));
    if (!strcmp(cmd, "train") || !strcmp(cmd, "ngram")) {
        int nf = 0; char **files = list_dir(train_dir, &nf); if (!files || nf == 0) { fprintf(stderr, "no training files in %s\n", train_dir); return 1; }
        Vocabulary *v = vocab_build(files, nf, vocab_size);
        Ids stream; memset(&stream, 0, sizeof stream); stream.v = v;
        for (int i = 0; i < nf; ++i) { char *t = read_file(files[i], NULL); if (t) { ids_text(&stream, t); ids_push(&stream, 1); free(t); } }
        int ntc = 0; TestCorpus *tc = load_test(test, v, &ntc); if (!tc) { fprintf(stderr, "cannot read %s\n", test); return 1; }
        long test_tok = 0, test_unk = 0; for (int i = 0; i < ntc; ++i) { test_tok += tc[i].ids.n; test_unk += tc[i].ids.unk; }
        printf("data: %d corpora, %ld training tokens (%.2f%% unk), %d held-out corpora, %ld held-out tokens (%.2f%% unk), vocab %d\n", nf, stream.n, 100.0 * stream.unk / stream.n, ntc, test_tok, 100.0 * test_unk / test_tok, cnet_vsa_rlm_vocab_size(v));
        if (!strcmp(cmd, "ngram")) { double t0 = now(); double ppl = ngram_baseline(&stream, tc, ntc, cnet_vsa_rlm_vocab_size(v)); printf("trigram baseline (absolute discounting): held-out perplexity %.2f  (%.1f s)\n", ppl, now() - t0); return 0; }
        if (!out) { fprintf(stderr, "--out required\n"); return 2; }
        cnet_vsa_rlm_cfg cfg = { cnet_vsa_rlm_vocab_size(v), atoi(arg(argc, argv, "--d", "256")), atoi(arg(argc, argv, "--heads", "4")), atoi(arg(argc, argv, "--layers", "2")), atoi(arg(argc, argv, "--ff", "0")), mixer_id(arg(argc, argv, "--mixer", "deltanet")), atoi(arg(argc, argv, "--chunk", "64")), (unsigned)atoi(arg(argc, argv, "--seed", "1")), (float)atof(arg(argc, argv, "--wd", "0.01")) };
        if (cfg.mixer < 0) { fprintf(stderr, "unknown mixer\n"); return 2; }
        int epochs = atoi(arg(argc, argv, "--epochs", "4")); double lr = atof(arg(argc, argv, "--lr", "1e-3")), clip = atof(arg(argc, argv, "--clip", "1.0"));
        cnet_vsa_rlm *master = cnet_vsa_rlm_create(&cfg); if (!master) { fprintf(stderr, "bad config\n"); return 2; }
        cnet_vsa_rlm **wk = (cnet_vsa_rlm **)malloc(sizeof(void *) * streams); for (int b = 0; b < streams; ++b) wk[b] = cnet_vsa_rlm_clone_shared(master);
        long seg = stream.n / streams, steps_per_epoch = (seg - 1) / cfg.chunk, total_steps = steps_per_epoch * epochs, step = 0;
        printf("model: %s d=%d heads=%d layers=%d ff=%d chunk=%d params=%zu (%.1f MB fp32); %d streams x %ld tokens, %ld steps/epoch, lr %.2e, wd %.3f, clip %.1f\n", mixer_name(cfg.mixer), cfg.d_model, cfg.n_head, cfg.n_layer, cfg.d_ff > 0 ? cfg.d_ff : 2 * cfg.d_model, cfg.chunk, cnet_vsa_rlm_param_count(master), cnet_vsa_rlm_param_count(master) * 4 / 1048576.0, streams, seg, steps_per_epoch, lr, cfg.weight_decay, clip);
        mkdir(out, 0755); char path[1024]; snprintf(path, sizeof path, "%s/vocab.txt", out); vocab_save(v, path);
        double ppl0 = eval_ppl(master, tc, ntc, NULL, NULL); printf("epoch 0: held-out perplexity %.2f (untrained)\n", ppl0); fflush(stdout);
        double train_time = 0, best = 1e300; long train_tokens = 0;
        for (int ep = 1; ep <= epochs; ++ep) {
            for (int b = 0; b < streams; ++b) cnet_vsa_rlm_reset_state(wk[b]);
            double ep_loss = 0, t0 = now(); long ep_tok = 0;
            for (long s = 0; s < steps_per_epoch; ++s) {
                double losses[256];
#pragma omp parallel for schedule(static, 1)
                for (int b = 0; b < streams; ++b) { cnet_vsa_rlm_zero_grad(wk[b]); losses[b] = cnet_vsa_rlm_chunk(wk[b], stream.ids + b * seg + s * cfg.chunk, cfg.chunk, 1); }
                cnet_vsa_rlm_zero_grad(master); for (int b = 0; b < streams; ++b) { cnet_vsa_rlm_reduce_grad(master, wk[b], 1.0 / streams); ep_loss += losses[b]; }
                cnet_vsa_rlm_grad_clip(master, clip);
                step++; double warm = step < 200 ? step / 200.0 : 1.0, prog = (double)step / total_steps, lr_t = lr * warm * (0.1 + 0.9 * 0.5 * (1 + cos(3.14159265 * prog)));
                cnet_vsa_rlm_adam(master, (float)lr_t, (int)step);
                ep_tok += (long)streams * cfg.chunk;
                if ((s + 1) % 100 == 0 || s + 1 == steps_per_epoch) { printf("  epoch %d step %ld/%ld  train loss %.3f  lr %.2e  %.0f tok/s\n", ep, s + 1, steps_per_epoch, ep_loss / ((s + 1) * streams), lr_t, ep_tok / (now() - t0)); fflush(stdout); }
            }
            train_time += now() - t0; train_tokens += ep_tok;
            double te = now(), ppl = eval_ppl(master, tc, ntc, NULL, NULL);
            printf("epoch %d: train loss %.3f  held-out perplexity %.2f  (train %.0f s at %.0f tok/s, eval %.1f s)\n", ep, ep_loss / (steps_per_epoch * streams), ppl, train_time, train_tokens / train_time, now() - te); fflush(stdout);
            snprintf(path, sizeof path, "%s/model.rlm", out); cnet_vsa_rlm_save(master, path);
            if (ppl < best) { best = ppl; snprintf(path, sizeof path, "%s/model_best.rlm", out); cnet_vsa_rlm_save(master, path); }   /* best held-out epoch */
        }
        printf("best held-out perplexity %.2f (model_best.rlm); final epoch in model.rlm\n", best);
        return 0;
    }
    if (!model_dir) { fprintf(stderr, "--model required\n"); return 2; }
    char path[1024]; snprintf(path, sizeof path, "%s/model.rlm", model_dir); cnet_vsa_rlm *m = cnet_vsa_rlm_load(path); if (!m) { fprintf(stderr, "cannot load %s\n", path); return 1; }
    snprintf(path, sizeof path, "%s/vocab.txt", model_dir); Vocabulary *v = vocab_load(path); if (!v) { fprintf(stderr, "cannot load %s\n", path); return 1; }
    const cnet_vsa_rlm_cfg *cfg = cnet_vsa_rlm_config(m);
    if (!strcmp(cmd, "eval")) {
        int ntc = 0; TestCorpus *tc = load_test(test, v, &ntc); if (!tc) return 1; long ntok = 0; double t0 = now();
        double ppl = eval_ppl(m, tc, ntc, &ntok, NULL);
        printf("%s: held-out perplexity %.2f over %ld tokens (%d corpora), %.1f s, %zu params\n", mixer_name(cfg->mixer), ppl, ntok, ntc, now() - t0, cnet_vsa_rlm_param_count(m));
        return 0;
    }
    if (!strcmp(cmd, "generate")) {
        const char *prompt = arg(argc, argv, "--prompt", ""); int maxn = atoi(arg(argc, argv, "--max", "60")), topk = atoi(arg(argc, argv, "--topk", "40")); double temp = atof(arg(argc, argv, "--temp", "0.8"));
        grng ^= (uint64_t)atoi(arg(argc, argv, "--seed", "1")) * 0x9E3779B97F4A7C15ULL;
        Ids p; memset(&p, 0, sizeof p); p.v = v; ids_push(&p, 1); ids_text(&p, prompt);
        rlm_real *logits = (rlm_real *)malloc(sizeof(rlm_real) * cfg->vocab); int *outids = (int *)malloc(sizeof(int) * (maxn + 1)); int n = 0;
        cnet_vsa_rlm_reset_state(m); double t0 = now();
        cnet_vsa_rlm_predict(m, p.ids, (int)p.n, logits);
        for (; n < maxn; ++n) { int t = sample(logits, cfg->vocab, temp, topk); if (t == 1) break; outids[n] = t; cnet_vsa_rlm_predict(m, &t, 1, logits); }
        double dt = now() - t0;
        print_tokens(v, p.ids + 1, (int)p.n - 1, stdout); printf(" |"); if (n) { printf(" "); print_tokens(v, outids, n, stdout); } printf("\n");
        fprintf(stderr, "%d tokens in %.1f ms (%.0f tok/s)\n", n, dt * 1e3, n / dt);
        return 0;
    }
    if (!strcmp(cmd, "score")) {
        const char *text = arg(argc, argv, "--text", ""); Ids p; memset(&p, 0, sizeof p); p.v = v; ids_push(&p, 1); ids_text(&p, text); ids_push(&p, 1);
        cnet_vsa_rlm_reset_state(m); double total = 0; long n = 0;
        for (long pos = 0; pos + 1 < p.n; pos += cfg->chunk) { int k = (int)((p.n - 1 - pos) < cfg->chunk ? (p.n - 1 - pos) : cfg->chunk); total += cnet_vsa_rlm_chunk(m, p.ids + pos, k, 0) * k; n += k; }
        printf("tokens %ld  mean loss %.3f  perplexity %.2f  unk %ld\n", n, total / n, exp(total / n), p.unk);
        return 0;
    }
    if (!strcmp(cmd, "generate-batch") || !strcmp(cmd, "score-batch")) {
        const char *in = arg(argc, argv, "--in", NULL); if (!in) { fprintf(stderr, "--in required\n"); return 2; }
        char *t = read_file(in, NULL); if (!t) { fprintf(stderr, "cannot read %s\n", in); return 1; }
        int gen = !strcmp(cmd, "generate-batch"), maxn = atoi(arg(argc, argv, "--max", "60")), topk = atoi(arg(argc, argv, "--topk", "40")); double temp = atof(arg(argc, argv, "--temp", "0.8"));
        const char *prime = arg(argc, argv, "--prime", NULL); int prime_max = atoi(arg(argc, argv, "--prime-max", "512")); long primed = 0;
        grng ^= (uint64_t)atoi(arg(argc, argv, "--seed", "7")) * 0x9E3779B97F4A7C15ULL;
        rlm_real *logits = (rlm_real *)malloc(sizeof(rlm_real) * cfg->vocab); int *outids = (int *)malloc(sizeof(int) * (maxn + 1)); char *save = NULL; double t0 = now(); long ntot = 0;
        for (char *line = strtok_r(t, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            char *tab = strchr(line, '\t'); if (!tab) continue; *tab = 0; const char *text = tab + 1;
            Ids p; memset(&p, 0, sizeof p); p.v = v; ids_push(&p, 1); ids_text(&p, text);
            cnet_vsa_rlm_reset_state(m);
            if (prime) {   /* capsule-conditioned: the capsule's own corpus primes the recurrent state */
                char cp[1200]; snprintf(cp, sizeof cp, "%s/%s_corpus.txt", prime, line); char *ct = read_file(cp, NULL);
                if (ct) { Ids c; memset(&c, 0, sizeof c); c.v = v; ids_push(&c, 1); ids_text(&c, ct); ids_push(&c, 1); free(ct);
                    int n = (int)(c.n > prime_max ? prime_max : c.n); long off = c.n - n;   /* the tail of the corpus, ending at <eos> */
                    for (long pos = off; pos < c.n; pos += cfg->chunk) { int k = (int)((c.n - pos) < cfg->chunk ? (c.n - pos) : cfg->chunk); cnet_vsa_rlm_predict(m, c.ids + pos, k, logits); }
                    primed++; free(c.ids); }
            }
            if (gen) {
                cnet_vsa_rlm_predict(m, p.ids, (int)p.n, logits); int n = 0;
                for (; n < maxn; ++n) { int tk = sample(logits, cfg->vocab, temp, topk); if (tk == 1) break; outids[n] = tk; cnet_vsa_rlm_predict(m, &tk, 1, logits); }
                printf("%s\t", line); print_tokens(v, outids, n, stdout); printf("\n"); ntot += n;
            } else {
                ids_push(&p, 1); double total = 0; long n = 0;
                for (long pos = 0; pos + 1 < p.n; pos += cfg->chunk) { int k = (int)((p.n - 1 - pos) < cfg->chunk ? (p.n - 1 - pos) : cfg->chunk); total += cnet_vsa_rlm_chunk(m, p.ids + pos, k, 0) * k; n += k; }
                printf("%s\t%ld\t%.4f\t%.2f\t%ld\n", line, n, total / n, exp(total / n), p.unk); ntot += n;
            }
            free(p.ids);
        }
        fprintf(stderr, "%ld tokens in %.1f s (%.0f tok/s)%s\n", ntot, now() - t0, ntot / (now() - t0), primed ? " [primed]" : "");
        return 0;
    }
    fprintf(stderr, "unknown command %s\n", cmd); return 2;
}
