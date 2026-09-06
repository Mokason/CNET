#ifndef CNET_ALLOCATOR_DATA_H
#define CNET_ALLOCATOR_DATA_H
#include "cnet_core_allocator.h"
typedef struct {
    size_t n,rows;
    CnetAllocatorEpisode episode[CNET_ALLOCATOR_MAX_EPISODES];
    char sha256[65];
} AllocatorData;
/* ROWS contains independent outcome masks/receipts. TASKS has predecision data
 * only and is the sole accepted format for live choose. Both are bounded,
 * owner-private regular files read into one immutable hashed buffer. */
int allocator_data_read(const char *path,int outcomes,AllocatorData *out);
int allocator_data_disjoint(const AllocatorData *training,const AllocatorData *evaluation);
#endif
