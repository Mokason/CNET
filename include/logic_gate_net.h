#ifndef LOGIC_GATE_NET_H
#define LOGIC_GATE_NET_H

#include <stddef.h>

/* ===========================================================================
 * Logic Gate Network: a learnable primitive that is BORN EXACT.
 *
 * After arXiv:2602.05830 "Learning Compact Boolean Networks" (and the DLGN
 * lineage, arXiv:2210.08277). A fixed random DAG of 2-input gates. During
 * TRAINING each gate is a softmax mixture over the 16 two-input boolean
 * functions, with inputs relaxed to [0,1] and gates to their multilinear
 * (degree-2) surrogates -- fully differentiable. After DISCRETIZATION each
 * gate collapses to its argmax function, yielding a PURE boolean circuit whose
 * inference is exact integer logic.
 *
 * Why this is the most CNET-native primitive: the discrete circuit is exact by
 * construction, so on its enumerable boolean input domain it is PROVABLE by
 * enumeration -- the coverage layer's "PROVEN" verdict, but with zero
 * canonicalization margin because boolean logic has no rounding. A frozen
 * primitive that is verifiable the moment it is learned.
 * ===========================================================================
 */

typedef struct {
    size_t input_count;
    size_t output_count;
    size_t total_gates;
    /* Wiring (fixed at init): each gate reads 2 values. Value index < input_count
       is an input bit; >= input_count is gate (index - input_count). */
    int    *in_a;        /* [total_gates] */
    int    *in_b;        /* [total_gates] */
    /* Trainable: per-gate logits over the 16 boolean functions. */
    double *logit;       /* [total_gates * 16] */
    /* Discrete choice (valid after lgn_discretize): gate function id in [0,16). */
    int    *gate;        /* [total_gates] */
    int     discretized;
} LogicGateNetwork;

/* Build a random fixed-wired network: `input_count` inputs, `num_hidden` hidden
   layers with widths `hidden_widths`, and a final layer of `output_count` gates.
   `seed` fixes both the wiring and the logit init. num_hidden >= 1, all widths
   >= 1. Returns 0, or -1 on bad args / allocation failure. */
int lgn_init(LogicGateNetwork *net, size_t input_count, size_t output_count,
             const size_t *hidden_widths, size_t num_hidden, unsigned seed);

void lgn_free(LogicGateNetwork *net);

/* Differentiable full-batch training on a truth table: `inputs`
   [rows*input_count] and `targets` [rows*output_count], all values in {0,1}.
   Full-batch SGD with momentum on the gate logits. Returns the final mean
   squared error (per output), or -1 on bad args. */
double lgn_train(LogicGateNetwork *net, const double *inputs,
                 const double *targets, size_t rows, size_t epochs, double lr);

/* Collapse each gate to its argmax function -> a pure boolean circuit. */
void lgn_discretize(LogicGateNetwork *net);

/* Exact discrete inference (requires discretized). `input` [input_count] and
   `out` [output_count] in {0,1}. Returns 0, or -1. */
int lgn_eval(const LogicGateNetwork *net, const double *input, double *out);

/* Greedy discrete refinement (requires discretized): coordinate descent over
   the gate choices, each gate set to the function minimizing the circuit's
   total bit-mismatch against `targets`, repeated up to `max_passes` or until a
   pass makes no change. Closes the soft/discrete gap by optimizing the EXACT
   objective directly -- only affordable because the domain is enumerable. */
void lgn_refine_discrete(LogicGateNetwork *net, const double *inputs,
                         const double *targets, size_t rows, size_t max_passes);

/* Exhaustive certification: the discrete circuit must reproduce `targets`
   EXACTLY on every row, and `rows` must equal 2^input_count (so the table IS
   the whole domain). Returns 0 (PROVEN) or -1; writes the mismatch count to
   `mismatches` if non-NULL. */
int lgn_certify_exhaustive(const LogicGateNetwork *net, const double *inputs,
                           const double *targets, size_t rows, size_t *mismatches);

/* Train + discretize, retrying with a fresh wiring seed until the discrete
   circuit is exact over the (exhaustive) truth table, up to `max_restarts`.
   On success `net` holds the proven circuit and 0 is returned; otherwise the
   last attempt is kept and -1 is returned. */
int lgn_learn_exact(LogicGateNetwork *net, size_t input_count, size_t output_count,
                    const size_t *hidden_widths, size_t num_hidden,
                    const double *inputs, const double *targets, size_t rows,
                    size_t epochs, double lr, size_t max_restarts, unsigned seed0);

#endif /* LOGIC_GATE_NET_H */
