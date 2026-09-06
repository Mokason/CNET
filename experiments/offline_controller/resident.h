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
int resident_snapshot(Resident *r,Net *out);
const char *resident_error(const Resident *r);
#ifdef CONTROLLER_TESTING
int resident_test_device_failure(Resident *r);
#endif
#ifdef __cplusplus
}
#endif
#endif
