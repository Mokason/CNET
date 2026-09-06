#ifndef CNET_GPU_WORKER_H
#define CNET_GPU_WORKER_H
#include "cnet_core_cell.h"
#include <stdint.h>
enum {WORKER_TRAIN=1,WORKER_EVALUATE=2};
typedef struct {uint32_t magic,mode,device,rows;CnetCoreCell cell;float loss,max_error;uint32_t sandboxed;} WorkerResult;
typedef struct WorkerPool WorkerPool;
/* One trusted owner thread. Max two jobs, one per discrete device ordinal0/1.
 * Worker path is a trusted absolute compiled executable. No shell, inherited
 * environment, external scheduling changes or automatic promotion. Timeout
 * 1..60000ms includes initialization. Snapshot input is a sealed memfd copy.
 * Output is unpublished until exact result, EOF and clean child exit agree.
 * Caller must not reap pool-owned children or close pool concurrently. */
WorkerPool *worker_pool_open(const char *executable);
void worker_pool_close(WorkerPool *pool);
int worker_start(WorkerPool *pool,int mode,int device,const CnetCoreCell *snapshot,unsigned timeout_ms);
/* 0 running, 1 complete (out valid), -1 refused/failed. Terminal poll releases
 * the slot. Cancel kills/reaps only that pool-owned child, leaving GPUs alone. */
int worker_poll(WorkerPool *pool,int slot,WorkerResult *out);
int worker_cancel(WorkerPool *pool,int slot);
#ifdef CONTROLLER_TESTING
int worker_start_fault(WorkerPool *,int device,int fault,unsigned timeout_ms);
#endif
#endif
