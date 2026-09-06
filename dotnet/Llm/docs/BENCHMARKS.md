# Managed benchmark procedure

The original March CPU tables are historical evidence, retained in the
[archive](../../../docs/MAINTENANCE.md). They are not fresh results for this
source or the native AMD training experiment.

Inspect [benchmark](../benchmarks/) fixture setup before execution.
InferenceBenchmarks can download missing models; set
`CNET_LLM_BENCH_MODEL_PATH` to an existing local GGUF to avoid that fallback.
Package restore can still contact the network.

Record model/tokenizer hashes, quantization, context, prompt/output length,
threads/affinity, warmup, cache lifetime and independent repetitions.
Separate load, prefill, decode and end-to-end latency. Compare exact
logits/tokens alongside timing.

Warm-prefix gains are not cold-request gains; a fake-model unit test is not
real-model throughput. Read [SAMPLING.md](SAMPLING.md),
[WARMUP.md](WARMUP.md) and [KV_CACHE.md](KV_CACHE.md) for controls.
