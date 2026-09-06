# AOT experiment

The CLI has an optional Native AOT publishing path. Normal managed builds use
JIT; AOT is a separate configuration that needs its own build, loader and model
tests. It is not the CNET.Cce.Llm bridge's compatibility guarantee.

From the repository root, a deliberate Linux publish experiment is:

```sh
dotnet publish dotnet/Llm/src/CNET.Llm.Cli -c Release -p:PublishAot=true -r linux-x64
```

This creates build artifacts and may restore toolchain/runtime packages.
Inspect project warnings and target-platform prerequisites; do not suppress
trim/AOT diagnostics to call the result supported.

The original single-run startup/throughput tables are retained in the
[archive](../../../docs/MAINTENANCE.md). Neither a fixed 50 ms startup nor
a universal JIT/AOT speed ratio is guaranteed. Compare cold launch, model load,
warmup and steady-state inference separately with identical model/output.
