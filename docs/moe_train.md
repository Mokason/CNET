# MoE LM training in CNET (`make moe_train`)

A small but genuine sparse **Mixture-of-Experts language model**, trained end to
end with explicit, hand-written gradients — the sparse FFN sublayer that gives
MoE transformers their capacity, plus a router, top-k gating, a load-balancing
loss, and a next-token head.

Files: `include/cce/cce_moe_train.h`, `src/cce/cce_moe_train.c`, `tests/moe_train.c`.

## The model
```
ctx tokens --embed--> h = concat(context embeddings)        [ctx*d_model]
router:   g = softmax(h @ Wr)                               [n_expert]
pick the top-k experts;  expert_e(h) = relu(h @ W1_e) @ W2_e
z = h + sum_{e in top-k} g_e * expert_e(h)                  (residual MoE)
logits = z @ Wh + bh ;  loss = CE(logits, next) + lb_coef * aux
```
- **Router + top-k gating**: a softmax over experts; the top-k fire per token,
  their gates weight the expert outputs. Top-k selection is non-differentiable by
  design; gradient flows through the gate values and the fired experts (standard).
- **Load-balancing aux loss** (Switch-Transformer style): `lb_coef · E · Σ_e
  importance_e · load_e`, which keeps the router from collapsing onto a few
  experts. `balance = max_e load_e / mean` (1.0 = perfect).
- **Optimizer**: Adam with decoupled weight decay (AdamW), per-parameter moments.
- Everything — the router softmax, the gated expert sum, the FFNs, the CE head,
  and the aux loss — backprops by hand (no autograd dependency; CNET's autograd
  engine lacks the per-row softmax / broadcasting the router needs).

## Correctness: gradient-checked
The hand-written backward is verified by a **directional finite-difference check**
— the numeric derivative of the loss along the analytic gradient equals `|grad|`
(`max rel err ~2e-4`). This aggregates over all parameters, so it is robust to the
relu kinks and float-precision noise that make a naive per-weight check
meaningless. Run with `top_k = n_expert` (fully differentiable — no discrete
selection) so the check exercises the whole graph.

## What the smoke test validates (`MOE_TRAIN_PASS`)
On a synthetic topic task (K topics, each a distinct "select a context position
and shift it" rule; the router must infer the topic from the token block):

- **gradient check passes** — the backward is correct;
- **training reduces cross-entropy** to ~0 — the model fits the task;
- **routing stays balanced** — the load-balance loss keeps every expert live
  (no dead experts);
- **experts specialize by topic** — each topic concentrates on a dominant expert
  (mean share ~0.5, well above the ~0.17 of random top-1 routing over 6 experts).

These are the config-independent facts that show the MoE training machinery works.

## Honest limits
- **Held-out generalization is not gated.** On a task this small both the MoE and
  a matched-compute dense (1-expert) baseline **memorize** the training examples
  (train CE → 0) and overfit held-out combinations. Which of the two generalizes
  better on held-out flips with configuration (it is a memorization artifact, not
  a real capacity signal), so claiming "MoE beats dense" here would be dishonest.
  Demonstrating MoE's capacity/generalization advantage cleanly needs a larger,
  properly-regularized setup — beyond a smoke test.
- **No attention.** The context is a concatenation of embeddings, a stand-in for
  attention output — so this is the MoE FFN mechanism itself, not a full
  transformer. The block is exactly the sparse sublayer that drops into one.
- Raw-float host storage, CPU, tiny dims — a training *setup* to build on, not a
  scaled trainer.
