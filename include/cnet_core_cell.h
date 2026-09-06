#ifndef CNET_CORE_CELL_H
#define CNET_CORE_CELL_H
#ifdef __cplusplus
extern "C" {
#endif
/* Version-1 shared local-update cell. This score is a proposal, never a capsule
 * certificate. Hidden rows are [input0,input1,input2,bias], then 8 output weights
 * and output bias. No native struct is a portable checkpoint format. */
#define CNET_CELL_INPUTS 3
#define CNET_CELL_HIDDEN 8
#define CNET_CELL_WEIGHTS 41
typedef struct { float weight[CNET_CELL_WEIGHTS]; } CnetCoreCell;
int cnet_core_cell_validate(const CnetCoreCell *cell);
/* Features must be finite in [0,1]. Output is cleared on refusal. Stateless:
 * one immutable cell may be used concurrently by independent requests. */
int cnet_core_cell_predict(const CnetCoreCell *cell,const float features[3],float *score);
#ifdef __cplusplus
}
#endif
#endif
