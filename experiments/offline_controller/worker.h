#ifndef CNET_GPU_WORKER_H
#define CNET_GPU_WORKER_H
#include "cnet_core_cell.h"
#include <stdint.h>
#include <math.h>
enum {WORKER_TRAIN=1,WORKER_EVALUATE=2};
enum {WORKER_BATCH_TRAIN=3,WORKER_BATCH_EVALUATE=4};
#define WORKER_BATCH_MAGIC UINT32_C(0x43414231)
/* Private same-build IPC, not a portable checkpoint or knowledge package.
 * Caller supplies independently verified outcomes; this record grants no
 * approval. New allocator_worker reads it only after all sandbox seals. */
typedef struct {
    uint32_t magic,feature_version,rows,epochs;
    float lr;
    CnetCoreCell cell;
    float x[2048*3],y[2048];
} WorkerBatch;
static inline int worker_batch_validate(const WorkerBatch *b){
    if(!b||b->magic!=WORKER_BATCH_MAGIC||b->feature_version!=1||!b->rows||b->rows>2048||!b->epochs||b->epochs>4096||
       !isfinite(b->lr)||b->lr<=0||b->lr>1||cnet_core_cell_validate(&b->cell))return -1;
    for(unsigned i=0;i<b->rows*3;i++)if(!isfinite(b->x[i])||b->x[i]<0||b->x[i]>1)return -1;
    for(unsigned i=0;i<b->rows;i++)if(!isfinite(b->y[i])||b->y[i]<0||b->y[i]>1)return -1;
    return 0;
}
typedef struct {uint32_t magic,mode,device,rows;CnetCoreCell cell;float loss,max_error;uint32_t sandboxed;} WorkerResult;
typedef struct WorkerPool WorkerPool;
/* Private same-build native entry contract: FD5 is the pool-captured spawning
 * thread pidfd; deadline is an absolute CLOCK_BOOTTIME millisecond value.
 * Call before input/GPU use and exit immediately on failure. Fixed trusted
 * entry/runtime only: this is not an arbitrary executable/GPU sandbox. */
int worker_guard_enter(const char *deadline_text);
/* One trusted owner thread. Max two jobs, one per discrete device ordinal0/1.
 * Worker path is a trusted absolute compiled executable. No shell, inherited
 * environment, external scheduling changes or automatic promotion. Timeout
 * 1..60000ms includes preparation, spawn and initialization, including suspend.
 * Native entry arms uncatchable parent-thread death and deadline signals,
 * including a pidfd check for death before arming. Requires Linux >=6.9;
 * unsupported controls refuse. No GPU initialization occurs before this guard.
 * The trusted dynamic loader runs before entry; kernel/driver cleanup after
 * SIGKILL is not a hard realtime guarantee. Child stdio is /dev/null.
 * Snapshot input is a sealed memfd copy.
 * Output is unpublished until exact result, EOF and clean child exit agree.
 * Caller must not reap pool-owned children or close pool concurrently. */
WorkerPool *worker_pool_open(const char *executable);
void worker_pool_close(WorkerPool *pool);
int worker_start(WorkerPool *pool,int mode,int device,const CnetCoreCell *snapshot,unsigned timeout_ms);
int worker_start_batch(WorkerPool *pool,int mode,int device,const WorkerBatch *batch,unsigned timeout_ms);
/* 0 running, 1 complete (out valid), -1 refused/failed. Terminal poll releases
 * the slot. Cancel kills/reaps only that pool-owned child, leaving GPUs alone. */
int worker_poll(WorkerPool *pool,int slot,WorkerResult *out);
int worker_cancel(WorkerPool *pool,int slot);
#ifdef CONTROLLER_TESTING
int worker_start_fault(WorkerPool *,int device,int fault,unsigned timeout_ms);
#endif
#endif
