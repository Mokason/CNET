#ifndef CNET_BRAIN_MIRROR_H
#define CNET_BRAIN_MIRROR_H

/* CORE hemisphere → CNET_Brain admit outbox.
 *
 * On a CORE claimed_cert bind, append one JSONL record under
 * CNET_BRAIN_MIRROR_DIR (default: $HERMES_HOME/brain_mirror or
 * ~/.hermes/brain_mirror). Brain (or a later admit tool) consumes these
 * as external teacher pairs / proposal seeds. Never writes residual drafts.
 *
 * Law: mirror is not self-CERT inside main CNET. Brain still verify-admits.
 */

#include "cnet_hemisphere.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_BRAIN_MIRROR_PATH 1024

/* Resolve outbox directory into buf. 0 ok. */
int cnet_brain_mirror_dir(char *buf, size_t cap);

/* Append one CORE event if r is CORE + bound + claimed_cert.
 * Returns 0 written, 1 skipped (not CORE cert), <0 IO/arg error. */
int cnet_brain_mirror_core(const CnetHemiResult *r);

/* Test helper: set override dir (NULL clears). */
void cnet_brain_mirror_set_dir(const char *dir);

#ifdef __cplusplus
}
#endif

#endif /* CNET_BRAIN_MIRROR_H */
