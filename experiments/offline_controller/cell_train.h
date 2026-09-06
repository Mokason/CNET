#ifndef CNET_CELL_TRAIN_H
#define CNET_CELL_TRAIN_H
#include "cnet_core_cell.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct CellGpu CellGpu;
CellGpu *cell_gpu_open(const CnetCoreCell *initial,int discrete_device);
void cell_gpu_close(CellGpu *gpu);
/* One owner. Finite [0,1] samples/labels; n=1..2048, epochs=1..4096.
 * All host inputs validated before GPU submission; blocking private-stream fit.
 * Any GPU/numeric failure poisons this private candidate. No CPU fallback. */
int cell_gpu_fit(CellGpu *gpu,const float *x,const float *y,int n,int epochs,float lr,float *loss);
int cell_gpu_snapshot(CellGpu *gpu,CnetCoreCell *out);
/* Frozen forward evaluation, n=1..2048. Does not update any weight. */
int cell_gpu_predict(CellGpu *gpu,const float *x,int n,float *scores);
const char *cell_gpu_error(const CellGpu *gpu);
#ifdef CONTROLLER_TESTING
int cell_gpu_fail_for_test(CellGpu *gpu);
#endif
#ifdef __cplusplus
}
#endif
#endif
