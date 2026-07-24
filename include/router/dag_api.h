/* Public DAG boundary — composition surface used outside dag_full.c.
 * Implementation remains in src/router/dag_full.c; include this header for the
 * ownership seam rather than depending on dag_full internals.
 *
 * Layers:
 *   plan/build  — registry + goals → RoutePlan / circuit
 *   execute     — route_execute / route_execute_ex / dag paths
 *   lifecycle   — registry_record_fault after strict faults
 *
 * Incremental TU split of dag_full.c is tracked separately; call-site include
 * discipline is the first step.
 */
#ifndef CNET_DAG_API_H
#define CNET_DAG_API_H
#include "../router.h"
#ifdef __cplusplus
extern "C" {
#endif
#define CNET_DAG_API_VERSION 1
#ifdef __cplusplus
}
#endif
#endif
