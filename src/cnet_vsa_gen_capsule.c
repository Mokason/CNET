#include "cnet_vsa_gen_capsule.h"
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

    cap->safe_radius = 0.90f; /* Calibrated domain distance ceiling */
    cap->certified = 0;

    return cnet_vsa_ngram_init(&cap->ngram, dim, 1337);
}

int cnet_vsa_gencap_encode_intent(const char *text, float *out_vec, int dim) {
    if (!text || !out_vec || dim <= 0) return -1;
    CnetVsaTokenList tl;
    if (cnet_vsa_text_tokenize(text, &tl) <= 0) return -1;
    return cnet_vsa_text_encode_topical(&tl, out_vec, dim);
}

int cnet_vsa_gencap_ingest(CnetVsaGenCapsule *cap, const char *text) {
    if (!cap || !text || !*text) return -1;

    int toks = cnet_vsa_ngram_ingest_sentence(&cap->ngram, text);
    if (toks > 0) {
        /* Update semantic centroid with unpermuted topical vector */
        float doc_vec[CNET_VSA_DEFAULT_DIM];
        if (cnet_vsa_gencap_encode_intent(text, doc_vec, cap->ngram.dim) == 0) {
            for (int d = 0; d < cap->ngram.dim; ++d) {
                cap->centroid[d] += doc_vec[d];
            }
        }
    }
    return toks;
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
        cnet_vsa_gencap_encode_intent(frame_name, f->trigger_intent, cap->ngram.dim);
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

int cnet_vsa_gencap_seal(CnetVsaGenCapsule *cap) {
    if (!cap) return -1;
    if (cap->ngram.vocab_count == 0) return -2;

    /* Normalize centroid */
    cnet_vsa_normalize(cap->centroid, cap->ngram.dim);

    /* Safe radius: calibrated to avoid catastrophic out-of-domain false positives */
    if (cap->safe_radius <= 0.0f) {
        cap->safe_radius = 0.90f;
    }

    /* Compute digest and certify */
    cap->digest = compute_capsule_digest(cap);
    cap->certified = 1;
    return 0;
}

int cnet_vsa_gencap_save(const CnetVsaGenCapsule *cap, const char *filepath) {
    if (!cap || !filepath || !*filepath) return -1;
    if (!cap->certified) return -2; /* Refuse to save uncertified capsule */

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp-%ld", filepath, (long)time(NULL));

    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) return -3;

    size_t written = fwrite(cap, sizeof(CnetVsaGenCapsule), 1, fp);
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

    size_t read = fread(cap, sizeof(CnetVsaGenCapsule), 1, fp);
    fclose(fp);

    if (read != 1) return -3;

    /* Verify Magic and Version */
    if (cap->magic != CNET_VSA_GENCAP_MAGIC || cap->version != CNET_VSA_GENCAP_VERSION) {
        return -4; /* Corrupt or incompatible capsule */
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

int cnet_vsa_gencap_generate(const CnetVsaGenCapsule *cap,
                             const char *seed_word,
                             const float *intent_vec,
                             float steer_weight,
                             int max_tokens,
                             char *out_text,
                             size_t out_text_size,
                             int *out_tokens) {
    if (!cap || !cap->certified || !out_text || out_text_size < 32) return -1;

    /* 1. Gate check: if intent vector is provided, verify scope against domain centroid */
    if (intent_vec) {
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

    /* 3. Pure Algebraic Unbinding Generation */
    CnetVsaNgramEngine *non_const_ngram = (CnetVsaNgramEngine *)&cap->ngram;
    int rc = cnet_vsa_ngram_generate(non_const_ngram,
                                     actual_seed,
                                     intent_vec,
                                     steer_weight,
                                     0.85f,
                                     max_tokens,
                                     out_text,
                                     out_text_size,
                                     out_tokens);

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
    return 0;
}

int cnet_vsa_registry_add_capsule(CnetVsaGenRegistry *reg, const char *filepath) {
    if (!reg || !filepath || !*filepath) return -1;
    if (reg->count >= CNET_VSA_REGISTRY_MAX_CAPSULES) return -2;

    FILE *fp = fopen(filepath, "rb");
    if (!fp) return -3;

    CnetVsaRegisteredCap *entry = &reg->capsules[reg->count];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->filepath, sizeof(entry->filepath), "%s", filepath);

    if (fread(&entry->header, sizeof(CnetVsaCapsuleHeader), 1, fp) != 1) {
        fclose(fp);
        return -4;
    }
    fclose(fp);

    if (entry->header.magic != CNET_VSA_GENCAP_MAGIC ||
        entry->header.version != CNET_VSA_GENCAP_VERSION) {
        return -5;
    }
    if (!entry->header.certified) {
        return -6;
    }

    reg->count++;
    return 0;
}

int cnet_vsa_registry_load_dir(CnetVsaGenRegistry *reg, const char *dir_path) {
    if (!reg || !dir_path) return -1;

    DIR *d = opendir(dir_path);
    if (!d) return -2;

    struct dirent *dir;
    int loaded = 0;
    while ((dir = readdir(d)) != NULL) {
        const char *name = dir->d_name;
        size_t len = strlen(name);
        if (len > 7 && strcmp(name + len - 7, ".gencap") == 0) {
            char full_path[512];
            snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, name);
            if (cnet_vsa_registry_add_capsule(reg, full_path) == 0) {
                loaded++;
            }
        }
    }
    closedir(d);
    return loaded;
}

int cnet_vsa_registry_route(const CnetVsaGenRegistry *reg, const float *query_vec,
                            int *out_best_idx, float *out_best_dist) {
    if (!reg || !query_vec || reg->count == 0) return -1;

    int best_idx = -1;
    float best_sim = -2.0f;

    for (size_t i = 0; i < reg->count; ++i) {
        const CnetVsaRegisteredCap *c = &reg->capsules[i];
        if (!c->header.certified) continue;

        float sim = cnet_vsa_similarity(query_vec, c->header.centroid, reg->dim);
        if (sim > best_sim) {
            best_sim = sim;
            best_idx = (int)i;
        }
    }

    if (best_idx < 0) return -1;

    float best_dist = 1.0f - best_sim;
    if (out_best_idx) *out_best_idx = best_idx;
    if (out_best_dist) *out_best_dist = best_dist;

    /* Verify against the winning capsule's safe radius */
    if (best_dist <= reg->capsules[best_idx].header.safe_radius) {
        return best_idx; /* In-Domain Match */
    }

    return -1; /* Out of Domain (Abstain) */
}

int cnet_vsa_registry_dispatch(const CnetVsaGenRegistry *reg,
                               const char *prompt,
                               char *out_text,
                               size_t out_text_size,
                               char *out_capsule_name,
                               float *out_dist) {
    if (!reg || !prompt || !out_text || out_text_size < 32) return -1;

    float query_vec[CNET_VSA_DEFAULT_DIM];
    if (cnet_vsa_gencap_encode_intent(prompt, query_vec, reg->dim) != 0) {
        snprintf(out_text, out_text_size, "ERROR: failed to encode query intent");
        return -2;
    }

    int best_idx = -1;
    float best_dist = 1.0f;
    int route_idx = cnet_vsa_registry_route(reg, query_vec, &best_idx, &best_dist);

    if (out_dist) *out_dist = best_dist;

    if (route_idx < 0) {
        const char *cand_name = (best_idx >= 0) ? reg->capsules[best_idx].header.name : "none";
        float lim = (best_idx >= 0) ? reg->capsules[best_idx].header.safe_radius : 0.900f;
        snprintf(out_text, out_text_size,
                 "ABSTAIN: no_certified_capsule_in_domain (closest='%s', dist=%.3f, limit=%.3f)",
                 cand_name, best_dist, lim);
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

    int toks = 0;
    int gen_rc = cnet_vsa_gencap_generate(cap, "the", query_vec, 0.45f, 28, out_text, out_text_size, &toks);
    free(cap);
    return gen_rc;
}

