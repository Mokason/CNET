# Asynchronous draft context

The managed pipeline prepares bounded context while a user types, using
parallel CPU history/association sources and serialized model prefill.
It shares one conversation's mutable inference session; these are not three
concurrent model contexts.

## Lifecycle

`UpdateDraftAsync` versions and debounces a draft, cancels its predecessor,
collects history and word connections, then publishes only a current bounded
snapshot. If supported by the chat client, it prefills the exact enriched
prompt and discards a one-token speculative result.

`SendAsync` cancels draft work, reuses only an exact completed snapshot or
collects sources immediately, then performs final generation. Only the raw
user/assistant turn is committed. Transient context is labeled user-role
reference data, not system instructions or permanent conversation history.

A concurrent second final send is rejected. Source failures can produce an
empty section; cancellation/staleness must not publish an old snapshot.
Disposal cancels and awaits owned work.

## Limits

Default budgets are 120 ms debounce, 2048 history characters, 1024 connection
characters, 3072 combined context characters and 16384 draft characters.
Conversation storage has its own turn/character bounds.
These are text/work limits, not an operating-system memory quota.

Native generation is synchronous once entered. Managed cancellation cannot
terminate it mid-call; speculative model work is therefore capped at one
output token, which still may require expensive prompt processing.
“Cancelled” must not be reported as immediate native preemption.

See [harness](cnet_dotnet_inference_harness.md) and
[managed bridge](../dotnet/Cce.Llm/README.md). Exact implementation and tests
remain under the managed projects; the historical benchmark and API
walkthrough are recoverable through [maintenance](MAINTENANCE.md).
