#include "cnet_vsa_delta.h"
#include "cnet_vsa.h"
#include "cnet_vsa_ngram.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static double now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3; }

int cnet_vsa_delta_init(CnetVsaDeltaMemory *m, int dim) {
    if (!m || dim <= 0 || dim > CNET_VSA_DEFAULT_DIM) return -1;
    memset(m, 0, sizeof(*m));
    m->S = (float *)calloc((size_t)dim * (size_t)dim, sizeof(float));
    if (!m->S) return -2;
    m->dim = dim;
    return 0;
}

void cnet_vsa_delta_free(CnetVsaDeltaMemory *m) {
    if (!m) return;
    free(m->S); m->S = NULL; m->dim = 0;
}

void cnet_vsa_delta_read(const CnetVsaDeltaMemory *m, const float *k, float *out_v) {
    const int d = m->dim;
    for (int i = 0; i < d; ++i) {
        const float *row = m->S + (size_t)i * d;
        float acc = 0.0f;
        for (int j = 0; j < d; ++j) acc += row[j] * k[j];
        out_v[i] = acc;
    }
}

void cnet_vsa_delta_write(CnetVsaDeltaMemory *m, const float *k, const float *v, float beta) {
    const int d = m->dim;
    float err[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_delta_read(m, k, err);                 /* S k */
    for (int i = 0; i < d; ++i) err[i] = beta * (v[i] - err[i]);
    for (int i = 0; i < d; ++i) {
        float *row = m->S + (size_t)i * d;
        const float e = err[i];
        if (e == 0.0f) continue;
        for (int j = 0; j < d; ++j) row[j] += e * k[j];
    }
}

/* tokens of one sentence as engine vocabulary ids; unknown words end the run
 * (the engine's ingest added every word, so on a capsule's own passages all
 * words are known unless the vocabulary cap was hit) */
static int sentence_ids(const CnetVsaNgramEngine *eng, const char *sentence, int *ids, int cap) {
    char copy[2048];
    snprintf(copy, sizeof(copy), "%s", sentence);
    int n = 0; char *save = NULL;
    for (char *tok = strtok_r(copy, " \t\r\n", &save); tok && n < cap; tok = strtok_r(NULL, " \t\r\n", &save)) {
        int id = cnet_vsa_ngram_lookup(eng, tok);
        if (id < 0) continue;     /* unknown word: skipped, as the engine skips empty tokens */
        ids[n++] = id;
    }
    return n;
}

int cnet_vsa_delta_build_from_capsule(CnetVsaDeltaMemory *m, const CnetVsaGenCapsule *cap, int passes, float beta) {
    if (!m || !cap || !cap->passages.present || cap->passages.count == 0 || cap->ngram.vocab_count == 0) return -1;
    if (passes < 1) passes = 1;
    if (!(beta > 0.0f) || beta > 1.0f) beta = 1.0f;
    if (m->S && m->dim != cap->ngram.dim) cnet_vsa_delta_free(m);
    if (!m->S && cnet_vsa_delta_init(m, cap->ngram.dim) != 0) return -2;
    memset(m->S, 0, sizeof(float) * (size_t)m->dim * m->dim);
    double t0 = now_us();
    const int d = m->dim;
    int ids[256]; float key[CNET_VSA_DEFAULT_DIM];
    uint32_t pairs = 0;
    for (int p = 0; p < passes; ++p) {
        for (uint32_t s = 0; s < cap->passages.count; ++s) {
            int n = sentence_ids(&cap->ngram, cap->passages.text + cap->passages.offset[s], ids, 256);
            for (int t = 1; t < n; ++t) {
                cnet_vsa_ngram_context_key(&cap->ngram, t >= 2 ? ids[t - 2] : -1, ids[t - 1], key);
                cnet_vsa_delta_write(m, key, cap->ngram.vocab[ids[t]].vector, beta);
                if (p == 0) pairs++;
            }
        }
    }
    m->pairs = pairs; m->passes = (uint32_t)passes; m->beta = beta;
    m->build_ms = (now_us() - t0) / 1e3;
    (void)d;
    return (int)pairs;
}

/* rank of the true word among the vocabulary under a score vector (0 = best) */
static int rank_of(const float *scores, size_t n, int truth) {
    int r = 0; float s = scores[truth];
    for (size_t v = 0; v < n; ++v) if (scores[v] > s) r++;
    return r;
}

int cnet_vsa_delta_recall(const CnetVsaDeltaMemory *m, const CnetVsaGenCapsule *cap,
                          const char *const *sentences, size_t n, CnetVsaDeltaRecall *out) {
    if (!m || !m->S || !cap || !sentences || !out) return -1;
    memset(out, 0, sizeof(*out));
    const CnetVsaNgramEngine *eng = &cap->ngram;
    const int d = eng->dim;
    int ids[256]; float key[CNET_VSA_DEFAULT_DIM], vhat[CNET_VSA_DEFAULT_DIM], unb[CNET_VSA_DEFAULT_DIM];
    float *sc = (float *)malloc(sizeof(float) * eng->vocab_count);
    if (!sc) return -2;
    double td = 0, tt = 0, tb = 0;
    for (size_t s = 0; s < n; ++s) {
        int cnt = sentence_ids(eng, sentences[s], ids, 256);
        for (int t = 1; t < cnt; ++t) {
            cnet_vsa_ngram_context_key(eng, t >= 2 ? ids[t - 2] : -1, ids[t - 1], key);
            int truth = ids[t];
            out->positions++;
            /* delta: v_hat = S k, score = cosine with each vocab vector */
            double a = now_us();
            cnet_vsa_delta_read(m, key, vhat);
            for (size_t v = 0; v < eng->vocab_count; ++v) sc[v] = cnet_vsa_similarity(vhat, eng->vocab[v].vector, d);
            td += now_us() - a;
            int r = rank_of(sc, eng->vocab_count, truth); out->delta_top1 += r == 0; out->delta_top5 += r < 5;
            /* table: the engine's scan */
            a = now_us();
            memset(sc, 0, sizeof(float) * eng->vocab_count);
            for (size_t tr = 0; tr < eng->transition_count; ++tr) {
                float match = cnet_vsa_similarity(key, eng->transitions[tr].context_key, d);
                if (match > 0.20f) sc[eng->transitions[tr].next_token_id] += match;
            }
            tt += now_us() - a;
            r = rank_of(sc, eng->vocab_count, truth); out->table_top1 += r == 0; out->table_top5 += r < 5;
            /* bundle: unbind the global binding */
            a = now_us();
            cnet_vsa_unbind(unb, eng->global_transition_matrix, key, d);
            for (size_t v = 0; v < eng->vocab_count; ++v) sc[v] = cnet_vsa_similarity(unb, eng->vocab[v].vector, d);
            tb += now_us() - a;
            r = rank_of(sc, eng->vocab_count, truth); out->bundle_top1 += r == 0; out->bundle_top5 += r < 5;
        }
    }
    free(sc);
    if (out->positions) { out->delta_read_us = td / out->positions; out->table_read_us = tt / out->positions; out->bundle_read_us = tb / out->positions; }
    return out->positions;
}
