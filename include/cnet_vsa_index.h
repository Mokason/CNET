#ifndef CNET_VSA_INDEX_H
#define CNET_VSA_INDEX_H

#include "cnet_vsa.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_VSA_INDEX_MAX_CLUSTERS 128
#define CNET_VSA_INDEX_MAX_PER_CLUSTER 512

typedef struct {
    int id;
    char name[CNET_VSA_NAME_MAX];
    float vector[CNET_VSA_DEFAULT_DIM];
} CnetVsaIndexEntry;

typedef struct {
    float centroid[CNET_VSA_DEFAULT_DIM];
    float radius; /* maximum distance from centroid to any member in cluster */
    size_t count;
    CnetVsaIndexEntry entries[CNET_VSA_INDEX_MAX_PER_CLUSTER];
} CnetVsaCluster;

typedef struct {
    int dim;
    size_t total_items;
    size_t cluster_count;
    size_t cluster_capacity;
    CnetVsaCluster *clusters;
} CnetVsaMetricIndex;

/* Initialize hierarchical metric cluster index */
int  cnet_vsa_index_init(CnetVsaMetricIndex *idx, int dim, size_t max_clusters);
void cnet_vsa_index_free(CnetVsaMetricIndex *idx);

/* Insert a named vector into the index. Automatically routes to closest cluster or spawns a new cluster. */
int  cnet_vsa_index_insert(CnetVsaMetricIndex *idx, const char *name, const float *vec);

/* Sub-linear query: uses cluster bounds and triangle inequality to prune distant clusters.
   Returns best match clean vector, name, and cosine similarity. Records clusters visited. */
int  cnet_vsa_index_query(const CnetVsaMetricIndex *idx, const float *query_vec,
                          float *out_clean, char *out_name, size_t name_cap,
                          float *out_sim, size_t *out_clusters_visited);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_INDEX_H */
