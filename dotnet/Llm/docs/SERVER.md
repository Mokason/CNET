# Local sample server

This is a trusted-user development surface. The builder enables permissive
CORS and developer exception pages, but no authentication, TLS, API-key quotas
or rate limiting. Model/config/cache endpoints mutate shared state.

From the repository root, with an existing local model:

```sh
dotnet run --project dotnet/Llm/samples/CNET.Llm.Sample.Server -c Release -- \
  /absolute/local/model.gguf --port 8080
```

This starts a service and may restore packages. Inspect effective hosting
configuration and keep it loopback/private. Do not run it as a documentation
test. Omitting the model argument exits with an error.

[EndpointExtensions](../src/CNET.Llm.Server/EndpointExtensions.cs) registers
chat/completions, completions, model/tokenization, health/properties,
configuration, model-management and inspection handlers, plus optional UI.
It does not register `/v1/embeddings`.

[ServerState](../src/CNET.Llm.Server/ServerState.cs) serializes access with a
single-request semaphore. It does not promise FIFO fairness, continuous
batching, priority admission or preemption. Model replacement disposes the
incumbent before loading; failure does not preserve the old model.

OpenAI-compatible field names describe a subset, not full API equivalence.
Inspect handler/converter support. Parsed tool calls need separate host
authorization; see [TOOL_CALLING.md](TOOL_CALLING.md).
