/* Chess-fetch generation + FIFO outbox — NOT next-token parrot CE.
 *
 * Law:
 *   place/CERT pieces once (chess)
 *   each step: FETCH bound unit for state → run neuron → FIFO emit
 *   no global re-sort over all units/vocab each step
 *
 * State modes:
 *   CNET_GEN_ADVANCE_MARKOV   — state := argmax(y)  (char Markov; can collapse)
 *   CNET_GEN_ADVANCE_POSITION — state := state+1    (board square; teacher path)
 *
 * Fail-closed: no bound unit / coverage miss → abstain (no invented token).
 */
#ifndef CNET_GENERATE_FIFO_H
#define CNET_GENERATE_FIFO_H

#include <stddef.h>

#include "cnet_export.h"
#include "hybrid_ai.h"
#include "nn.h"
#include "router.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_GEN_FIFO_DEFAULT 4096
#define CNET_GEN_MAX_UNITS 256
#define CNET_GEN_NAME_MAX 64

enum {
    CNET_GEN_ADVANCE_MARKOV = 0,
    CNET_GEN_ADVANCE_POSITION = 1
};

typedef struct {
    char *buf;
    size_t cap;
    size_t head;
    size_t tail;
    size_t count;
    size_t dropped;
} CnetGenFifo;

typedef struct {
    char name[CNET_GEN_NAME_MAX];
    int state_id;
    int active;
} CnetGenBinding;

typedef struct {
    CnetGenFifo fifo;
    CnetGenBinding bind[CNET_GEN_MAX_UNITS];
    int n_bind;
    int state_dim;  /* one-hot input size (chars or positions) */
    int alphabet;   /* one-hot output size = charset classes */
    int advance;    /* CNET_GEN_ADVANCE_* */
    int pos_limit;  /* position mode: abstain when state >= pos_limit */
    Port in_port;
    Port out_port;
    size_t n_steps;
    size_t n_emit;
    size_t n_fetch;
    size_t n_resort;
    size_t n_abstain;
    size_t n_forwards;
} CnetGenEngine;

CNET_API void cnet_gen_fifo_init(CnetGenFifo *f, size_t cap);
CNET_API void cnet_gen_fifo_free(CnetGenFifo *f);
CNET_API int cnet_gen_fifo_push(CnetGenFifo *f, char c);
CNET_API int cnet_gen_fifo_pop(CnetGenFifo *f, char *c_out);
CNET_API size_t cnet_gen_fifo_drain(CnetGenFifo *f, char *dst, size_t dst_cap);

/* alphabet = output classes; state_dim = input one-hot (default = alphabet). */
CNET_API void cnet_gen_engine_init(CnetGenEngine *e, int alphabet, size_t fifo_cap);
CNET_API void cnet_gen_engine_set_dims(CnetGenEngine *e, int state_dim, int alphabet);
CNET_API void cnet_gen_engine_set_advance(CnetGenEngine *e, int advance, int pos_limit);
CNET_API void cnet_gen_engine_free(CnetGenEngine *e);

CNET_API int cnet_gen_bind(CnetGenEngine *e, int state_id, const char *unit_name);

CNET_API int cnet_gen_step_chess(CnetGenEngine *e, const HybridAi *cov,
                                 const PrimitiveRegistry *reg, int *state_io,
                                 const char *charset);

CNET_API int cnet_gen_step_resort(CnetGenEngine *e, const HybridAi *cov,
                                  const PrimitiveRegistry *reg, int *state_io,
                                  const char *charset);

CNET_API int cnet_gen_run(CnetGenEngine *e, const HybridAi *cov,
                          const PrimitiveRegistry *reg, int start_state, int n_steps,
                          int mode_resort, const char *charset, char *out,
                          size_t out_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
