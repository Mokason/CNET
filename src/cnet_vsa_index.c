#include "../include/cnet_vsa_index.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cnet_vsa_index_init(CnetVsaMetricIndex *idx, int dim, size_t max_clusters) {
    if (!idx || dim <= 0 || max_clusters == 0) return -1;
    idx->dim = dim;
    idx->total_items = 0;
    idx->cluster_count = 0;
    idx->cluster_capacity = max_clusters;
    idx->clusters = (CnetVsaCluster *)calloc(max_clusters, sizeof(CnetVsaCluster));
    if (!idx->clusters) return -1;
    return 0;
}

void cnet_vsa_index_free(CnetVsaMetricIndex *idx) {
    if (!idx) return;
    free(idx->clusters);
    memset(idx, 0, sizeof(*idx));
}

int cnet_vsa_index_insert(CnetVsaMetricIndex *idx, const char *name, const float *vec) {
    if (!idx || !name || !vec || idx->cluster_capacity == 0) return -1;

    if (idx->cluster_count == 0) {
        /* First cluster */
        CnetVsaCluster *c = &idx->clusters[0];
        memcpy(c->centroid, vec, (size_t)idx->dim * sizeof(float));
        c->radius = 0.0f;
        c->count = 0;

        CnetVsaIndexEntry *e = &c->entries[c->count++];
        e->id = (int)idx->total_items + 1;
        strncpy(e->name, name, CNET_VSA_NAME_MAX - 1);
        e->name[CNET_VSA_NAME_MAX - 1] = '\0';
        memcpy(e->vector, vec, (size_t)idx->dim * sizeof(float));

        idx->cluster_count = 1;
        idx->total_items = 1;
        return 0;
    }

    /* Find closest existing cluster */
    float min_dist = 1e9f;
    size_t best_c = 0;
    for (size_t i = 0; i < idx->cluster_count; ++i) {
        float d = cnet_vsa_distance(idx->clusters[i].centroid, vec, idx->dim);
        if (d < min_dist) {
            min_dist = d;
            best_c = i;
        }
    }

    /* If closest cluster is reasonably close and has room, insert into it */
    const float CLUSTER_SPLIT_THRESHOLD = 0.85f;
    if (min_dist < CLUSTER_SPLIT_THRESHOLD &&
        idx->clusters[best_c].count < CNET_VSA_INDEX_MAX_PER_CLUSTER) {
        CnetVsaCluster *c = &idx->clusters[best_c];
        CnetVsaIndexEntry *e = &c->entries[c->count++];
        e->id = (int)idx->total_items + 1;
        strncpy(e->name, name, CNET_VSA_NAME_MAX - 1);
        e->name[CNET_VSA_NAME_MAX - 1] = '\0';
        memcpy(e->vector, vec, (size_t)idx->dim * sizeof(float));

        if (min_dist > c->radius) {
            c->radius = min_dist;
        }
        idx->total_items++;
        return 0;
    }

    /* Otherwise, spawn new cluster if capacity allows */
    if (idx->cluster_count < idx->cluster_capacity) {
        CnetVsaCluster *c = &idx->clusters[idx->cluster_count++];
        memcpy(c->centroid, vec, (size_t)idx->dim * sizeof(float));
        c->radius = 0.0f;
        c->count = 0;

        CnetVsaIndexEntry *e = &c->entries[c->count++];
        e->id = (int)idx->total_items + 1;
        strncpy(e->name, name, CNET_VSA_NAME_MAX - 1);
        e->name[CNET_VSA_NAME_MAX - 1] = '\0';
        memcpy(e->vector, vec, (size_t)idx->dim * sizeof(float));

        idx->total_items++;
        return 0;
    }

    /* Fallback: insert into best existing cluster */
    if (idx->clusters[best_c].count < CNET_VSA_INDEX_MAX_PER_CLUSTER) {
        CnetVsaCluster *c = &idx->clusters[best_c];
        CnetVsaIndexEntry *e = &c->entries[c->count++];
        e->id = (int)idx->total_items + 1;
        strncpy(e->name, name, CNET_VSA_NAME_MAX - 1);
        e->name[CNET_VSA_NAME_MAX - 1] = '\0';
        memcpy(e->vector, vec, (size_t)idx->dim * sizeof(float));

        if (min_dist > c->radius) c->radius = min_dist;
        idx->total_items++;
        return 0;
    }

    return -1;
}

int cnet_vsa_index_query(const CnetVsaMetricIndex *idx, const float *query_vec,
                          float *out_clean, char *out_name, size_t name_cap,
                          float *out_sim, size_t *out_clusters_visited) {
    if (!idx || !query_vec || idx->total_items == 0) return -1;

    size_t visited = 0;
    float best_sim = -2.0f;
    float best_dist = 2.0f;
    const CnetVsaIndexEntry *best_entry = NULL;

    /* Phase 1: Compute distance from query to each cluster centroid */
    float c_dists[CNET_VSA_INDEX_MAX_CLUSTERS];
    size_t order[CNET_VSA_INDEX_MAX_CLUSTERS];
    for (size_t i = 0; i < idx->cluster_count; ++i) {
        c_dists[i] = cnet_vsa_distance(idx->clusters[i].centroid, query_vec, idx->dim);
        order[i] = i;
    }

    /* Sort clusters by centroid distance ascending (nearest first) */
    for (size_t i = 0; i < idx->cluster_count; ++i) {
        for (size_t j = i + 1; j < idx->cluster_count; ++j) {
            if (c_dists[order[j]] < c_dists[order[i]]) {
                size_t tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
        }
    }

    /* Phase 2: Traverse clusters with branch-and-bound pruning */
    for (size_t i = 0; i < idx->cluster_count; ++i) {
        size_t c_idx = order[i];
        const CnetVsaCluster *c = &idx->clusters[c_idx];
        float d_centroid = c_dists[c_idx];

        /* Triangle inequality lower bound:
           Distance to any point in cluster c is at least d_centroid - c->radius.
           If this lower bound >= best_dist found so far, prune this cluster and all later ones! */
        if (best_entry != NULL && (d_centroid - c->radius) >= best_dist) {
            /* Sub-linear pruning: skip scanning this cluster */
            continue;
        }

        visited++;

        /* Scan items in this cluster */
        for (size_t j = 0; j < c->count; ++j) {
            const CnetVsaIndexEntry *e = &c->entries[j];
            float sim = cnet_vsa_similarity(query_vec, e->vector, idx->dim);
            if (sim > best_sim) {
                best_sim = sim;
                best_dist = (float)sqrt(fmaxf(0.0f, 2.0f - 2.0f * sim));
                best_entry = e;
            }
        }
    }

    if (!best_entry) return -1;

    if (out_clean) {
        memcpy(out_clean, best_entry->vector, (size_t)idx->dim * sizeof(float));
    }
    if (out_name && name_cap > 0) {
        strncpy(out_name, best_entry->name, name_cap - 1);
        out_name[name_cap - 1] = '\0';
    }
    if (out_sim) {
        *out_sim = best_sim;
    }
    if (out_clusters_visited) {
        *out_clusters_visited = visited;
    }
    return 0;
}
