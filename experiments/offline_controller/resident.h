#ifndef CONTROLLER_RESIDENT_H
#define CONTROLLER_RESIDENT_H
#ifdef __cplusplus
extern "C" {
#endif
#include "net.h"
typedef struct Resident Resident;
/* One owner/caller per mutable context. Fixed bounded Net shape; no silent
 * fallback. Device failures poison the context until close/recreate. */
Resident *resident_open(const Net *initial,int discrete_device);
void resident_close(Resident *r);
int resident_predict(Resident *r,const float *x,int n,float *probabilities);
int resident_step(Resident *r,const float *x,const float *labels,int n,float lr,float *loss);
/* Blocking bounded transaction over 1..8 sequential minibatches. Features and
 * labels are packed [batches,n,width]; losses has batches entries. Validates all
 * host inputs before enqueueing. No outputs/snapshot usable after device failure. */
int resident_steps(Resident *r,const float *x,const float *labels,int batches,int n,float lr,float *losses);
int resident_snapshot(Resident *r,Net *out);
const char *resident_error(const Resident *r);
const char *resident_backend(const Resident *r);
#ifdef CONTROLLER_TESTING
int resident_test_device_failure(Resident *r);
int resident_test_fail_after_enqueue(Resident *r);
int resident_test_stream_idle(Resident *r);
#endif
#ifdef __cplusplus
}
#endif
#endif
