# CNET asynchronous context pipeline

## Purpose

Improve offline-model coherence and perceived latency while a person is typing,
without multiplying model residency or allowing stale background work to enter
the final prompt.

The pipeline has three logical lanes:

1. **History lane (CPU):** select relevant complete turns from bounded committed
   conversation history.
2. **Connection lane (CPU):** derive bounded lexical/co-occurrence associations
   for words in the current draft.
3. **Speculative lane (model):** after a debounce, prefill the exact enriched
   prompt and generate at most one lead token. This warms the existing native
   session's prompt state; the token is never shown or committed.

History and connection work run concurrently. Model work is serialized through
the existing `CnetHarnessConversation` and its single mutable native session.
There are never three simultaneous llama contexts.

## Non-goals

- Unbounded transcript or retrieval storage.
- Multiple concurrent decode calls against one native session.
- Treating speculative text as authoritative conversation history.
- Changing the existing C harness ABI v1.
- Pretending synchronous native decode can be interrupted after it has entered
  llama.cpp. The speculative call is therefore capped at one token.

## Public managed API

```csharp
public interface IAsyncContextSource
{
    ValueTask<string> CollectAsync(
        string draft, int maxCharacters, CancellationToken cancellationToken);
}

public sealed record AsyncContextSnapshot(
    long Version,
    string Draft,
    string HistoryContext,
    string ConnectionContext,
    string CombinedContext,
    bool PrefillCompleted);

public sealed class AsyncContextPipelineOptions
{
    public TimeSpan TypingDebounce { get; init; }
    public int MaxHistoryCharacters { get; init; }
    public int MaxConnectionCharacters { get; init; }
    public int MaxCombinedCharacters { get; init; }
}

public sealed class AsyncContextPipeline : IAsyncDisposable
{
    public AsyncContextSnapshot? Current { get; }
    public Task<AsyncContextSnapshot> UpdateDraftAsync(
        string draft, CancellationToken cancellationToken = default);
    public Task<string> SendAsync(
        string user, CancellationToken cancellationToken = default);
}
```

`UpdateDraftAsync` is called whenever the UI publishes a changed draft. It
cancels the previous version, debounces, runs the two CPU sources concurrently,
publishes a bounded immutable snapshot only if the version is still current,
and then requests speculative prefill.

`SendAsync` gives final work priority: it cancels the active draft pipeline. If
an exact completed snapshot exists it is reused; otherwise both CPU sources are
run immediately without debounce. Final generation receives the transient
context and commits only the raw user/assistant turn.

## Conversation integration

`CnetHarnessConversation` gains overloads that accept transient context and a
cancellation token. Transient context is represented as a labeled user-role
reference-data block, never promoted to system authority, counts against the
request character budget, and is never retained in the committed transcript.

A private/public companion interface allows an offline client to prefill:

```csharp
public interface IPrefillChatClient
{
    Task PrefillAsync(
        (string Role, string Content)[] messages,
        CancellationToken cancellationToken);
}
```

`CnetHarnessChatClient` implements this by issuing the same deterministic native
generation with `MaxTokens = 1`. It checks cancellation before native entry and
after return. Existing longest-common-prefix reuse makes the following final
request cheap when its prompt matches or extends the draft. The generated lead
token is discarded. Native generation is atomic once entered and therefore not
interruptible; managed cancellation discards its result after this bounded call
returns.

Clients without `IPrefillChatClient` simply skip speculative work.

## Versioning and cancellation

- Versions increase monotonically per submitted draft.
- Every asynchronous stage checks cancellation and current version before
  publishing or entering speculative work.
- A stale version completes as cancelled and cannot replace `Current`.
- Final send cancels speculative work before entering the serialized
  conversation lane.
- Disposal cancels all active work, awaits it, clears snapshots, and disposes
  only objects explicitly owned by the pipeline.
- Source exceptions other than cancellation are isolated to that source and
  produce an empty section; final generation remains available.

## Boundedness

Defaults:

- typing debounce: 120 ms;
- history result: 2,048 characters
- connection result: 1,024 characters
- combined transient context: 3,072 characters
- draft input: 16,384 characters
- retained conversation history: existing `CnetHarnessConversation` limits
- committed conversation: existing turn and character limits.

All strings are clipped before publication. No unbounded channel is used: only
one current version, at most one draft task, at most one final task, their
cancellation sources, and one immutable snapshot are retained. A concurrent
second final send is rejected.

## Built-in CPU sources

### Relevant history

Tokenize the draft and complete committed turns case-insensitively. Rank turns
by overlapping distinct terms, break ties by recency, and emit complete turns
until the source character budget is exhausted. If no terms overlap, emit the
most recent complete turn only.

### Word connections

Build a transient co-occurrence table from committed user/assistant turns.
For each meaningful draft term, count nearby terms within a small fixed window,
rank by count then ordinal text, and emit the strongest bounded associations.
The graph is rebuilt from bounded history, so it has a strict memory ceiling.

## Correctness and performance acceptance

Managed deterministic tests must prove:

1. history and connection sources are concurrently in flight;
2. a newer draft cancels an older version and stale results never publish;
3. all source and combined character budgets are exact;
4. source failure is isolated;
5. only the newest debounced draft reaches speculative prefill;
6. final send cancels a cancellable prefill and runs next;
7. transient context is visible to the model but absent from committed history;
8. pipeline disposal cancels and awaits draft and final work and is idempotent;
9. a concurrent second final send is rejected and cannot commit a turn;
10. existing conversation and harness tests remain green.

The optional real-model gate must prove:

- controlled non-final draft sources are cancelled, so only the final typing
  update performs a one-token prefill on the existing production session;
- the identical final prompt returns 64-token deterministic output exactly
  equal to a sequential fresh full-prefill session;
- end-to-end warmed generation latency is reported against fresh generation;
- the production path opens no additional model session, and the comparison
  session is opened only after the production session is disposed;
- a repeated update/send soak stays within the existing memory-growth limits.

## Performance interpretation

This architecture improves latency by overlapping cheap retrieval with human
think time and warming the exact prompt prefix before send. It does **not** make
three CPU-bound decodes run in parallel. On one offline model, concurrent decode
would increase contention, mutable-state risk, and resident memory. One model
lane with work-conserving CPU enrichment is the coherent, memory-efficient
shape.

The executable smoke gate is deliberately bounded to a 512-token context. Its
timings compare generate-only warm-prefix reuse with full prefill while both
runs benefit from the same hot OS page cache. They establish correctness and
the value of skipping prefill; they are not a claim about cold model loading,
larger production context windows, or a universal throughput multiplier.
