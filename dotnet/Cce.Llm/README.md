# CNET.Cce.Llm — managed inference backend for CCE

Lets `CNET.Cce` run inference through the managed `CNET.Llm` engine instead of
the native `cnet.so` harness, behind one shared interface.

## The seam

`CNET.Cce` defines `ICnetInferenceSession` (in `CnetHarness/ICnetInferenceSession.cs`):

```csharp
public interface ICnetInferenceSession : IDisposable
{
    CnetHarnessGenerationResult Generate(CnetHarnessGenerateOptions options);
    CnetHarnessRouteInfo ProbeRoute(string role, CnetHarnessSamplingMode overrideMode = Auto);
}
```

Two implementations:

| backend | assembly | opens with | needs |
| --- | --- | --- | --- |
| native (default, unchanged) | `CNET.Cce` | `CnetHarnessSession.Open(config)` | `cnet.so` + llama context |
| managed | `CNET.Cce.Llm` | `CnetLlmInferenceSession.Open(config)` | nothing but the GGUF file |

Hosts hold the interface and pick a backend at the open call:

```csharp
ICnetInferenceSession session = useManaged
    ? CnetLlmInferenceSession.Open(config)
    : CnetHarnessSession.Open(config);

var result = session.Generate(new CnetHarnessGenerateOptions
{
    System = "You are terse.",
    User   = "The capital of France is",
    Role   = "probe",
    MaxTokens = 64,
    Sampling  = CnetHarnessSamplingMode.Deterministic,
});
```

Nothing downstream of `Generate` changes: same options type, same result record,
same `CnetHarnessException` status codes.

## Why this is a separate assembly

Two reasons, both load-bearing — do not "simplify" this by adding a
`CNET.Llm` reference directly to `Cce.csproj`:

1. **License containment.** `CNET.Cce` is Apache-2.0; `CNET.Llm` is GPL-3.0-only
   (vendored from [dotLLM](https://github.com/kkokosa/dotLLM)). A direct
   reference would put every CCE consumer under GPL-3.0 on distribution. With
   the bridge separate, only hosts that actually reference `CNET.Cce.Llm` take on
   that obligation. `CNET.Cce` still references nothing from `CNET.Llm`.
2. **AOT.** `Cce.csproj` sets `IsAotCompatible=true`. The LLM engine is not part
   of that guarantee, so this project does not claim it.

The only concession is an `InternalsVisibleTo("CNET.Cce.Llm")` in `Cce.csproj`,
which lets the bridge call `CnetHarnessSession`'s config/option validators. That
is visibility only — no reference in the other direction — and it exists so the
two backends cannot drift apart on what input they accept.

## What the managed backend does not do

Stated plainly, because the result records have fields it cannot fill honestly:

- **No AICIMO routing.** The native backend selects an adapter and reports route
  uncertainty. There is no AICIMO here: `SelectedAdapter` is always `0`,
  `RouteUncertainty` always `0.0`, and `AicimoOverride` always `false`. Those are
  "not measured", not "measured as zero".
- **`Auto` sampling has nothing to defer to.** On the native backend `Auto` hands
  the decision to AICIMO. Here it resolves to `Balanced`, and
  `EffectiveSampling` reports `Balanced` — so callers see what was applied
  rather than what they asked for. Full table in `CnetLlmSamplingProfile`.
- **CPU only.** `Open` *rejects* a GPU `ResourceMask` or any `GpuOffload` with
  `InvalidArgument` rather than silently running on CPU. Use the native session
  for GPU work. (`CNET.Llm.Cuda` exists but is NVIDIA-only and is not wired here.)
- **`Role` does not influence generation.** It is validated for parity with the
  native backend, and ignored.

Prompt construction uses the GGUF's own chat template when the model carries one
(`HasChatTemplate`); otherwise `System` and `User` are concatenated, which is the
honest fallback for a base model with no turn structure.

## Tests

`dotnet test dotnet/Cce.Llm.Tests` — 19 tests. The generation tests need a local
GGUF fixture and skip cleanly without one; they reuse the model the `CNET.Llm`
integration suite downloads to `~/.cnet-llm/test-cache/` rather than fetching
another copy.
