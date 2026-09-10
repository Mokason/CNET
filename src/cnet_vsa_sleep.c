#include "../include/cnet_vsa_sleep.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int cnet_vsa_sleep_init(CnetVsaSleepConsolidator *sc, int dim, uint32_t min_access, float min_coherence) {
    if (!sc || dim <= 0) return -1;
    memset(sc, 0, sizeof(*sc));
    sc->dim = dim;
    sc->min_access_threshold = min_access > 0 ? min_access : 2;
    sc->min_coherence_threshold = min_coherence > 0.0f ? min_coherence : 0.65f;
    return 0;
}

void cnet_vsa_sleep_free(CnetVsaSleepConsolidator *sc) {
    if (!sc) return;
    memset(sc, 0, sizeof(*sc));
}

int cnet_vsa_sleep_record_event(CnetVsaSleepConsolidator *sc, const char *label, const char *text,
                                 const float *vec, uint32_t access_count,
                                 float confidence, bool contract_verified) {
    if (!sc || !label || !text || !vec || sc->event_count >= CNET_VSA_SLEEP_MAX_BUFFER) {
        return -1;
    }

    CnetVsaSleepEvent *ev = &sc->events[sc->event_count++];
    ev->node_id = (int)sc->event_count;
    snprintf(ev->label, sizeof(ev->label), "%s", label);
    snprintf(ev->text, sizeof(ev->text), "%s", text);
    memcpy(ev->vector, vec, (size_t)sc->dim * sizeof(float));
    ev->access_count = access_count;
    ev->confidence = confidence;
    ev->contract_verified = contract_verified;
    ev->timestamp = sc->event_count;

    return ev->node_id;
}

int cnet_vsa_sleep_consolidate(CnetVsaSleepConsolidator *sc, CnetVsaCodebook *ltm_codebook,
                                size_t *out_pruned, size_t *out_promoted) {
    if (!sc || !ltm_codebook) return -1;

    size_t pruned = 0;
    size_t promoted = 0;

    /* Phase 1: Filter verified persistent events */
    int valid_indices[CNET_VSA_SLEEP_MAX_BUFFER];
    size_t valid_count = 0;

    for (size_t i = 0; i < sc->event_count; ++i) {
        CnetVsaSleepEvent *ev = &sc->events[i];
        if (ev->contract_verified && ev->access_count >= sc->min_access_threshold) {
            valid_indices[valid_count++] = (int)i;
        } else {
            pruned++;
        }
    }

    /* Phase 2: Cluster valid events into prototypes */
    sc->cluster_count = 0;

    for (size_t k = 0; k < valid_count; ++k) {
        CnetVsaSleepEvent *ev = &sc->events[valid_indices[k]];
        int best_cluster = -1;
        float best_sim = -2.0f;

        for (size_t c = 0; c < sc->cluster_count; ++c) {
            float sim = cnet_vsa_similarity(ev->vector, sc->prototypes[c].centroid, sc->dim);
            if (sim > best_sim) {
                best_sim = sim;
                best_cluster = (int)c;
            }
        }

        if (best_sim >= 0.55f && best_cluster >= 0) {
            /* Merge into existing cluster */
            CnetVsaConsolidatedPrototype *cp = &sc->prototypes[best_cluster];
            for (int j = 0; j < sc->dim; ++j) {
                cp->centroid[j] = cp->centroid[j] * (float)cp->member_count + ev->vector[j];
            }
            cnet_vsa_normalize(cp->centroid, sc->dim);
            cp->member_count++;
        } else if (sc->cluster_count < CNET_VSA_SLEEP_MAX_CLUSTERS) {
            /* Create new prototype cluster */
            CnetVsaConsolidatedPrototype *cp = &sc->prototypes[sc->cluster_count++];
            cp->cluster_id = (int)sc->cluster_count;
            char src_label[CNET_VSA_NAME_MAX];
            snprintf(src_label, sizeof(src_label), "%.48s", ev->label);
            snprintf(cp->prototype_label, sizeof(cp->prototype_label), "proto_%.48s", src_label);
            memcpy(cp->centroid, ev->vector, (size_t)sc->dim * sizeof(float));
            cp->member_count = 1;
            cp->coherence = 1.0f;
        }
    }

    /* Phase 3: Evaluate coherence and promote to LTM */
    for (size_t c = 0; c < sc->cluster_count; ++c) {
        CnetVsaConsolidatedPrototype *cp = &sc->prototypes[c];
        if (cp->member_count >= 2) {
            /* Compute intra-cluster coherence */
            float sum_sim = 0.0f;
            size_t count_mem = 0;
            for (size_t k = 0; k < valid_count; ++k) {
                CnetVsaSleepEvent *ev = &sc->events[valid_indices[k]];
                float sim = cnet_vsa_similarity(ev->vector, cp->centroid, sc->dim);
                if (sim >= 0.50f) {
                    sum_sim += sim;
                    count_mem++;
                }
            }
            cp->coherence = count_mem > 0 ? (sum_sim / (float)count_mem) : 0.0f;

            if (cp->coherence >= sc->min_coherence_threshold) {
                /* Promote to permanent LTM codebook */
                cnet_vsa_codebook_add(ltm_codebook, cp->prototype_label, cp->centroid);
                promoted++;
            }
        }
    }

    sc->total_pruned_count += pruned;
    sc->total_promoted_count += promoted;
    sc->event_count = 0; /* Clear compacted wake buffer */

    if (out_pruned) *out_pruned = pruned;
    if (out_promoted) *out_promoted = promoted;

    return 0;
}
