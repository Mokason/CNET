#ifndef CNET_EMBER_STEER_H
#define CNET_EMBER_STEER_H

/* Ember directional steering for Teacher residual style only.
 *
 * DS4 applies f32 layer directions in the residual stream. Ember cannot
 * rewrite CORE activations. Instead a steer file supplies a *style card*
 * (and optional scale) injected into Teacher draft prompts only.
 *
 * File formats accepted:
 *   1) Plain text style card (first non-# lines, max 1.5k)
 *   2) JSON-ish: {"style":"...","scale":1.0} (best-effort parse)
 *   3) Binary f32 dump: ignored for math; presence enables default succinct card
 *
 * Law: never touches CERT RESULT. claimed_cert stays 0.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_EMBER_STEER_CARD 1536

typedef struct {
    char path[512];
    char card[CNET_EMBER_STEER_CARD];
    float scale; /* >0 amplify card weight in prompt prefix */
    int loaded;
} CnetEmberSteer;

void cnet_ember_steer_init(CnetEmberSteer *st);
int cnet_ember_steer_load(CnetEmberSteer *st, const char *path);
void cnet_ember_steer_clear(CnetEmberSteer *st);

/* Prefix user turn with style card. dst must differ from turn. */
int cnet_ember_steer_apply(const CnetEmberSteer *st, const char *turn,
                           char *dst, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CNET_EMBER_STEER_H */
