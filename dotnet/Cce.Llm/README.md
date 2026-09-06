# CNET.Cce.Llm bridge

This .NET 10 assembly implements
[ICnetInferenceSession](../Cce/CnetHarness/ICnetInferenceSession.cs) over
the managed engine, plus memory, verification and tool helpers.
Its [project](CNET.Cce.Llm.csproj) references CNET.Cce and the vendored
Engine/Models/Tokenizers/Cpu projects and carries GPL-3.0-only metadata.
It does not inherit CCE's Apache-2.0 or AOT guarantee.

## Build and tests

From the repository root:

```sh
dotnet build dotnet/Cce.Llm/CNET.Cce.Llm.csproj -c Release
dotnet test dotnet/Cce.Llm.Tests/CNET.Cce.Llm.Tests.csproj -c Release
```

Restore may use the network. Named local-model tests skip when fixtures are
missing; report those skips separately.

## Backend selection

| Backend | Boundary |
| --- | --- |
| [CnetLlmInferenceSession](CnetLlmInferenceSession.cs) | Managed CPU; rejects GPU masks and every offload policy |
| Native CnetHarnessSession | Requires separate `libcnet_harness.so`, not just `cnet.so` |
| [OllamaSession](Ollama/OllamaSession.cs) | HTTP provider; can reach remote/cloud models |

The managed backend uses static sampling profiles, not AICIMO routing;
selected-adapter and route-uncertainty compatibility fields are zero.
Effective context is the minimum of requested and model maximum length.

Build the native plugin with `make cnet_harness_plugin`;
`CNET_HARNESS_LIBRARY` can select it. See
[harness](../../docs/cnet_dotnet_inference_harness.md) and
[bounded AMD offload](../../docs/cnet_bounded_gpu_offload.md).

## Memory and tools

BlobStore, memory consolidation and tool registration write persistent state.
GhostChat experiments must choose an explicit private `--store`.
A loopback Ollama URL is not proof of offline execution.

Scriptlet “certification” replays supplied examples; it is not CNU1 sealing,
unseen-input coverage or capsule portability.
[ScriptletSandbox](Tools/ScriptletSandbox.cs) runs compiled code in-process.
On timeout it abandons a still-running background thread, not its computation.
Output-length checks occur after execution and are not memory quotas.
Use only with cooperative trusted users; it is not hostile-code isolation.

## Performance evidence

Local kernels, tokenization and caches have changed since the vendor rename.
Do not promise upstream-identical IL, throughput or model parity.
Compare exact model/tokenizer bytes, contexts, logits/tokens, threads and caches.

The [archive](../../docs/MAINTENANCE.md) preserves the original full guide,
including measured tables, rejected optimizations, cache/threading pitfalls,
speculation regressions and batched-versus-sequential logit discrepancies.
Those are historical results, not measurements from this cleanup.
