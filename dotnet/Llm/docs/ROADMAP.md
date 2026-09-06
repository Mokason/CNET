# Remaining managed scope

Working components include GGUF loading, CPU kernels, tokenizers, generation,
sampling, constraints, caches and a bounded greedy speculative decoder.
Each supported model/quantization still needs its own parity evidence.

Do not treat these design/interface surfaces as shipped implementations:

- Continuous scheduling, priority/preemption or multi-user fairness.
- A complete adapter manager, hot LoRA loading or adapter batching.
- Embeddings endpoint or a SafeTensors model-loader path.
- Concrete telemetry exporters, activation capture, logit lens or SAE execution.
- Model-parallel collectives/send/receive.
- Probabilistic speculative acceptance or applied RoPE scaling extensions.

CNET's managed bridge is CPU-only. AMD work uses the
[native guides](../../../docs/GPU_TRAINING.md).
Original milestones and projected performance remain in the
[archive](../../../docs/MAINTENANCE.md), not today's completion ledger.
New work needs an explicit contract, RED test and unchanged quality floors.
