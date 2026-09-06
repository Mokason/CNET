# Scheduling interface and current serialization

[IScheduler](../src/CNET.Llm.Engine/IScheduler.cs),
[SchedulerMetrics](../src/CNET.Llm.Engine/SchedulerMetrics.cs) and request
priority types are interface/design surfaces. There is no concrete
continuous-batching scheduler implementing that interface.

The sample [ServerState](../src/CNET.Llm.Server/ServerState.cs) serializes
requests with a semaphore. It does not implement priority admission,
preemption, KV swapping, multi-user fairness or guaranteed FIFO ordering.

The original state-machine proposal remains in the
[archive](../../../docs/MAINTENANCE.md). New scheduling work needs explicit
ownership/cancellation, bounded queues, overload refusal and latency/fairness
tests. It must not run concurrent mutation of one model's cache accidentally.
See [server boundary](SERVER.md) and [cache ownership](KV_CACHE.md).
