# Asynchronous KV recall

The [STM/LTM API](../include/cce/cce_stm_ltm_bridge.h) connects a streaming
attention index to an optional cold-page worker and an optional local skill
probe. This is a KV adapter, not the [capsule memory runtime](MEM_RUNTIME.md).

HOT append updates the active index. A recall already in STM returns
immediately; a cold recall queues work and later completes through polling.
The queue and pin set each have 64 slots. A full queue returns
`CCE_RECALL_BUSY`; callers must apply backpressure, not wait indefinitely
on the decode path. Explicit sync is a test/drain operation.

```sh
make stm_ltm_bridge stm_ltm_bench
```

Without a pager, the benchmark can simulate cold delay. State that distinction
when reporting latency. A queued/rehydrated KV row is not a certified fact;
the optional local skill callback has its own trust boundary.
Consult [weight epochs](WEIGHT_EPOCH.md) before reusing recalled neural state
after any host-model weight change.
