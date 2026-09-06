# Warmup and measurement

[WarmupRunner](../src/CNET.Llm.Engine/WarmupRunner.cs) executes configured
dummy-prompt generation iterations with greedy sampling.
[WarmupOptions](../src/CNET.Llm.Engine/WarmupOptions.cs) controls enablement,
iteration count and generation length.

Warmup exercises loading/JIT/cache effects; a fixed three iterations does
not guarantee steady state or a particular JIT tier. Measure repeated
latency and state the stopping criterion instead of excluding convenient
slow observations.

Warmup can populate prompt state, so distinguish a warm-prefix benchmark
from a fresh request. It does not certify model quality.

The sample server's model replacement disposes the incumbent before loading
the replacement. A failed load does not preserve the previous model;
do not equate it with the guarded CNET capsule/core lifecycle.

Original platform-bound warmup observations remain in the
[archive](../../../docs/MAINTENANCE.md). See [BENCHMARKS.md](BENCHMARKS.md).
