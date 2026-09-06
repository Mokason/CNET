# Bounded speculative decoding

[SpeculativeDecoder](../src/CNET.Llm.Engine/SpeculativeDecoder.cs) implements
draft/verify/accept with greedy acceptance only. Its constructor rejects
nongreedy mode because full transformed-distribution acceptance is not
implemented correctly.

[TextGenerator](../src/CNET.Llm.Engine/TextGenerator.cs) additionally checks
eligibility: temperature must be nonpositive, repetition penalty must be one,
and requesting logprobs disables speculation. Check all selected sampler
options rather than assuming a seeded request is greedy.

Draft/target tokenization and cache rollback must agree. Accepted-token
equality also depends on target batched-vs-sequential numerical parity;
an algorithmic argument cannot erase a measured kernel discrepancy.

Report draft cost, accepted fraction, verification/rollback cost and total
latency. Low acceptance can be slower than ordinary decode; no universal
2–3× gain is claimed. Historical regressions and measurements remain in the
[archive](../../../docs/MAINTENANCE.md).

[PromptLookupDecoder](../src/CNET.Llm.Engine/PromptLookupDecoder.cs) is
a separate draft source/path with its own tests, not unrestricted
probabilistic speculation.
