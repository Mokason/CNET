#include "cnet_vsa_gen_capsule.h"
#include "cnet_vsa_lexicon.h"
#include "cnet_vsa_delta.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
#include <dirent.h>

#define FNV_OFFSET_BASIS 14695981039346656037ULL
#define FNV_PRIME        1099511628211ULL

static uint64_t fnv1a_update(uint64_t hash, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; ++i) {
        hash ^= (uint64_t)p[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

static uint64_t compute_capsule_digest(const CnetVsaGenCapsule *cap) {
    uint64_t h = FNV_OFFSET_BASIS;
    h = fnv1a_update(h, cap->name, sizeof(cap->name));
    h = fnv1a_update(h, cap->domain, sizeof(cap->domain));
    h = fnv1a_update(h, cap->centroid, sizeof(float) * cap->ngram.dim);
    h = fnv1a_update(h, &cap->safe_radius, sizeof(float));

    /* Cover vocabulary and vectors */
    for (size_t i = 0; i < cap->ngram.vocab_count; ++i) {
        h = fnv1a_update(h, cap->ngram.vocab[i].word, sizeof(cap->ngram.vocab[i].word));
        h = fnv1a_update(h, cap->ngram.vocab[i].vector, sizeof(float) * cap->ngram.dim);
    }

    /* Cover transitions and global matrix */
    for (size_t i = 0; i < cap->ngram.transition_count; ++i) {
        h = fnv1a_update(h, cap->ngram.transitions[i].context_key, sizeof(float) * cap->ngram.dim);
        h = fnv1a_update(h, &cap->ngram.transitions[i].next_token_id, sizeof(int));
    }
    h = fnv1a_update(h, cap->ngram.global_transition_matrix, sizeof(float) * cap->ngram.dim);

    /* Version 2 covers the calibration receipt so a forged radius provenance
     * is detected on load; version 3 also covers the wide topical block.
     * Version 1 files carry neither. */
    if (cap->version >= CNET_VSA_GENCAP_VERSION_V2) {
        h = fnv1a_update(h, &cap->version, sizeof(cap->version));
        h = fnv1a_update(h, &cap->calib, sizeof(cap->calib));
    }
    if (cap->version >= CNET_VSA_GENCAP_VERSION_V3) {
        h = fnv1a_update(h, &cap->topical, sizeof(cap->topical));
    }
    if (cap->version >= CNET_VSA_GENCAP_VERSION) {
        h = fnv1a_update(h, &cap->passages, sizeof(cap->passages));
    }
    return h;
}

int cnet_vsa_gencap_init(CnetVsaGenCapsule *cap, const char *name, const char *domain, int dim) {
    if (!cap || !name || !domain) return -1;
    memset(cap, 0, sizeof(*cap));

    cap->magic = CNET_VSA_GENCAP_MAGIC;
    cap->version = CNET_VSA_GENCAP_VERSION;
    snprintf(cap->name, sizeof(cap->name), "%s", name);
    snprintf(cap->domain, sizeof(cap->domain), "%s", domain);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    cap->created_tick = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    cap->safe_radius = 0.90f; /* legacy ceiling until cnet_vsa_gencap_calibrate replaces it */
    cap->certified = 0;
    cap->calib.reserved0 = CNET_VSA_ENCODER_DEFAULT; /* encoder id; cnet_vsa_gencap_set_encoder overrides */

    return cnet_vsa_ngram_init(&cap->ngram, dim, 1337);
}

int cnet_vsa_gencap_encode_intent(const char *text, float *out_vec, int dim) {
    return cnet_vsa_gencap_encode_intent_ex(text, out_vec, dim, CNET_VSA_ENCODER_DEFAULT);
}

int cnet_vsa_gencap_encode_intent_ex(const char *text, float *out_vec, int dim, uint32_t encoder_id) {
    if (!text || !out_vec || dim <= 0) return -1;
    CnetVsaTokenList tl;
    if (cnet_vsa_text_tokenize(text, &tl) <= 0) return -1;
    return cnet_vsa_text_encode_topical_ex(&tl, out_vec, dim, encoder_id);
}

uint32_t cnet_vsa_gencap_encoder_id(const CnetVsaGenCapsule *cap) {
    if (!cap) return CNET_VSA_ENCODER_BAG;
    return cap->calib.reserved0;
}

int cnet_vsa_gencap_set_encoder(CnetVsaGenCapsule *cap, uint32_t encoder_id) {
    if (!cap || cap->certified) return -1;
    if (cap->ngram.vocab_count > 0) return -1;
    if (!cnet_vsa_encoder_valid(encoder_id)) return -1;
    cap->calib.reserved0 = encoder_id;
    return 0;
}

static int encode_for_cap(const CnetVsaGenCapsule *cap, const char *text, float *v) {
    return cnet_vsa_gencap_encode_intent_ex(text, v, cap->ngram.dim, cnet_vsa_gencap_encoder_id(cap));
}

int cnet_vsa_gencap_encode_intent_q8(const char *text, int8_t *out_q8, uint32_t encoder_id) {
    if (!text || !out_q8) return -1;
    CnetVsaTokenList tl;
    if (cnet_vsa_text_tokenize(text, &tl) <= 0) return -1;
    return cnet_vsa_text_encode_topical_q8(&tl, out_q8, encoder_id);
}

/* wide float vector of a text under the capsule's encoder (heap scratch owned by caller) */
static int encode_wide_for_cap(const CnetVsaGenCapsule *cap, const char *text, float *wide) {
    CnetVsaTokenList tl;
    if (cnet_vsa_text_tokenize(text, &tl) <= 0) return -1;
    return cnet_vsa_text_encode_topical_wide(&tl, wide, cnet_vsa_gencap_encoder_id(cap));
}

int cnet_vsa_gencap_ingest(CnetVsaGenCapsule *cap, const char *text) {
    if (!cap || !text || !*text) return -1;

    if (cap->certified) return -1; /* sealed capsules are immutable */
    if (cap->calib.negative_count > 0 || cap->calib.calibrated) return -1; /* receipt would go stale */

    /* The topical centroid takes every sentence the encoder can represent,
     * independent of whether the n-gram engine still has vocabulary room:
     * calibrate_ex measures every ingested sentence leave-one-out, so the two
     * must agree on what was added. */
    int centroid_added = 0;
    float doc_vec[CNET_VSA_DEFAULT_DIM];
    if (encode_for_cap(cap, text, doc_vec) == 0) {
        for (int d = 0; d < cap->ngram.dim; ++d) {
            cap->centroid[d] += doc_vec[d];
        }
        centroid_added = 1;
        /* same sentence into the wide binary space (build-time accumulator) */
        float wide[CNET_VSA_TOPICAL_DIM];
        if (encode_wide_for_cap(cap, text, wide) == 0) {
            for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) cap->wide_sum[d] += wide[d];
            cap->wide_count++;
            cap->wide_hash += fnv1a_update(FNV_OFFSET_BASIS, text, strlen(text)); /* order-free multiset */
            /* and kept verbatim as a certified passage while the block has room */
            size_t len = strlen(text);
            cap->passages.ingested++;
            if (cap->passages.count < CNET_VSA_PASSAGE_MAX && cap->passages.bytes + len + 1 <= CNET_VSA_PASSAGE_BYTES) {
                cap->passages.offset[cap->passages.count] = cap->passages.bytes;
                memcpy(cap->passages.text + cap->passages.bytes, text, len + 1);
                cap->passages.bytes += (uint32_t)(len + 1);
                cap->passages.count++;
            }
        }
    }

    int toks = cnet_vsa_ngram_ingest_sentence(&cap->ngram, text);
    if (toks > 0) return toks;
    return centroid_added ? 1 : 0;
}

int cnet_vsa_gencap_add_frame(CnetVsaGenCapsule *cap, const char *frame_name,
                             const float *trigger_intent, int slot_count,
                             const char slot_names[][32]) {
    if (!cap || !frame_name || slot_count <= 0 || slot_count > CNET_VSA_GENCAP_SLOTS_MAX) return -1;
    if (cap->frame_count >= CNET_VSA_GENCAP_MAX_FRAMES) return -1;

    CnetVsaGrammarFrame *f = &cap->frames[cap->frame_count++];
    memset(f, 0, sizeof(*f));
    snprintf(f->frame_name, sizeof(f->frame_name), "%s", frame_name);

    if (trigger_intent) {
        memcpy(f->trigger_intent, trigger_intent, sizeof(float) * cap->ngram.dim);
    } else {
        encode_for_cap(cap, frame_name, f->trigger_intent);
    }

    f->slot_count = slot_count;
    for (int s = 0; s < slot_count; ++s) {
        snprintf(f->slots[s].name, sizeof(f->slots[s].name), "%s", slot_names[s]);
        /* Deterministic TPR role vector for this syntactic slot */
        cnet_vsa_text_token_vec(slot_names[s], f->slots[s].role_vector, cap->ngram.dim);
        cnet_vsa_normalize(f->slots[s].role_vector, cap->ngram.dim);
    }
    return 0;
}

/* ---- radius calibration ---------------------------------------------------- */

static int cmp_float_asc(const void *a, const void *b) {
    float fa = *(const float *)a, fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

static void dist_stats(const float *d, size_t n, float *mean, float *sd) {
    double m = 0.0, v = 0.0;
    for (size_t i = 0; i < n; ++i) m += d[i];
    m = n ? m / (double)n : 0.0;
    for (size_t i = 0; i < n; ++i) v += (d[i] - m) * (d[i] - m);
    v = n > 1 ? v / (double)(n - 1) : 0.0;
    *mean = (float)m;
    *sd = (float)sqrt(v);
}

/* distance of a normalized query to a normalized centroid */
static float centroid_dist(const float *q, const float *c, int dim) {
    return 1.0f - cnet_vsa_similarity(q, c, dim);
}

int cnet_vsa_gencap_calibrate(CnetVsaGenCapsule *cap,
                              const char *const *in_sentences, size_t in_count,
                              const char *const *probes, size_t probe_count,
                              const char *const *negatives, size_t neg_count,
                              float target_in_accept, float target_neg_reject) {
    return cnet_vsa_gencap_calibrate_ex(cap, in_sentences, in_count, probes, probe_count,
                                        negatives, neg_count, target_in_accept, target_neg_reject,
                                        NULL, 0, NULL, NULL, 0, NULL);
}

int cnet_vsa_gencap_calibrate_ex(CnetVsaGenCapsule *cap,
                                 const char *const *in_sentences, size_t in_count,
                                 const char *const *probes, size_t probe_count,
                                 const char *const *negatives, size_t neg_count,
                                 float target_in_accept, float target_neg_reject,
                                 float *out_d_in, size_t in_cap, size_t *out_in_n,
                                 float *out_d_neg, size_t neg_cap, size_t *out_neg_n) {
    CnetVsaCalibDistances fs;
    fs.d_in = out_d_in; fs.in_cap = out_d_in ? in_cap : 0; fs.in_n = 0;
    fs.d_neg = out_d_neg; fs.neg_cap = out_d_neg ? neg_cap : 0; fs.neg_n = 0;
    int rc = cnet_vsa_gencap_calibrate_dual(cap, in_sentences, in_count, probes, probe_count,
                                            negatives, neg_count, target_in_accept, target_neg_reject,
                                            &fs, NULL);
    if (out_in_n) *out_in_n = fs.in_n;
    if (out_neg_n) *out_neg_n = fs.neg_n;
    return rc;
}

/* The radius rule shared by both spaces. d_in / d_neg sorted ascending. */
typedef struct {
    float r_in, r_neg, r, in_rate, neg_rate, separation;
    float in_mean, in_sd, neg_mean, neg_sd;
    int separable;
} RadiusChoice;

static void choose_radius(const float *d_in, size_t n_in, const float *d_neg, size_t n_neg,
                          float target_in_accept, float target_neg_reject, RadiusChoice *out) {
    memset(out, 0, sizeof(*out));
    /* targets arrive as floats (0.80f = 0.800000012); snap them to 1e-4 in
     * double so 0.80 * 10 is 8, not 9, and the receipt means what it says */
    double t_in_d = floor((double)target_in_accept * 1e4 + 0.5) / 1e4;
    double t_neg_d = floor((double)target_neg_reject * 1e4 + 0.5) / 1e4;

    /* tightest radius admitting >= target_in of in-domain: the k-th smallest
     * in-domain distance where k = ceil(target * n) */
    size_t k_in = (size_t)ceil(t_in_d * (double)n_in - 1e-9);
    if (k_in == 0) k_in = 1;
    if (k_in > n_in) k_in = n_in;
    float r_in = d_in[k_in - 1];

    /* loosest radius rejecting >= target_neg of negatives: at most
     * floor((1 - target) * m) negatives may fall inside, so the radius must be
     * strictly below the (j+1)-th smallest negative distance */
    size_t j = (size_t)floor((1.0 - t_neg_d) * (double)n_neg + 1e-9);
    if (j >= n_neg) j = n_neg - 1;
    float r_neg = d_neg[j] - 1e-4f;

    float r = 0.5f * (r_in + r_neg);
    if (r < r_in) r = r_in;
    if (r > r_neg) r = r_neg;
    if (r <= 0.0f) r = r_in > 0.0f ? r_in : 1e-4f;

    size_t in_ok = 0, neg_ok = 0;
    for (size_t i = 0; i < n_in; ++i) if (d_in[i] <= r) in_ok++;
    for (size_t i = 0; i < n_neg; ++i) if (d_neg[i] > r) neg_ok++;

    out->r_in = r_in; out->r_neg = r_neg; out->r = r;
    out->separation = r_neg - r_in;
    out->in_rate = (float)in_ok / (float)n_in;
    out->neg_rate = (float)neg_ok / (float)n_neg;
    dist_stats(d_in, n_in, &out->in_mean, &out->in_sd);
    dist_stats(d_neg, n_neg, &out->neg_mean, &out->neg_sd);
    out->separable = (out->separation >= 0.0f &&
                      out->in_rate + 1e-6f >= target_in_accept &&
                      out->neg_rate + 1e-6f >= target_neg_reject) ? 1 : 0;
}

static void export_distances(CnetVsaCalibDistances *dst, const float *d_in, size_t n_in,
                             const float *d_neg, size_t n_neg) {
    if (!dst) return;
    dst->in_n = n_in;
    dst->neg_n = n_neg;
    if (dst->d_in && dst->in_cap) memcpy(dst->d_in, d_in, sizeof(float) * (n_in < dst->in_cap ? n_in : dst->in_cap));
    if (dst->d_neg && dst->neg_cap) memcpy(dst->d_neg, d_neg, sizeof(float) * (n_neg < dst->neg_cap ? n_neg : dst->neg_cap));
}

int cnet_vsa_gencap_calibrate_dual(CnetVsaGenCapsule *cap,
                                   const char *const *in_sentences, size_t in_count,
                                   const char *const *probes, size_t probe_count,
                                   const char *const *negatives, size_t neg_count,
                                   float target_in_accept, float target_neg_reject,
                                   CnetVsaCalibDistances *float_space,
                                   CnetVsaCalibDistances *binary_space) {
    if (float_space) { float_space->in_n = 0; float_space->neg_n = 0; }
    if (binary_space) { binary_space->in_n = 0; binary_space->neg_n = 0; }
    if (!cap || !negatives) return -1;
    if (in_count > 0 && !in_sentences) return -1;
    if (probe_count > 0 && !probes) return -1;
    if (target_in_accept <= 0.0f || target_in_accept > 1.0f) return -1;
    if (target_neg_reject <= 0.0f || target_neg_reject > 1.0f) return -1;
    if (cap->certified) return -1; /* sealed capsules are immutable; reload and reseal */

    const int dim = cap->ngram.dim;
    if (dim <= 0 || dim > CNET_VSA_DEFAULT_DIM) return -1;

    uint32_t encoder_id = cap->calib.reserved0;
    memset(&cap->calib, 0, sizeof(cap->calib));
    memset(&cap->topical, 0, sizeof(cap->topical));
    cap->calib.reserved0 = encoder_id;
    cap->calib.target_in_accept = target_in_accept;
    cap->calib.target_neg_reject = target_neg_reject;
    cap->topical.width = CNET_VSA_TOPICAL_DIM;
    cap->topical.kind = CNET_VSA_TOPICAL_KIND_Q8;

    const size_t in_total = in_count + probe_count;
    if (in_total < CNET_VSA_GENCAP_MIN_IN_DOMAIN || neg_count < CNET_VSA_GENCAP_MIN_NEGATIVES) {
        return CNET_VSA_GENCAP_INSUFFICIENT_EVIDENCE;
    }

    float *d_in = (float *)calloc(in_total, sizeof(float));
    float *d_neg = (float *)calloc(neg_count, sizeof(float));
    float *b_in = (float *)calloc(in_total, sizeof(float));
    float *b_neg = (float *)calloc(neg_count, sizeof(float));
    float *wide = (float *)malloc(sizeof(float) * CNET_VSA_TOPICAL_DIM);
    float *wide_loo = (float *)malloc(sizeof(float) * CNET_VSA_TOPICAL_DIM);
    if (!d_in || !d_neg || !b_in || !b_neg || !wide || !wide_loo) {
        free(d_in); free(d_neg); free(b_in); free(b_neg); free(wide); free(wide_loo);
        return -4;
    }

    /* cap->centroid and cap->wide_sum are raw sums here (seal normalizes /
     * thresholds them). Leave-one-out is exact on a plain sum. */
    float full[CNET_VSA_DEFAULT_DIM];
    memcpy(full, cap->centroid, sizeof(float) * (size_t)dim);
    float full_n[CNET_VSA_DEFAULT_DIM];
    memcpy(full_n, full, sizeof(float) * (size_t)dim);
    cnet_vsa_normalize(full_n, dim);
    int8_t full_q8[CNET_VSA_TOPICAL_DIM];
    cnet_vsa_text_wide_to_q8(cap->wide_sum, full_q8);
    float full_q8_norm = cnet_vsa_text_q8_norm(full_q8);

    size_t n_in = 0, n_in_b = 0, n_wide_seen = 0;
    uint64_t seen_hash = 0;
    for (size_t i = 0; i < in_count; ++i) {
        float v[CNET_VSA_DEFAULT_DIM];
        if (!in_sentences[i] || encode_for_cap(cap, in_sentences[i], v) != 0) continue;
        n_wide_seen++;
        seen_hash += fnv1a_update(FNV_OFFSET_BASIS, in_sentences[i], strlen(in_sentences[i]));
        float loo[CNET_VSA_DEFAULT_DIM];
        for (int d = 0; d < dim; ++d) loo[d] = full[d] - v[d];
        cnet_vsa_normalize(loo, dim);
        d_in[n_in++] = centroid_dist(v, loo, dim);

        /* wide space: the query is the runtime int8 count vector, the centroid
         * is the quantised leave-one-out sum, exactly as routing will see it */
        int8_t vq[CNET_VSA_TOPICAL_DIM], lq[CNET_VSA_TOPICAL_DIM];
        if (encode_wide_for_cap(cap, in_sentences[i], wide) == 0 &&
            cnet_vsa_gencap_encode_intent_q8(in_sentences[i], vq, encoder_id) == 0) {
            for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) wide_loo[d] = cap->wide_sum[d] - wide[d];
            cnet_vsa_text_wide_to_q8(wide_loo, lq);
            b_in[n_in_b++] = 1.0f - cnet_vsa_text_q8_similarity(vq, lq);
        }
    }
    for (size_t i = 0; i < probe_count; ++i) {
        float v[CNET_VSA_DEFAULT_DIM];
        if (!probes[i] || encode_for_cap(cap, probes[i], v) != 0) continue;
        d_in[n_in++] = centroid_dist(v, full_n, dim);
        int8_t vq[CNET_VSA_TOPICAL_DIM];
        if (cnet_vsa_gencap_encode_intent_q8(probes[i], vq, encoder_id) == 0) {
            b_in[n_in_b++] = 1.0f - cnet_vsa_text_q8_similarity_n(vq, cnet_vsa_text_q8_norm(vq), full_q8, full_q8_norm);
        }
    }
    size_t n_neg = 0, n_neg_b = 0;
    for (size_t i = 0; i < neg_count; ++i) {
        float v[CNET_VSA_DEFAULT_DIM];
        if (!negatives[i] || encode_for_cap(cap, negatives[i], v) != 0) continue;
        d_neg[n_neg++] = centroid_dist(v, full_n, dim);
        int8_t vq[CNET_VSA_TOPICAL_DIM];
        if (cnet_vsa_gencap_encode_intent_q8(negatives[i], vq, encoder_id) == 0) {
            b_neg[n_neg_b++] = 1.0f - cnet_vsa_text_q8_similarity_n(vq, cnet_vsa_text_q8_norm(vq), full_q8, full_q8_norm);
        }
    }
    free(wide); free(wide_loo);
    /* leave-one-out is only exact if the sentences measured are the sentences
     * that were accumulated (same multiset, not merely the same count) */
    if (in_count > 0 && (n_wide_seen != cap->wide_count || seen_hash != cap->wide_hash)) {
        free(d_in); free(d_neg); free(b_in); free(b_neg);
        return -1;
    }
    if (n_in < CNET_VSA_GENCAP_MIN_IN_DOMAIN || n_neg < CNET_VSA_GENCAP_MIN_NEGATIVES ||
        n_in_b < CNET_VSA_GENCAP_MIN_IN_DOMAIN || n_neg_b < CNET_VSA_GENCAP_MIN_NEGATIVES) {
        free(d_in); free(d_neg); free(b_in); free(b_neg);
        return CNET_VSA_GENCAP_INSUFFICIENT_EVIDENCE;
    }

    qsort(d_in, n_in, sizeof(float), cmp_float_asc);
    qsort(d_neg, n_neg, sizeof(float), cmp_float_asc);
    qsort(b_in, n_in_b, sizeof(float), cmp_float_asc);
    qsort(b_neg, n_neg_b, sizeof(float), cmp_float_asc);
    export_distances(float_space, d_in, n_in, d_neg, n_neg);
    export_distances(binary_space, b_in, n_in_b, b_neg, n_neg_b);

    RadiusChoice f, b;
    choose_radius(d_in, n_in, d_neg, n_neg, target_in_accept, target_neg_reject, &f);
    choose_radius(b_in, n_in_b, b_neg, n_neg_b, target_in_accept, target_neg_reject, &b);
    free(d_in); free(d_neg); free(b_in); free(b_neg);

    /* float-512 receipt: recorded honestly; when not separable the radius sits
     * on the fail-closed side (meets the reject target, misses accept) */
    cap->calib.in_domain_count = (uint32_t)n_in;
    cap->calib.negative_count = (uint32_t)n_neg;
    cap->calib.radius_in = f.r_in;
    cap->calib.radius_neg = f.r_neg;
    cap->calib.separation = f.separation;
    cap->calib.in_dist_mean = f.in_mean;  cap->calib.in_dist_std = f.in_sd;
    cap->calib.neg_dist_mean = f.neg_mean; cap->calib.neg_dist_std = f.neg_sd;
    cap->calib.in_accept_rate = f.in_rate;
    cap->calib.neg_reject_rate = f.neg_rate;
    cap->calib.reserved1 = f.separable ? 1.0f : 0.0f;  /* float space separable? */
    if (f.separable) {
        cap->safe_radius = f.r;
    } else {
        float r_closed = f.r_neg > 1e-4f ? f.r_neg : 1e-4f;
        cap->safe_radius = r_closed;
    }

    /* wide int8 receipt: the routing space of a v3 registry, so it decides */
    cap->topical.radius_in = b.r_in;
    cap->topical.radius_neg = b.r_neg;
    cap->topical.separation = b.separation;
    cap->topical.in_dist_mean = b.in_mean;  cap->topical.in_dist_std = b.in_sd;
    cap->topical.neg_dist_mean = b.neg_mean; cap->topical.neg_dist_std = b.neg_sd;
    cap->topical.in_accept_rate = b.in_rate;
    cap->topical.neg_reject_rate = b.neg_rate;
    cap->topical.calibrated = b.separable ? 1u : 0u;
    cap->topical.safe_radius = b.separable ? b.r : (b.r_neg > 1e-4f ? b.r_neg : 1e-4f);

    cap->calib.calibrated = b.separable ? 1u : 0u;
    if (!b.separable) {
        /* leave both receipts as evidence; seal will refuse */
        return CNET_VSA_GENCAP_NOT_SEPARABLE;
    }
    return 0;
}

static int seal_common(CnetVsaGenCapsule *cap, uint32_t version) {
    if (!cap) return -1;
    if (cap->certified) return -1;  /* sealed (or loaded) capsules are immutable: rebuild from corpus */
    if (cap->ngram.vocab_count == 0) return -2;
    if (version >= CNET_VSA_GENCAP_VERSION_V3 && cap->wide_count == 0) return -7; /* no wide evidence */

    /* A calibration that was attempted and failed is a refusal, not a fallback */
    if (!cap->calib.calibrated && cap->calib.negative_count > 0) {
        cap->certified = 0;
        return CNET_VSA_GENCAP_NOT_SEPARABLE;
    }

    /* Normalize centroid (idempotent for an already-normalized centroid) */
    cnet_vsa_normalize(cap->centroid, cap->ngram.dim);

    if (!cap->calib.calibrated) {
        /* Legacy: uncalibrated capsules keep the historical fixed ceiling */
        if (cap->safe_radius <= 0.0f) cap->safe_radius = 0.90f;
    }

    cap->version = version;
    uint32_t encoder_id = cap->calib.reserved0;
    if (version == CNET_VSA_GENCAP_VERSION_V1) memset(&cap->calib, 0, sizeof(cap->calib));

    if (version >= CNET_VSA_GENCAP_VERSION) {
        cap->passages.present = cap->passages.count > 0 ? 1u : 0u;
        if (cap->passages.z_min <= 0.0f) cap->passages.z_min = 1.0f;
    } else {
        memset(&cap->passages, 0, sizeof(cap->passages));
    }
    if (version >= CNET_VSA_GENCAP_VERSION_V3) {
        /* wide int8 topical block from the build-time accumulator */
        cap->topical.present = 1;
        cap->topical.width = CNET_VSA_TOPICAL_DIM;
        cap->topical.kind = CNET_VSA_TOPICAL_KIND_Q8;
        cnet_vsa_text_wide_to_q8(cap->wide_sum, cap->topical.q8);
        cap->topical.q8_norm = cnet_vsa_text_q8_norm(cap->topical.q8);
        /* a lexicon-encoded capsule records which lexicon its wide space is in
         * (low 32 bits of the lexicon digest, bit-cast into reserved1) */
        if (encoder_id == CNET_VSA_ENCODER_LEX) {
            uint32_t tag = cnet_vsa_lexicon_active_tag();
            if (tag == 0) { cap->certified = 0; return -9; } /* no active lexicon */
            memcpy(&cap->topical.reserved1, &tag, sizeof(tag));
        }
        if (!cap->topical.calibrated && cap->topical.safe_radius <= 0.0f) {
            cap->topical.safe_radius = 0.90f; /* uncalibrated ceiling, mirrors the float default */
        }
    } else {
        memset(&cap->topical, 0, sizeof(cap->topical));
    }

    cap->digest = compute_capsule_digest(cap);
    cap->certified = 1;
    return 0;
}

int cnet_vsa_gencap_seal(CnetVsaGenCapsule *cap) {
    return seal_common(cap, CNET_VSA_GENCAP_VERSION);
}

int cnet_vsa_gencap_seal_legacy_v1(CnetVsaGenCapsule *cap) {
    return seal_common(cap, CNET_VSA_GENCAP_VERSION_V1);
}

int cnet_vsa_gencap_seal_legacy_v2(CnetVsaGenCapsule *cap) {
    return seal_common(cap, CNET_VSA_GENCAP_VERSION_V2);
}

int cnet_vsa_gencap_seal_legacy_v3(CnetVsaGenCapsule *cap) {
    return seal_common(cap, CNET_VSA_GENCAP_VERSION_V3);
}

/* build side: encode the passages once, then score queries against them */
static int8_t *passages_q8(const CnetVsaGenCapsule *cap, float **out_norm) {
    uint32_t n = cap->passages.count;
    int8_t *pq = (int8_t *)malloc((size_t)n * CNET_VSA_TOPICAL_DIM);
    float *pn = (float *)malloc(sizeof(float) * n);
    if (!pq || !pn) { free(pq); free(pn); return NULL; }
    for (uint32_t i = 0; i < n; ++i) {
        int8_t *v = pq + (size_t)i * CNET_VSA_TOPICAL_DIM;
        if (cnet_vsa_gencap_encode_intent_q8(cap->passages.text + cap->passages.offset[i], v, cap->calib.reserved0) != 0) memset(v, 0, CNET_VSA_TOPICAL_DIM);
        pn[i] = cnet_vsa_text_q8_norm(v);
    }
    *out_norm = pn;
    return pq;
}

/* z of the best passage for one query: best vs mean/sd of the other passages. 0 when not scorable. */
static int passage_best_z(const CnetVsaGenCapsule *cap, const int8_t *pq, const float *pn, const char *query, float *out_z) {
    uint32_t n = cap->passages.count;
    if (n < 3) return 0;
    int8_t qv[CNET_VSA_TOPICAL_DIM];
    if (cnet_vsa_gencap_encode_intent_q8(query, qv, cap->calib.reserved0) != 0) return 0;
    float qn = cnet_vsa_text_q8_norm(qv);
    double sum = 0.0, sq = 0.0; float best = -2.0f;
    for (uint32_t i = 0; i < n; ++i) {
        float sim = cnet_vsa_text_q8_similarity_n(qv, qn, pq + (size_t)i * CNET_VSA_TOPICAL_DIM, pn[i]);
        sum += sim; sq += (double)sim * sim;
        if (sim > best) best = sim;
    }
    double mean = (sum - best) / (n - 1);
    double var = ((sq - (double)best * best) - (n - 1) * mean * mean) / (n - 2);
    if (var < 1e-12) var = 1e-12;
    if (out_z) *out_z = (float)((best - mean) / sqrt(var));
    return (int)n;
}

int cnet_vsa_gencap_calibrate_passages(CnetVsaGenCapsule *cap,
                                       const char *const *probes, size_t probe_count,
                                       const char *const *negatives, size_t neg_count,
                                       float target_reject) {
    if (!cap) return -1;
    if (cap->certified) return -1;
    cap->passages.calibrated = 0;
    cap->passages.probe_count = 0;
    cap->passages.neg_count = 0;
    cap->passages.probe_z_median = 0.0f;
    cap->passages.probe_accept = 0.0f;
    cap->passages.z_min = 1.0f;
    if (target_reject <= 0.0f || target_reject > 1.0f) return -1;
    if (!negatives || neg_count < 8 || cap->passages.count < 3) return 0;
    float *pn = NULL; int8_t *pq = passages_q8(cap, &pn);
    if (!pq) return -4;
    float *zs = (float *)malloc(sizeof(float) * (neg_count + (probes ? probe_count : 0) + 1));
    if (!zs) { free(pq); free(pn); return -4; }
    size_t n = 0;
    for (size_t i = 0; i < neg_count; ++i) { float z; if (negatives[i] && passage_best_z(cap, pq, pn, negatives[i], &z) > 0) zs[n++] = z; }
    if (n >= 8) {
        qsort(zs, n, sizeof(float), cmp_float_asc);
        size_t k = (size_t)((double)n * target_reject); if (k >= n) k = n - 1;
        float q = zs[k];
        cap->passages.z_min = q > 1.0f ? q : 1.0f;
        cap->passages.neg_count = (uint32_t)n;
        cap->passages.calibrated = 1;
        /* probes against the floor (recorded, not enforced) */
        size_t m = 0, acc = 0;
        for (size_t i = 0; probes && i < probe_count; ++i) {
            float z; if (probes[i] && passage_best_z(cap, pq, pn, probes[i], &z) > 0) { zs[m++] = z; acc += z >= cap->passages.z_min; }
        }
        if (m) { qsort(zs, m, sizeof(float), cmp_float_asc); cap->passages.probe_z_median = zs[m / 2]; cap->passages.probe_count = (uint32_t)m; cap->passages.probe_accept = (float)acc / (float)m; }
    }
    free(zs); free(pq); free(pn);
    return 0;
}

int cnet_vsa_gencap_save(const CnetVsaGenCapsule *cap, const char *filepath) {
    if (!cap || !filepath || !*filepath) return -1;
    if (!cap->certified) return -2; /* Refuse to save uncertified capsule */
    if (cap->version != CNET_VSA_GENCAP_VERSION) return -6; /* reseal before saving a legacy load */

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp-%ld", filepath, (long)time(NULL));

    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) return -3;

    /* only the persisted prefix: the wide accumulator after the topical block
     * is build-time state and never written */
    size_t written = fwrite(cap, CNET_VSA_GENCAP_FILE_SIZE, 1, fp);
    fclose(fp);

    if (written != 1) {
        remove(tmp_path);
        return -4;
    }

    if (rename(tmp_path, filepath) != 0) {
        remove(tmp_path);
        return -5;
    }
    return 0;
}

int cnet_vsa_gencap_load(CnetVsaGenCapsule *cap, const char *filepath) {
    if (!cap || !filepath || !*filepath) return -1;

    FILE *fp = fopen(filepath, "rb");
    if (!fp) return -2;

    memset(cap, 0, sizeof(*cap));
    size_t got = fread(cap, 1, CNET_VSA_GENCAP_FILE_SIZE, fp);
    int extra = fgetc(fp);
    fclose(fp);

    if (cap->magic != CNET_VSA_GENCAP_MAGIC) return -4;

    /* Each older version is an exact prefix of the next: v1 has no receipt,
     * v2 no topical block. Missing tails stay zeroed. Any other length is corrupt. */
    if (cap->version == CNET_VSA_GENCAP_VERSION) {
        if (got != CNET_VSA_GENCAP_V4_SIZE || extra != EOF) return -3;
    } else if (cap->version == CNET_VSA_GENCAP_VERSION_V3) {
        if (got != CNET_VSA_GENCAP_V3_SIZE) return -3;
        memset(&cap->passages, 0, sizeof(cap->passages));
    } else if (cap->version == CNET_VSA_GENCAP_VERSION_V2) {
        if (got != CNET_VSA_GENCAP_V2_SIZE) return -3;
        memset(&cap->topical, 0, sizeof(cap->topical));
        memset(&cap->passages, 0, sizeof(cap->passages));
    } else if (cap->version == CNET_VSA_GENCAP_VERSION_V1) {
        if (got != CNET_VSA_GENCAP_V1_SIZE) return -3;
        memset(&cap->calib, 0, sizeof(cap->calib));
        memset(&cap->topical, 0, sizeof(cap->topical));
        memset(&cap->passages, 0, sizeof(cap->passages));
    } else {
        return -4; /* Incompatible capsule version */
    }
    /* passage block bounds: counts, byte span, and every passage NUL-terminated inside it */
    if (cap->passages.present) {
        if (cap->passages.count == 0 || cap->passages.count > CNET_VSA_PASSAGE_MAX ||
            cap->passages.bytes == 0 || cap->passages.bytes > CNET_VSA_PASSAGE_BYTES) { cap->certified = 0; return -4; }
        for (uint32_t i = 0; i < cap->passages.count; ++i) {
            uint32_t off = cap->passages.offset[i];
            if (off >= cap->passages.bytes) { cap->certified = 0; return -4; }
            if (!memchr(cap->passages.text + off, 0, cap->passages.bytes - off)) { cap->certified = 0; return -4; }
        }
    }

    /* Structural bounds come from the file and size every later loop and
     * stack buffer (digest walk, encode_for_cap, generation). Refuse before
     * touching them: a corrupt or crafted count must fail closed, not crash. */
    if (cap->ngram.dim <= 0 || cap->ngram.dim > CNET_VSA_DEFAULT_DIM ||
        cap->ngram.vocab_count > CNET_VSA_NGRAM_MAX_VOCAB ||
        cap->ngram.transition_count > CNET_VSA_NGRAM_MAX_TRANS ||
        cap->frame_count > CNET_VSA_GENCAP_MAX_FRAMES ||
        !cnet_vsa_encoder_valid(cap->calib.reserved0) ||
        (cap->topical.present && (cap->topical.width != CNET_VSA_TOPICAL_DIM ||
                                  cap->topical.kind != CNET_VSA_TOPICAL_KIND_Q8))) {
        cap->certified = 0;
        return -4; /* structurally invalid: out-of-range dimension, counts, encoder or width */
    }

    /* Verify Cryptographic Integrity Digest */
    uint64_t expected_digest = compute_capsule_digest(cap);
    if (expected_digest != cap->digest) {
        cap->certified = 0;
        return -5; /* Tampered capsule detected: fail closed */
    }

    cap->certified = 1;
    return 0;
}

int cnet_vsa_gencap_verify_scope(const CnetVsaGenCapsule *cap, const float *query_vec, float *out_dist) {
    if (!cap || !cap->certified || !query_vec) return 0;

    float sim = cnet_vsa_similarity(query_vec, cap->centroid, cap->ngram.dim);
    float dist = 1.0f - sim;
    if (out_dist) *out_dist = dist;

    /* Within certified radius */
    return (dist <= cap->safe_radius) ? 1 : 0;
}

int cnet_vsa_gencap_verify_scope_q8(const CnetVsaGenCapsule *cap, const int8_t *query_q8,
                                    float *out_dist) {
    if (!cap || !cap->certified || !query_q8 || !cap->topical.present) return 0;
    float dist = 1.0f - cnet_vsa_text_q8_similarity_n(query_q8, cnet_vsa_text_q8_norm(query_q8),
                                                       cap->topical.q8, cap->topical.q8_norm);
    if (out_dist) *out_dist = dist;
    return (dist <= cap->topical.safe_radius) ? 1 : 0;
}

int cnet_vsa_gencap_generate(const CnetVsaGenCapsule *cap,
                             const char *seed_word,
                             const float *intent_vec,
                             float steer_weight,
                             int max_tokens,
                             char *out_text,
                             size_t out_text_size,
                             int *out_tokens) {
    return cnet_vsa_gencap_generate_ex(cap, seed_word, intent_vec, NULL, steer_weight,
                                       max_tokens, out_text, out_text_size, out_tokens);
}

int cnet_vsa_gencap_generate_ex(const CnetVsaGenCapsule *cap,
                                const char *seed_word,
                                const float *intent_vec,
                                const int8_t *intent_q8,
                                float steer_weight,
                                int max_tokens,
                                char *out_text,
                                size_t out_text_size,
                                int *out_tokens) {
    return cnet_vsa_gencap_generate_mem(cap, seed_word, intent_vec, intent_q8, steer_weight, max_tokens,
                                        CNET_VSA_GEN_MEM_DEFAULT, out_text, out_text_size, out_tokens);
}

static void delta_read_cb(const void *ctx, const float *key, float *out_v) {
    cnet_vsa_delta_read((const CnetVsaDeltaMemory *)ctx, key, out_v);
}

int cnet_vsa_gencap_generate_mem(const CnetVsaGenCapsule *cap,
                                 const char *seed_word,
                                 const float *intent_vec,
                                 const int8_t *intent_q8,
                                 float steer_weight,
                                 int max_tokens,
                                 unsigned mem,
                                 char *out_text,
                                 size_t out_text_size,
                                 int *out_tokens) {
    if (!cap || !cap->certified || !out_text || out_text_size < 32) return -1;

    /* 1. Gate: wide int8 space when available and given, else the float space */
    if (intent_q8 && cap->topical.present) {
        float dist = 0.0f;
        if (!cnet_vsa_gencap_verify_scope_q8(cap, intent_q8, &dist)) {
            snprintf(out_text, out_text_size, "ABSTAIN: out_of_domain (space=wide, dist=%.3f, limit=%.3f)",
                     dist, cap->topical.safe_radius);
            if (out_tokens) *out_tokens = 0;
            return -2; /* Refused out of domain */
        }
    } else if (intent_vec) {
        float dist = 0.0f;
        if (!cnet_vsa_gencap_verify_scope(cap, intent_vec, &dist)) {
            snprintf(out_text, out_text_size, "ABSTAIN: out_of_domain (dist=%.3f, limit=%.3f)",
                     dist, cap->safe_radius);
            if (out_tokens) *out_tokens = 0;
            return -2; /* Refused out of domain */
        }
    }

    /* 2. Choose seed word based on seed_word, frames, or default */
    const char *actual_seed = seed_word;
    if (!actual_seed || !*actual_seed) {
        if (cap->frame_count > 0 && cap->frames[0].slot_count > 0 &&
            cap->frames[0].slots[0].exemplar_count > 0) {
            actual_seed = cap->frames[0].slots[0].exemplar_words[0];
        } else {
            actual_seed = cap->ngram.vocab_count > 0 ? cap->ngram.vocab[0].word : "the";
        }
    }

    /* 3. Pure Algebraic Unbinding Generation (table, bundle and/or a
     *    delta-rule memory derived from the certified passages) */
    CnetVsaNgramEngine *non_const_ngram = (CnetVsaNgramEngine *)&cap->ngram;
    CnetVsaDeltaMemory dm; memset(&dm, 0, sizeof(dm));
    int have_delta = 0;
    if (mem & CNET_VSA_GEN_MEM_DELTA) {
        if (cnet_vsa_delta_build_from_capsule(&dm, cap, 4, 0.5f) > 0) have_delta = 1;
        else mem &= ~(unsigned)CNET_VSA_GEN_MEM_DELTA;   /* pre-v4 capsule: no passages to derive from */
    }
    int rc = cnet_vsa_ngram_generate_ex(non_const_ngram,
                                        actual_seed,
                                        intent_vec,
                                        steer_weight,
                                        0.85f,
                                        max_tokens,
                                        mem,
                                        have_delta ? delta_read_cb : NULL,
                                        have_delta ? (const void *)&dm : NULL,
                                        out_text,
                                        out_text_size,
                                        out_tokens);
    if (have_delta) cnet_vsa_delta_free(&dm);

    /* 4. Sentence Boundary Cleanup */
    size_t len = strlen(out_text);
    if (len > 0) {
        /* Capitalize first character */
        out_text[0] = (char)toupper((unsigned char)out_text[0]);

        /* Ensure clean terminal punctuation */
        char last = out_text[len - 1];
        if (last != '.' && last != '!' && last != '?') {
            if (len + 1 < out_text_size) {
                out_text[len] = '.';
                out_text[len + 1] = '\0';
            }
        }
    }

    return rc;
}

int cnet_vsa_registry_init(CnetVsaGenRegistry *reg, int dim) {
    if (!reg || dim <= 0) return -1;
    memset(reg, 0, sizeof(*reg));
    reg->dim = dim;
    reg->z_margin = CNET_VSA_ROUTE_Z_MARGIN_DEFAULT;
    reg->min_null_count = CNET_VSA_ROUTE_MIN_NULL_DEFAULT;
    reg->ambiguity_k_wide = CNET_VSA_ROUTE_AMBIGUITY_K_WIDE_DEFAULT;
    reg->term_gate = 1;
    reg->answer_siblings = 1;      /* measured 2026-09-12: +5 points of correct answers at equal precision (76%) */
    reg->sibling_zgap = 2.0f;
    reg->ambiguity_k_float = CNET_VSA_ROUTE_AMBIGUITY_K_FLOAT_DEFAULT;
    reg->force_float = 0;
    return 0;
}

int cnet_vsa_registry_add_header(CnetVsaGenRegistry *reg, const CnetVsaCapsuleHeader *header,
                                 const char *filepath) {
    if (!reg || !header) return -1;
    if (reg->count >= CNET_VSA_REGISTRY_MAX_CAPSULES) return -2;
    if (header->magic != CNET_VSA_GENCAP_MAGIC ||
        (header->version != CNET_VSA_GENCAP_VERSION &&
         header->version != CNET_VSA_GENCAP_VERSION_V3 &&
         header->version != CNET_VSA_GENCAP_VERSION_V2 &&
         header->version != CNET_VSA_GENCAP_VERSION_V1)) {
        return -5;
    }
    if (!header->certified) return -6;

    CnetVsaRegisteredCap *entry = &reg->capsules[reg->count];
    memset(entry, 0, sizeof(*entry));
    entry->header = *header;
    if (filepath) snprintf(entry->filepath, sizeof(entry->filepath), "%s", filepath);
    reg->count++;
    return 0;
}

int cnet_vsa_registry_add_capsule_mem(CnetVsaGenRegistry *reg, const CnetVsaGenCapsule *cap,
                                      const char *filepath) {
    if (!reg || !cap) return -1;
    if (reg->count >= CNET_VSA_REGISTRY_MAX_CAPSULES) return -2;
    if (cap->magic != CNET_VSA_GENCAP_MAGIC || !cap->certified) return -6;

    if (cap->calib.reserved0 == CNET_VSA_ENCODER_LEX) {
        /* the wide space of this capsule exists only under its lexicon */
        uint32_t tag = 0;
        memcpy(&tag, &cap->topical.reserved1, sizeof(tag));
        if (!cap->topical.present || tag == 0 || tag != cnet_vsa_lexicon_active_tag()) return -8;
    }

    CnetVsaRegisteredCap *entry = &reg->capsules[reg->count];
    memset(entry, 0, sizeof(*entry));
    memcpy(&entry->header, cap, sizeof(CnetVsaCapsuleHeader)); /* header is the capsule prefix */
    if (filepath) snprintf(entry->filepath, sizeof(entry->filepath), "%s", filepath);
    entry->encoder_id = cnet_vsa_encoder_valid(cap->calib.reserved0) ? cap->calib.reserved0 : CNET_VSA_ENCODER_BAG;
    if (cap->version >= CNET_VSA_GENCAP_VERSION_V3 && cap->topical.present &&
        cap->topical.width == CNET_VSA_TOPICAL_DIM && cap->topical.kind == CNET_VSA_TOPICAL_KIND_Q8) {
        entry->has_topical = 1;
        entry->topical_radius = cap->topical.safe_radius;
        entry->topical_norm = cap->topical.q8_norm;
        memcpy(entry->topical, cap->topical.q8, sizeof(entry->topical));
    }
    if (cap->version >= CNET_VSA_GENCAP_VERSION && cap->passages.present && cap->passages.count > 0) {
        entry->passage_text = (char *)malloc(cap->passages.bytes);
        if (!entry->passage_text) return -7;
        memcpy(entry->passage_text, cap->passages.text, cap->passages.bytes);
        memcpy(entry->passage_off, cap->passages.offset, sizeof(entry->passage_off));
        entry->passage_count = cap->passages.count;
        entry->passage_zmin = cap->passages.z_min > 0.0f ? cap->passages.z_min : 1.0f;
    }
    reg->count++;
    return 0;
}

void cnet_vsa_registry_release(CnetVsaGenRegistry *reg) {
    if (!reg) return;
    for (size_t i = 0; i < reg->count; ++i) {
        CnetVsaRegisteredCap *e = &reg->capsules[i];
        free(e->passage_text); free(e->passage_q8); free(e->passage_norm);
        e->passage_text = NULL; e->passage_q8 = NULL; e->passage_norm = NULL; e->passage_count = 0;
    }
}

const char *cnet_vsa_registry_passage(const CnetVsaGenRegistry *reg, int cap_idx, int passage_idx) {
    if (!reg || cap_idx < 0 || (size_t)cap_idx >= reg->count) return NULL;
    const CnetVsaRegisteredCap *e = &reg->capsules[cap_idx];
    if (!e->passage_text || passage_idx < 0 || (uint32_t)passage_idx >= e->passage_count) return NULL;
    return e->passage_text + e->passage_off[passage_idx];
}

static double answer_now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3; }

static int term_gate_check(const CnetVsaGenRegistry *reg, const char *prompt, CnetVsaRouteResult *r);

/* rank one capsule's passages against a q8 query; returns best passage index and fills sims/z; -1 if none */
static int rank_passages(CnetVsaRegisteredCap *e, const int8_t *qv, float qn, float *sims, float *out_z, int *out_cold) {
    if (!e->passage_text || e->passage_count == 0) return -1;
    if (!e->passage_q8) {
        int8_t *pq = (int8_t *)malloc((size_t)e->passage_count * CNET_VSA_TOPICAL_DIM);
        float *pn = (float *)malloc(sizeof(float) * e->passage_count);
        if (!pq || !pn) { free(pq); free(pn); return -1; }
        for (uint32_t i = 0; i < e->passage_count; ++i) {
            int8_t *v = pq + (size_t)i * CNET_VSA_TOPICAL_DIM;
            if (cnet_vsa_gencap_encode_intent_q8(e->passage_text + e->passage_off[i], v, e->encoder_id) != 0) memset(v, 0, CNET_VSA_TOPICAL_DIM);
            pn[i] = cnet_vsa_text_q8_norm(v);
        }
        e->passage_q8 = pq; e->passage_norm = pn;
        if (out_cold) *out_cold = 1;
    }
    int n = (int)e->passage_count, bi = -1; float best = -2.0f; double sum = 0.0, sq = 0.0;
    for (int i = 0; i < n; ++i) {
        sims[i] = cnet_vsa_text_q8_similarity_n(qv, qn, e->passage_q8 + (size_t)i * CNET_VSA_TOPICAL_DIM, e->passage_norm[i]);
        sum += sims[i]; sq += (double)sims[i] * sims[i];
        if (sims[i] > best) { best = sims[i]; bi = i; }
    }
    if (n >= 3) {
        double mean = (sum - best) / (n - 1);
        double var = ((sq - (double)best * best) - (n - 1) * mean * mean) / (n - 2);
        if (var < 1e-12) var = 1e-12;
        *out_z = (float)((best - mean) / sqrt(var));
    } else *out_z = e->passage_zmin;
    return bi;
}

int cnet_vsa_registry_answer(CnetVsaGenRegistry *reg, const char *prompt, int k, CnetVsaAnswer *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->capsule_idx = -1;
    if (!reg || !prompt || !*prompt) { out->status = CNET_VSA_ANSWER_ROUTE_REFUSED; return -1; }
    if (k < 1) k = 1;
    if (k > CNET_VSA_ANSWER_MAX) k = CNET_VSA_ANSWER_MAX;
    double t0 = answer_now_us();
    int winner = cnet_vsa_registry_route_query(reg, prompt, &out->route);
    out->route_us = answer_now_us() - t0;
    if (winner < 0 && reg->answer_siblings && out->route.status == CNET_VSA_ROUTE_REFUSE_AMBIGUOUS &&
        out->route.radius_ok && out->route.margin_ok && out->route.best_idx >= 0 && out->route.second_idx >= 0) {
        /* mixture-of-memories path: the two tied capsules both hold the query inside the radius; take the
         * better passage of the two, each judged against its own floor, and let the term gate check the pick */
        CnetVsaRegisteredCap *a = &reg->capsules[out->route.best_idx], *b = &reg->capsules[out->route.second_idx];
        int8_t qv[CNET_VSA_TOPICAL_DIM];
        if (a->passage_text && b->passage_text && cnet_vsa_gencap_encode_intent_q8(prompt, qv, a->encoder_id) == 0) {
            float qn = cnet_vsa_text_q8_norm(qv), sa[CNET_VSA_PASSAGE_MAX], sb[CNET_VSA_PASSAGE_MAX], za = 0, zb = 0; int cold = 0;
            t0 = answer_now_us();
            int ia = rank_passages(a, qv, qn, sa, &za, &cold), ib = rank_passages(b, qv, qn, sb, &zb, &cold);
            int pick_a = ia >= 0 && (ib < 0 || sa[ia] >= sb[ib]);
            CnetVsaRegisteredCap *e = pick_a ? a : b; int pi = pick_a ? ia : ib; float z = pick_a ? za : zb; float *sims = pick_a ? sa : sb;
            int cidx = pick_a ? out->route.best_idx : out->route.second_idx;
            out->rank_us = answer_now_us() - t0; out->cold = cold; out->passages = (int)e->passage_count; out->z = z; out->z_min = e->passage_zmin;
            float zgap = pick_a ? za - zb : zb - za;
            if (pi >= 0 && z >= e->passage_zmin && zgap >= reg->sibling_zgap) {
                /* term gate on the picked capsule: the accept must not hinge on one word */
                CnetVsaRouteResult tr = out->route; tr.best_idx = cidx; tr.radius = e->topical_radius;
                if (!reg->term_gate || term_gate_check(reg, prompt, &tr) >= 0) {
                    out->capsule_idx = cidx; out->sibling_pick = 1;
                    int n = (int)e->passage_count, used[CNET_VSA_PASSAGE_MAX]; memset(used, 0, sizeof(int) * (size_t)n);
                    for (int j = 0; j < k && j < n; ++j) { int bi = -1; float bs = -2.0f; for (int i = 0; i < n; ++i) if (!used[i] && sims[i] > bs) { bs = sims[i]; bi = i; } used[bi] = 1; out->passage_idx[j] = bi; out->sim[j] = bs; out->n = j + 1; }
                    out->status = CNET_VSA_ANSWER_OK;
                    return cidx;
                }
                out->route.term_checked = tr.term_checked; out->route.term_ok = tr.term_ok;
            }
            out->capsule_idx = cidx; out->status = CNET_VSA_ANSWER_REFUSE_PASSAGE; return -1;
        }
    }
    if (winner < 0) { out->status = CNET_VSA_ANSWER_ROUTE_REFUSED; return -1; }
    out->capsule_idx = winner;
    CnetVsaRegisteredCap *e = &reg->capsules[winner];
    out->passages = (int)e->passage_count;
    out->z_min = e->passage_zmin;
    if (!e->passage_text || e->passage_count == 0) { out->status = CNET_VSA_ANSWER_NO_PASSAGES; return -1; }
    t0 = answer_now_us();
    if (!e->passage_q8) {
        /* first answer for this capsule: build and keep its passage vectors */
        int8_t *pq = (int8_t *)malloc((size_t)e->passage_count * CNET_VSA_TOPICAL_DIM);
        float *pn = (float *)malloc(sizeof(float) * e->passage_count);
        if (!pq || !pn) { free(pq); free(pn); out->status = CNET_VSA_ANSWER_NO_PASSAGES; return -1; }
        for (uint32_t i = 0; i < e->passage_count; ++i) {
            int8_t *v = pq + (size_t)i * CNET_VSA_TOPICAL_DIM;
            if (cnet_vsa_gencap_encode_intent_q8(e->passage_text + e->passage_off[i], v, e->encoder_id) != 0) memset(v, 0, CNET_VSA_TOPICAL_DIM);
            pn[i] = cnet_vsa_text_q8_norm(v);
        }
        e->passage_q8 = pq; e->passage_norm = pn;
        out->cold = 1;
    }
    int8_t qv[CNET_VSA_TOPICAL_DIM];
    if (cnet_vsa_gencap_encode_intent_q8(prompt, qv, e->encoder_id) != 0) { out->status = CNET_VSA_ANSWER_ROUTE_REFUSED; return -1; }
    float qn = cnet_vsa_text_q8_norm(qv);
    float sims[CNET_VSA_PASSAGE_MAX];
    double sum = 0.0, sq = 0.0; int n = (int)e->passage_count;
    for (int i = 0; i < n; ++i) {
        sims[i] = cnet_vsa_text_q8_similarity_n(qv, qn, e->passage_q8 + (size_t)i * CNET_VSA_TOPICAL_DIM, e->passage_norm[i]);
        sum += sims[i]; sq += (double)sims[i] * sims[i];
    }
    /* top-k by partial selection */
    int used[CNET_VSA_PASSAGE_MAX]; memset(used, 0, sizeof(int) * (size_t)n);
    for (int j = 0; j < k && j < n; ++j) {
        int bi = -1; float bs = -2.0f;
        for (int i = 0; i < n; ++i) if (!used[i] && sims[i] > bs) { bs = sims[i]; bi = i; }
        used[bi] = 1; out->passage_idx[j] = bi; out->sim[j] = bs; out->n = j + 1;
    }
    float best = out->sim[0];
    if (n >= 3) {
        double mean = (sum - best) / (n - 1);
        double var = ((sq - (double)best * best) - (n - 1) * mean * mean) / (n - 2);
        if (var < 1e-12) var = 1e-12;
        out->z = (float)((best - mean) / sqrt(var));
    } else {
        out->z = out->z_min; /* too few passages to measure: the route decides */
    }
    out->rank_us = answer_now_us() - t0;
    if (out->z < out->z_min) { out->status = CNET_VSA_ANSWER_REFUSE_PASSAGE; return -1; }
    out->status = CNET_VSA_ANSWER_OK;
    return winner;
}

int cnet_vsa_registry_binary_space(const CnetVsaGenRegistry *reg) {
    if (!reg || reg->count == 0 || reg->force_float) return 0;
    for (size_t i = 0; i < reg->count; ++i) {
        const CnetVsaRegisteredCap *c = &reg->capsules[i];
        if (!c->header.certified) continue;
        if (!c->has_topical) return 0;
    }
    return 1;
}

static int registry_add_verified(CnetVsaGenRegistry *reg, const char *filepath, CnetVsaGenCapsule *scratch) {
    if (!reg || !filepath || !*filepath || !scratch) return -1;
    if (reg->count >= CNET_VSA_REGISTRY_MAX_CAPSULES) return -2;
    /* Full read + bounds + digest: a corrupt centroid, receipt or block never
     * reaches the routing scan, not even as a losing entry in the null stats. */
    int rc = cnet_vsa_gencap_load(scratch, filepath);
    if (rc != 0) return rc;
    return cnet_vsa_registry_add_capsule_mem(reg, scratch, filepath);
}

int cnet_vsa_registry_add_capsule(CnetVsaGenRegistry *reg, const char *filepath) {
    CnetVsaGenCapsule *scratch = (CnetVsaGenCapsule *)malloc(sizeof(CnetVsaGenCapsule));
    if (!scratch) return -7;
    int rc = registry_add_verified(reg, filepath, scratch);
    free(scratch);
    return rc;
}

int cnet_vsa_registry_load_dir(CnetVsaGenRegistry *reg, const char *dir_path) {
    if (!reg || !dir_path) return -1;

    DIR *d = opendir(dir_path);
    if (!d) return -2;
    /* the registry's own lexicon (<dir>/registry.lex) certifies its LEX
     * capsules; activate it unless a lexicon is already active. A missing or
     * mismatching table refuses those capsules at admission (-8). */
    cnet_vsa_lexicon_activate_default(dir_path);

    CnetVsaGenCapsule *scratch = (CnetVsaGenCapsule *)malloc(sizeof(CnetVsaGenCapsule));
    if (!scratch) { closedir(d); return -3; }
    struct dirent *dir;
    int loaded = 0;
    while ((dir = readdir(d)) != NULL) {
        const char *name = dir->d_name;
        size_t len = strlen(name);
        if (len > 7 && strcmp(name + len - 7, ".gencap") == 0) {
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, name);
            if (registry_add_verified(reg, full_path, scratch) == 0) {
                loaded++;
            }
        }
    }
    closedir(d);
    free(scratch);
    return loaded;
}

static int route_from_sims(const CnetVsaGenRegistry *reg, const float *sims, int space,
                           CnetVsaRouteResult *out) {
    CnetVsaRouteResult r;
    memset(&r, 0, sizeof(r));
    r.space = space;
    r.status = CNET_VSA_ROUTE_NONE;
    r.best_idx = -1;
    r.second_idx = -1;
    r.best_sim = -2.0f;
    r.second_sim = -2.0f;
    r.best_dist = 1.0f;
    r.margin_ok = 1;

    if (!reg || !sims || reg->count == 0) {
        if (out) *out = r;
        return -1;
    }

    double sum = 0.0, sumsq = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < reg->count; ++i) {
        const CnetVsaRegisteredCap *c = &reg->capsules[i];
        if (!c->header.certified) continue;
        float sim = sims[i];
        sum += sim; sumsq += (double)sim * sim; n++;
        if (sim > r.best_sim) {
            r.second_sim = r.best_sim; r.second_idx = r.best_idx;
            r.best_sim = sim; r.best_idx = (int)i;
        } else if (sim > r.second_sim) {
            r.second_sim = sim; r.second_idx = (int)i;
        }
    }
    if (r.best_idx < 0) {
        if (out) *out = r;
        return -1;
    }

    r.best_dist = 1.0f - r.best_sim;
    r.radius = (space == CNET_VSA_ROUTE_SPACE_BINARY)
             ? reg->capsules[r.best_idx].topical_radius
             : reg->capsules[r.best_idx].header.safe_radius;
    r.radius_ok = (r.best_dist <= r.radius) ? 1 : 0;
    if (r.second_idx >= 0) r.gap = r.best_sim - r.second_sim;

    size_t m = n - 1;
    if (m >= reg->min_null_count && m >= 2) {
        double s1 = sum - r.best_sim;
        double s2 = sumsq - (double)r.best_sim * r.best_sim;
        double mean = s1 / (double)m;
        double var = (s2 - (double)m * mean * mean) / (double)(m - 1);
        if (var < 1e-12) var = 1e-12;
        r.null_mean = (float)mean;
        r.null_std = (float)sqrt(var);
        r.z = (float)((r.best_sim - mean) / sqrt(var));
        r.z_min = (float)sqrt(2.0 * log((double)m)) + reg->z_margin;
        r.margin_checked = 1;
        r.margin_ok = (r.z >= r.z_min) ? 1 : 0;
        r.ambiguous = (r.second_idx >= 0 && r.gap < r.null_std) ? 1 : 0;
    }
    r.ambiguity_ok = 1;
    float amb_k = (space == CNET_VSA_ROUTE_SPACE_WIDE) ? reg->ambiguity_k_wide : reg->ambiguity_k_float;
    if (r.margin_checked && amb_k > 0.0f && r.second_idx >= 0 && r.gap < amb_k * r.null_std) {
        r.ambiguity_ok = 0;
    }

    if (!r.radius_ok) r.status = CNET_VSA_ROUTE_REFUSE_RADIUS;
    else if (!r.margin_ok) r.status = CNET_VSA_ROUTE_REFUSE_MARGIN;
    else if (!r.ambiguity_ok) r.status = CNET_VSA_ROUTE_REFUSE_AMBIGUOUS;
    else r.status = CNET_VSA_ROUTE_ACCEPT;

    if (out) *out = r;
    return (r.status == CNET_VSA_ROUTE_ACCEPT) ? r.best_idx : -1;
}

int cnet_vsa_registry_route_ex(const CnetVsaGenRegistry *reg, const float *query_vec,
                               CnetVsaRouteResult *out) {
    if (!reg || !query_vec || reg->count == 0) {
        return route_from_sims(reg, NULL, CNET_VSA_ROUTE_SPACE_FLOAT, out);
    }
    float sims[CNET_VSA_REGISTRY_MAX_CAPSULES];
    for (size_t i = 0; i < reg->count; ++i) {
        const CnetVsaRegisteredCap *c = &reg->capsules[i];
        if (!c->header.certified) {
            sims[i] = -2.0f;
            continue;
        }
        sims[i] = cnet_vsa_similarity(query_vec, c->header.centroid, reg->dim);
    }
    return route_from_sims(reg, sims, CNET_VSA_ROUTE_SPACE_FLOAT, out);
}

/* Wide-space representations: encoders that share a word key and vector rule
 * share one query encoding. Plain hashes (BAG/HD/CGRAM_C), stemmed hashes
 * (STEM/STEM_BI), and the learned lexicon (LEX) are three distinct spaces. */
#define WIDE_REP_COUNT 3
static int wide_rep_of(uint32_t enc) {
    if (enc == CNET_VSA_ENCODER_LEX) return 2;
    if (enc == CNET_VSA_ENCODER_STEM || enc == CNET_VSA_ENCODER_STEM_BI) return 1;
    return 0;
}
static uint32_t wide_rep_encoder(int rep) {
    return rep == 2 ? CNET_VSA_ENCODER_LEX : (rep == 1 ? CNET_VSA_ENCODER_STEM : CNET_VSA_ENCODER_BAG);
}

static int term_gate_check(const CnetVsaGenRegistry *reg, const char *prompt, CnetVsaRouteResult *r);

static int route_query_wide(const CnetVsaGenRegistry *reg, const char *prompt,
                            CnetVsaRouteResult *out) {
    /* Local scratch keeps overlapping queries from overwriting each other's vectors. */
    int8_t qq[WIDE_REP_COUNT][CNET_VSA_TOPICAL_DIM];
    float qn[WIDE_REP_COUNT];
    int haveq[WIDE_REP_COUNT];
    memset(haveq, 0, sizeof(haveq));
    for (size_t i = 0; i < reg->count; ++i) {
        const CnetVsaRegisteredCap *c = &reg->capsules[i];
        if (!c->header.certified) continue;
        int rep = wide_rep_of(c->encoder_id);
        if (!haveq[rep]) {
            if (cnet_vsa_gencap_encode_intent_q8(prompt, qq[rep], wide_rep_encoder(rep)) != 0) {
                return route_from_sims(reg, NULL, CNET_VSA_ROUTE_SPACE_WIDE, out);
            }
            qn[rep] = cnet_vsa_text_q8_norm(qq[rep]);
            haveq[rep] = 1;
        }
    }
    float wsims[CNET_VSA_REGISTRY_MAX_CAPSULES];
    for (size_t i = 0; i < reg->count; ++i) {
        const CnetVsaRegisteredCap *c = &reg->capsules[i];
        if (!c->header.certified) { wsims[i] = -2.0f; continue; }
        int rep = wide_rep_of(c->encoder_id);
        wsims[i] = cnet_vsa_text_q8_similarity_n(qq[rep], qn[rep], c->topical, c->topical_norm);
    }
    CnetVsaRouteResult r;
    int winner = route_from_sims(reg, wsims, CNET_VSA_ROUTE_SPACE_WIDE, &r);
    if (winner >= 0 && reg->term_gate) winner = term_gate_check(reg, prompt, &r);
    if (out) *out = r;
    return winner;
}

/* Term-dependence gate: re-encode the prompt with each content word left out
 * and require every variant to stay inside the winner's radius; a prompt with
 * fewer than two content words is refused. Runs only on an accept, so
 * abstentions cost nothing; an accept costs one encode per content word. */
static int term_gate_check(const CnetVsaGenRegistry *reg, const char *prompt, CnetVsaRouteResult *r) {
    CnetVsaTokenList tl;
    r->term_checked = 1;
    r->term_ok = 0;
    r->term_words = 0;
    r->term_worst_dist = 0.0f;
    r->term_word[0] = 0;
    if (cnet_vsa_text_tokenize(prompt, &tl) <= 0) { r->status = CNET_VSA_ROUTE_REFUSE_TERM; return -1; }
    size_t content[CNET_VSA_MAX_TOKENS]; int nc = 0;
    for (size_t i = 0; i < tl.count && nc < CNET_VSA_MAX_TOKENS; ++i)
        if (!cnet_vsa_text_is_stopword(tl.tokens[i].token)) content[nc++] = i;
    r->term_words = nc;
    if (nc < 2) {
        if (nc == 1) snprintf(r->term_word, sizeof(r->term_word), "%s", tl.tokens[content[0]].token);
        r->status = CNET_VSA_ROUTE_REFUSE_TERM;
        return -1;
    }
    const CnetVsaRegisteredCap *w = &reg->capsules[r->best_idx];
    uint32_t enc = wide_rep_encoder(wide_rep_of(w->encoder_id));
    char *buf = (char *)malloc(tl.count * (CNET_VSA_TOKEN_LEN + 1) + 1);
    if (!buf) { r->status = CNET_VSA_ROUTE_REFUSE_TERM; return -1; }
    int8_t vq[CNET_VSA_TOPICAL_DIM];
    for (int k = 0; k < nc; ++k) {
        size_t pos = 0;
        for (size_t i = 0; i < tl.count; ++i) {
            if (i == content[k]) continue;
            size_t len = strlen(tl.tokens[i].token);
            memcpy(buf + pos, tl.tokens[i].token, len); pos += len; buf[pos++] = ' ';
        }
        buf[pos] = 0;
        if (cnet_vsa_gencap_encode_intent_q8(buf, vq, enc) != 0) { free(buf); r->status = CNET_VSA_ROUTE_REFUSE_TERM; return -1; }
        float d = 1.0f - cnet_vsa_text_q8_similarity_n(vq, cnet_vsa_text_q8_norm(vq), w->topical, w->topical_norm);
        if (d > r->term_worst_dist) r->term_worst_dist = d;
        if (d > r->radius) {
            snprintf(r->term_word, sizeof(r->term_word), "%s", tl.tokens[content[k]].token);
            free(buf);
            r->status = CNET_VSA_ROUTE_REFUSE_TERM;
            return -1;
        }
    }
    free(buf);
    r->term_ok = 1;
    return r->best_idx;
}

static int route_query_float(const CnetVsaGenRegistry *reg, const char *prompt,
                             CnetVsaRouteResult *out) {
    static const uint32_t kCount = CNET_VSA_ENCODER_COUNT;
    /* Float space: encode the prompt once per encoder that is actually present
     * in the registry, then score each capsule against its own encoder's vector. */
    float q[CNET_VSA_ENCODER_COUNT][CNET_VSA_DEFAULT_DIM];
    int have[CNET_VSA_ENCODER_COUNT];
    memset(have, 0, sizeof(have));
    for (size_t i = 0; i < reg->count; ++i) {
        const CnetVsaRegisteredCap *c = &reg->capsules[i];
        if (!c->header.certified) continue;
        uint32_t enc = c->encoder_id < kCount ? c->encoder_id : CNET_VSA_ENCODER_BAG;
        if (!have[enc]) {
            if (cnet_vsa_gencap_encode_intent_ex(prompt, q[enc], reg->dim, enc) != 0) {
                return route_from_sims(reg, NULL, CNET_VSA_ROUTE_SPACE_FLOAT, out);
            }
            have[enc] = 1;
        }
    }
    float sims[CNET_VSA_REGISTRY_MAX_CAPSULES];
    for (size_t i = 0; i < reg->count; ++i) {
        const CnetVsaRegisteredCap *c = &reg->capsules[i];
        if (!c->header.certified) {
            sims[i] = -2.0f;
            continue;
        }
        uint32_t enc = c->encoder_id < kCount ? c->encoder_id : CNET_VSA_ENCODER_BAG;
        sims[i] = cnet_vsa_similarity(q[enc], c->header.centroid, reg->dim);
    }
    return route_from_sims(reg, sims, CNET_VSA_ROUTE_SPACE_FLOAT, out);
}

int cnet_vsa_registry_route_query(const CnetVsaGenRegistry *reg, const char *prompt,
                                  CnetVsaRouteResult *out) {
    if (!reg || !prompt || reg->count == 0) {
        return route_from_sims(reg, NULL, CNET_VSA_ROUTE_SPACE_FLOAT, out);
    }
    /* Separate paths keep each call's scratch bounded by its own encoding. */
    if (cnet_vsa_registry_binary_space(reg)) return route_query_wide(reg, prompt, out);
    return route_query_float(reg, prompt, out);
}

int cnet_vsa_registry_route(const CnetVsaGenRegistry *reg, const float *query_vec,
                            int *out_best_idx, float *out_best_dist) {
    CnetVsaRouteResult r;
    int w = cnet_vsa_registry_route_ex(reg, query_vec, &r);
    if (out_best_idx) *out_best_idx = r.best_idx;
    if (out_best_dist) *out_best_dist = r.best_dist;
    return w;
}

int cnet_vsa_registry_dispatch(const CnetVsaGenRegistry *reg,
                               const char *prompt,
                               char *out_text,
                               size_t out_text_size,
                               char *out_capsule_name,
                               float *out_dist) {
    if (!reg || !prompt || !out_text || out_text_size < 32) return -1;

    CnetVsaRouteResult rr;
    int route_idx = cnet_vsa_registry_route_query(reg, prompt, &rr);
    int best_idx = rr.best_idx;
    float best_dist = rr.best_dist;

    if (out_dist) *out_dist = best_dist;

    if (route_idx < 0) {
        const char *cand_name = (best_idx >= 0) ? reg->capsules[best_idx].header.name : "none";
        float lim = (best_idx >= 0) ? rr.radius : 0.900f;
        if (rr.status == CNET_VSA_ROUTE_REFUSE_MARGIN) {
            snprintf(out_text, out_text_size,
                     "ABSTAIN: no_certified_capsule_in_domain (closest='%s', dist=%.3f, limit=%.3f, "
                     "margin z=%.2f < z_min=%.2f over %zu others)",
                     cand_name, best_dist, lim, rr.z, rr.z_min, reg->count - 1);
        } else if (rr.status == CNET_VSA_ROUTE_REFUSE_TERM) {
            if (rr.term_words < 2)
                snprintf(out_text, out_text_size,
                         "ABSTAIN: single_term_prompt (closest='%s', %d content word%s; a route needs at least two)",
                         cand_name, rr.term_words, rr.term_words == 1 ? "" : "s");
            else
                snprintf(out_text, out_text_size,
                         "ABSTAIN: single_term_dependence (closest='%s', without '%s' dist=%.3f > limit=%.3f)",
                         cand_name, rr.term_word, rr.term_worst_dist, lim);
        } else if (rr.status == CNET_VSA_ROUTE_REFUSE_AMBIGUOUS) {
            const char *second = (rr.second_idx >= 0) ? reg->capsules[rr.second_idx].header.name : "none";
            float amb_k = (rr.space == CNET_VSA_ROUTE_SPACE_WIDE) ? reg->ambiguity_k_wide : reg->ambiguity_k_float;
            snprintf(out_text, out_text_size,
                     "ABSTAIN: ambiguous_between_capsules (closest='%s', runner_up='%s', gap=%.4f < %.2f*sd=%.4f)",
                     cand_name, second, rr.gap, amb_k, amb_k * rr.null_std);
        } else {
            snprintf(out_text, out_text_size,
                     "ABSTAIN: no_certified_capsule_in_domain (closest='%s', dist=%.3f, limit=%.3f)",
                     cand_name, best_dist, lim);
        }
        if (out_capsule_name) snprintf(out_capsule_name, 64, "%s", cand_name);
        return -3;
    }

    const CnetVsaRegisteredCap *winner = &reg->capsules[route_idx];
    if (out_capsule_name) snprintf(out_capsule_name, 64, "%s", winner->header.name);

    /* Allocate and load winning capsule */
    CnetVsaGenCapsule *cap = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule));
    if (!cap) return -4;

    int load_rc = cnet_vsa_gencap_load(cap, winner->filepath);
    if (load_rc != 0) {
        free(cap);
        snprintf(out_text, out_text_size, "ERROR: winning capsule corrupt or tampered (rc=%d)", load_rc);
        return -5;
    }

    float query_vec[CNET_VSA_DEFAULT_DIM];
    if (encode_for_cap(cap, prompt, query_vec) != 0) {
        free(cap);
        snprintf(out_text, out_text_size, "ERROR: failed to encode query intent");
        return -2;
    }

    int toks = 0;
    int gen_rc;
    if (rr.space == CNET_VSA_ROUTE_SPACE_WIDE && cap->topical.present) {
        int8_t q8[CNET_VSA_TOPICAL_DIM];
        if (cnet_vsa_gencap_encode_intent_q8(prompt, q8, cnet_vsa_gencap_encoder_id(cap)) != 0) {
            free(cap);
            snprintf(out_text, out_text_size, "ERROR: failed to encode query vector");
            return -2;
        }
        gen_rc = cnet_vsa_gencap_generate_ex(cap, "the", query_vec, q8, 0.45f, 28, out_text, out_text_size, &toks);
    } else {
        gen_rc = cnet_vsa_gencap_generate(cap, "the", query_vec, 0.45f, 28, out_text, out_text_size, &toks);
    }
    free(cap);
    return gen_rc;
}
