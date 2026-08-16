#ifndef CONTRACT_ANTI_REPEAT_H
#define CONTRACT_ANTI_REPEAT_H

#include "../nn.h"
#include "contract.h"

/* Anti-repeat circuit logic as a first-class contract.
 *
 * This is not just for the LM generator — repeats/stuttering can happen
 * in narrative circuits, agent drafting, planner projections, etc.
 *
 * The contract defines a guard step:
 *   Inputs:  [prev_token (ONEHOT or EVIDENCE), candidate (EVIDENCE or logits)]
 *   Output:  next_token with repeat penalty applied / diversity enforced
 *
 * Can be wired into circuits via the planner (dag_plan_circuit) as a
 * reusable "anti_stutter" or "diversity_guard" primitive.
 *
 * The underlying BTN (if learned) or pure logic certifies that
 * consecutive identical tokens are suppressed.
 */

#define ANTI_REPEAT_CONTRACT_NAME "anti_repeat_guard"

/* Initialize the contract descriptor for anti-repeat.
 * Typically: 1-in (prev), 1-in (candidate distrib), 1-out (guarded next)
 */
int contract_init_anti_repeat(Contract *c,
                              const Port *prev_port,
                              const Port *candidate_port,
                              const Port *out_port);

/* Apply anti-repeat logic (contract-verified guard).
 *
 * Takes previous token index (or distrib), current candidate logits/distrib,
 * and returns an adjusted best index that discourages immediate repeats.
 *
 * This can be called from generators, and the step itself can be
 * registered as a primitive in the registry for circuit composition.
 *
 * penalty: how strongly to suppress repeat (0.0 = no effect, 0.1 = strong)
 * use_second: if true, fall back to 2nd best on repeat
 */
int apply_anti_repeat_guard(const Port *prev_port,
                            const double *prev_vec,
                            const Port *cand_port,
                            const double *cand_vec,
                            int vocab_size,
                            double penalty,
                            int use_second,
                            int *out_idx);

/* Optional: train a small BTN that learns to produce non-repeating next
 * given prev + candidate. The resulting BTN can be certified against
 * the anti_repeat contract.
 */
double train_anti_repeat_btn(BinaryTransformNetwork *btn,
                             const double *prev_inputs,
                             const double *cand_inputs,
                             const double *targets,
                             size_t samples,
                             size_t max_epochs);

#endif /* CONTRACT_ANTI_REPEAT_H */





