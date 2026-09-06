# Telemetry contracts

Core [IInferenceMetrics](../src/CNET.Llm.Core/Telemetry/IInferenceMetrics.cs)
and [IRequestTracer](../src/CNET.Llm.Core/Telemetry/IRequestTracer.cs)
define extension points. The telemetry package contains
[Placeholder.cs](../src/CNET.Llm.Telemetry/Placeholder.cs), not a working
metrics/tracing exporter.

The old metric names, span layouts and instrumentation examples are proposed
design retained in the [archive](../../../docs/MAINTENANCE.md).
Do not promise that those events are emitted by the server today.

Actual timing/logprobs fields must be checked in their generation/handler
implementation. Future instrumentation should bound cardinality, protect
prompt/token data and test failure behavior. See [DIAGNOSTICS.md](DIAGNOSTICS.md)
and [BENCHMARKS.md](BENCHMARKS.md).
