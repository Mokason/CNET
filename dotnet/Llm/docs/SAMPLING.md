# Sampling and stopping

[SamplerPipeline](../src/CNET.Llm.Engine/Samplers/SamplerPipeline.cs) applies
configured logit processors, then sampler steps, then token selection.
Its automatic path includes repetition penalty and enabled temperature,
top-k, top-p and min-p transforms. Nonpositive temperature selects greedy
behavior on that automatic path.

Explicit step lists are a separate constructor/options path.
Preserve ordering, RNG/seed and transform settings when comparing output.
Constraint masks and stopping are generation-level concerns too.

The current server converter does not implement the older guide's
frequency/presence penalties, logit-bias or `n > 1` beam-search promise.
It returns one choice. Built-in stop-token sequences are not established by
the stop-string API.

Stop strings are checked at token boundaries and can remove a whole final
token, including text preceding the matched suffix.
Max-token/context limits can leave structured output incomplete.

See [RequestConverter](../src/CNET.Llm.Server/RequestConverter.cs),
[stop conditions](../src/CNET.Llm.Engine/Samplers/StopConditions/)
and [speculation eligibility](SPECULATIVE.md).
