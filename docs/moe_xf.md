# Transformer-MoE block trainer (`make moe_xf`)

The scale-up path from `cce_moe_train`: a real **transformer block** — single-head
causal attention + a sparse **Mixture-of-Experts FFN** + a next-token head —
trained end to end with explicit, hand-written gradients, with every heavy matmul
dispatched to the **GPU** (dual R9700 / gfx1201 via `cce_clgemm`) or a CPU
reference.

Files: `include/cce/cce_moe_xf.h`, `src/cce/cce_moe_xf.c`, `tests/moe_xf.c`.

## The model
```
x  = embed(tokens) + pos                       (learned token + positional emb)
x1 = x  + Attention(x)                          (single-head, causal, scaled dot-product)
x2 = x1 + MoE_FFN(x1)                            (softmax router, top-k experts, residual)
logits = x2 @ Wh + bh ;  loss = CE(next) + lb_coef*aux
```
- **Attention**: Q,K,V = x·Wq/Wk/Wv; scores = QKᵀ/√d with a causal mask; softmax
  over keys; context = Σ a·V; output = context·Wo, added as a residual.
- **Learned positional embedding** (`pos`, one vector per position). Without it the
  attention is purely content-based and cannot address a token *by position* — it
  can solve a content-matching copy task (find where the current token last
  appeared) but is blind to relations like "the previous token". `pos` makes it a
  proper transformer.
- **MoE FFN**: a softmax router picks the top-k of `n_expert` experts per token;
  each expert is `relu(x·W1)·W2`; the gated sum is added as a residual. A
  Switch-style load-balancing aux loss keeps every expert live.
- **Optimizer**: Adam with decoupled weight decay (AdamW); embeddings, biases, and
  the positional table are excluded from decay.
- Everything backprops by hand (attention softmax, router softmax, gated expert
  sum, CE head) — no autograd dependency.

## GPU dispatch (single point)
Every heavy matmul — QKV/O projections, expert up/down, the head, and all of
their backward GEMMs (`dX = dY·Wᵀ`, `dW += Xᵀ·dY`) — goes through one dispatch
helper that runs on the CPU triple-loop (reference) or `cce_clgemm` on the GPU
above a size threshold. `cce_moe_xf_use_gpu(m, 1)` returns 1 when the GPU is
active. The GPU path is validated to be **bit-for-bit equivalent** to the CPU
forward (`rel 0.0`), and its **backward** is checked independently (below).

## Correctness: gradient-checked on both devices
A **directional finite-difference check** — the numeric derivative of the loss
along the analytic gradient equals `|grad|` — aggregates over all parameters, so
it is robust to the relu kinks and float noise that make a naive per-weight check
meaningless. It runs on whichever device is active, so it validates the **GPU
backward** against the GPU forward, not just the CPU path:
- CPU gradient check: `~1.4e-4`
- GPU forward == CPU forward: `rel 0.0` (exact)
- GPU gradient check: `~4e-4`

## Two tasks (`MOE_XF_TASK`)
### `copy3` (default) — the CI smoke test
Each sequence is a random period-3 pattern (`tok[i] = tok[i-3]`); predicting the
next token requires attending three positions back and copying. Deterministic
(entropy floor 0), converges fast: **CE 5.55 → ~0.03** in ~200 steps. This is the
quick correctness gate.

### `markov2` — the real language-modeling run
An **order-2 Markov source** with a *layered* conditional distribution:
```
logit(next | prev2, prev1) = A[prev1][next] + B[prev2][next]
```
- `A` (dominant, indexed by the **current** token) is an order-1 signal the FFN and
  head learn almost immediately.
- `B` (weaker, indexed by the **previous** token) is an order-2 correction the
  model can only capture by **attending one position back**.

So CE descends in two measurable stages, and an oracle computes both floors
exactly:
```
uniform  ->  H1 (order-1 plateau, current token only)  ->  H2 (floor, needs 1-back attention)
 4.159         2.836                                          2.402
```
**No memorization is possible**: every sequence is freshly sampled from the
source, so the model trains on an effectively infinite stream and never sees a
sequence twice. CE dropping below `H1` is therefore *genuine order-2 learning*
(the attention learned to use the previous token), not overfitting — the clean
generalization signal the tiny discrete task in `moe_train` could not provide.

Observed: 4.16 → ~2.84 (reaches H1) in ~120 steps, then the slower order-2 descent
toward H2 as the attention develops a 1-back pattern from the positional
embeddings.

## Throughput
~500 tok/s steady on the dual R9700 at `d=192, seq=96, 8 experts, d_ff=512`. This
is dominated by per-matmul weight **re-upload** overhead: the block issues ~20
small GEMMs per sequence forward (plus backward), each re-uploading its weights
ephemerally (required for training, where weights change every step). The GEMMs
are small (T=96), so launch + upload overhead dominates the math. The clear next
lever is **batching tokens across sequences** so the projection GEMMs become large
(BS·T rows), plus keeping weights resident within a step.

## Run it
```
make moe_xf                      # builds + copy3 smoke (200 steps, ~5 min on GPU)
./bin/moe_xf <steps> <lr>        # e.g. ./bin/moe_xf 3000 0.005
MOE_XF_TASK=markov2 ./bin/moe_xf 10000 0.005    # the language-modeling run
MOE_XF_CPU=1 ./bin/moe_xf 200                   # force the CPU reference path
```

## Honest limits (the scale-up path from here)
- **Single head, single block.** One attention head and one transformer layer.
  Multi-head and multi-layer are the next architectural steps.
- **Dense expert loop.** All experts are evaluated for every token and then masked
  by the top-k gate — correct and simple, but it does the full dense compute. Real
  MoE speedup needs gather/scatter **dispatch** (route tokens to only their
  experts), which is also the door to **expert parallelism** across the two GPUs.
- **Overhead-bound throughput.** See above — batching + weight residency is the
  main throughput win still on the table.
- **No LR schedule.** Fixed learning rate; a warmup/decay schedule would sharpen
  the order-2 tail on `markov2`.
- Raw-float host storage, small dims — a genuine GPU training *block* to build on,
  not yet a scaled trainer.
