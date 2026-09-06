# Small MoE training mechanism

[cce_moe_train.h](../include/cce/cce_moe_train.h) and
[cce_moe_train.c](../src/cce/cce_moe_train.c) implement a small CPU
mixture-of-experts next-token task with explicit gradients.

Context embeddings are concatenated, a softmax router selects top-k experts,
their weighted FFN outputs join a residual path, and a head predicts the next
token. The objective includes cross-entropy and a load-balancing term.
This model does not have an attention layer.

```sh
make moe_train
```

The gate checks directional finite differences, training loss and routing
behavior on synthetic topics. The finite-difference check uses all experts
to avoid differentiating through a changed discrete top-k choice.

Low training loss is not held-out quality. Both the MoE and matched small
dense comparison can memorize this fixture; held-out superiority is not gated.
Report expert usage and the exact split, not only loss.
For the attention-bearing trainer, see [moe_xf.md](moe_xf.md); for the separate
resident AMD training experiment, see [GPU_TRAINING.md](GPU_TRAINING.md).
