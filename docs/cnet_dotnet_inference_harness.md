# Native/.NET inference harness

The optional `libcnet_harness.so` plugin connects managed callers to a
selected llama.cpp build through [cnet_harness.h](../include/cnet_harness.h).
Core `cnet.so` remains a separate library; having it does not make the
harness plugin available.

```sh
make cnet_harness_plugin
dotnet build dotnet/Cce/Cce.csproj -c Release
```

Inspect the Make variables selecting llama.cpp source/build before compiling.
A real model is not needed for managed fake-backend contract tests; real
generation needs a local compatible GGUF and its dependencies.

## ABI and ownership

Public structures carry ABI version and size. Invalid layouts, resource masks,
budgets or arguments refuse before publication. Session open uses CNET's
model descriptor, residency manager and leases; failure releases partial state.

Managed `CnetHarnessSession` serializes generation, probes and disposal.
Native callers must provide equivalent serialization for a shared session.
Separate sessions can execute independently, while backend-global lifetime is
reference counted.

Generation is synchronous and non-streaming at this native boundary. Managed
cancellation before entry can stop queued work; it cannot interrupt a
synchronous native call already executing. The
[async context pipeline](cnet_async_context_pipeline.md) uses bounded
one-token speculation and discards stale results.

## Backends and resources

Legacy CPU open uses zero GPU layers. Legacy GPU open can request full
offload; it is not the bounded-layer policy. To limit AMD residency, use the
additive `open_with_offload` interface described in
[bounded GPU offload](cnet_bounded_gpu_offload.md).
A default-CPU config is the safest starting point for local contract work.

The managed-only `CNET.Cce.Llm` backend is a different implementation: it
rejects GPU masks and offload policies. See its
[guide](../dotnet/Cce.Llm/README.md) before choosing a backend.

## Output and trust

The harness uses the selected model's chat template and exposes sampling/route
metadata. AICIMO selects adapter/policy information; it does not generate
tokens or increase context capacity. Model output and route uncertainty are
not capsule certification.

Use `CNET_HARNESS_LIBRARY` to select the intended native plugin where the
managed resolver supports it. Verify model identity, loader paths and
fixture scope; do not copy machine-specific absolute library paths from
historical reports.

The original detailed ABI walkthrough and benchmark history remain in the
[archive](MAINTENANCE.md). Current declarations, managed implementations and
focused tests are the reference for exact fields and error codes.
