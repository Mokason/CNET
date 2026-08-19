#ifndef CNET_EMBER_H
#define CNET_EMBER_H

/* CNET Ember — residual heat, never CORE star.
 *
 * Owns residual multi-turn session craft borrowed from DS4-class engines:
 *   - disk session checkpoint index (prefix hash + reason + budget)
 *   - session_sync (extend vs rebuild)
 *   - compact (summary ≠ gold / auto_cert=false)
 *   - dir-steer style cards for Teacher drafts only
 *
 * Law: claimed_cert is always 0 on ember paths. Residual never auto-CERTs.
 * Backend engines (ds4-server, bonsai, MAX) are replaceable under Ember.
 *
 * Gate: make cnet_ember → CNET_EMBER_PASS
 */

#include "cnet_ember_ckpt.h"
#include "cnet_ember_session.h"
#include "cnet_ember_steer.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_EMBER_NAME "cnet-ember"
#define CNET_EMBER_CONTRACT "ember_residual_v1"

typedef struct {
    CnetEmberSession sess;
    CnetEmberCkptStore ckpt;
    CnetEmberSteer steer;
    int open;
} CnetEmber;

void cnet_ember_default_paths(char *ckpt_dir, size_t ckpt_cap,
                              char *steer_path, size_t steer_cap);

/* Open under dir (env CNET_EMBER_HOME or default under HERMES/CNET state). */
int cnet_ember_open(CnetEmber *e, const char *home_dir);
void cnet_ember_close(CnetEmber *e);

/* Residual draft turn: session_sync → optional compact → held ask with steer.
 * out always claimed_cert=0. Returns 0 if draft text written, 1 abstain, <0 err. */
int cnet_ember_draft(CnetEmber *e, const char *user_turn, char *out, size_t cap,
                     CnetEmberDraftReport *rep);

/* Append self-improve miss (auto_cert=false). */
int cnet_ember_note_miss(const char *reason, const char *q);

#ifdef __cplusplus
}
#endif

#endif /* CNET_EMBER_H */
