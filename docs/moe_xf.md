# Transformer-MoE trainer

The [API](../include/cce/cce_moe_xf.h) implements a single-head causal
attention block, positional/token embeddings, sparse expert FFN and next-token
head with explicit backward computations.

Heavy matrix operations use a shared CPU/OpenCL dispatch boundary.
`cce_moe_xf_use_gpu` reports whether the GPU path was activated.
Calling a GPU-capable executable is not proof that the measured work ran
on a device.

```sh
make moe_xf
```

The default `copy3` fixture predicts a repeated period-three sequence.
The `markov2` task uses a synthetic conditional source with a nonzero entropy
floor. Name the task, source entropy, train/held-out split, device and
finite-difference/forward comparisons in any benchmark report.

Top-k selection is discrete. Directional gradient checks and training-fit
checks cover a bounded configuration, not every shape or natural-language
task. Historical CPU/GPU tables remain in the [archive](MAINTENANCE.md);
fresh GPU claims require an actual device verdict, not a skip.

This trainer differs from the resident FP32/rocBLAS candidate worker and from
the generic CCE learner. Follow [GPU_TRAINING.md](GPU_TRAINING.md) to choose
the correct workload and its unchanged precision gate.
