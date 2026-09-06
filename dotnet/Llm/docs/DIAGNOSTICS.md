# Diagnostics and log probabilities

Core hook types and [ISparseAutoencoder](../src/CNET.Llm.Diagnostics/ISparseAutoencoder.cs)
are extension contracts, not implemented activation capture, logit-lens
analysis or SAE execution. The older tutorial's concrete capture-hook
examples must not be treated as runnable APIs.

Actual generation log-probability support is separate:
[LogprobsCapture](../src/CNET.Llm.Engine/Samplers/LogprobsCapture.cs) and
[RequestConverter](../src/CNET.Llm.Server/RequestConverter.cs) provide
the implemented boundary. Server `top_logprobs` defaults to 0 and is
clamped to 0–20; requesting logprobs disables speculation.

Diagnostic output can include prompt/model-derived private data.
Store it intentionally and bind numerical comparisons to the exact model,
tokenizer, sampler and cache path. A high cosine score does not by itself
prove equal selected tokens or equal generated text.

See [TELEMETRY.md](TELEMETRY.md) for unimplemented exporter scope and
[BENCHMARKS.md](BENCHMARKS.md) for measurement controls.
