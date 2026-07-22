# Telemetry & Observability — CNET LLM

## Metrics — IInferenceMetrics

All metrics via `System.Diagnostics.Metrics` (native .NET OpenTelemetry). Zero-cost when no listener.

### Throughput

| Metric | Type | Description |
|--------|------|-------------|
| `cnet-llm.tokens_per_second.prefill` | Gauge | Tokens/sec during prompt processing |
| `cnet-llm.tokens_per_second.decode` | Gauge | Tokens/sec during generation |
| `cnet-llm.requests.completed` | Counter | Total completed requests |
| `cnet-llm.tokens.generated` | Counter | Total tokens generated |
| `cnet-llm.tokens.prompt` | Counter | Total prompt tokens processed |

### Latency (Histograms)

| Metric | Description |
|--------|-------------|
| `cnet-llm.latency.time_to_first_token` | Request receipt → first generated token (TTFT) |
| `cnet-llm.latency.inter_token` | Time between consecutive tokens (ITL) |
| `cnet-llm.latency.request_duration` | Total request time including queue wait |
| `cnet-llm.latency.prefill_duration` | Time spent in prefill phase |

### Resource Utilization

| Metric | Description |
|--------|-------------|
| `cnet-llm.kv_cache.utilization` | Fraction of KV blocks in use |
| `cnet-llm.kv_cache.blocks.allocated` | Currently allocated blocks |
| `cnet-llm.kv_cache.blocks.free` | Available blocks |
| `cnet-llm.gpu.memory.used_bytes` | GPU memory in use |
| `cnet-llm.gpu.memory.total_bytes` | Total GPU memory |
| `cnet-llm.batch.size` | Active sequences in batch |
| `cnet-llm.queue.depth` | Requests waiting for admission |

### Scheduler

| Metric | Description |
|--------|-------------|
| `cnet-llm.scheduler.preemptions` | Sequence preemption count |
| `cnet-llm.prefix_cache.hit_ratio` | Prefix cache hit rate |
| `cnet-llm.prefix_cache.entries` | Cached prefix count |

## Implementation

```csharp
// Define meter once
private static readonly Meter s_meter = new("CNET.Llm.Engine");

// Create instruments
private static readonly Counter<long> s_tokensGenerated =
    s_meter.CreateCounter<long>("cnet-llm.tokens.generated");

private static readonly Histogram<double> s_ttft =
    s_meter.CreateHistogram<double>("cnet-llm.latency.time_to_first_token",
        unit: "s", description: "Time to first token");

// Record (zero-cost if no listener)
s_tokensGenerated.Add(1);
s_ttft.Record(elapsed.TotalSeconds);
```

## Request Tracing — IRequestTracer

Per-request distributed tracing via `System.Diagnostics.Activity` (OpenTelemetry-compatible).

### Trace Spans

```
cnet-llm.request                    (root)
├── cnet-llm.queue_wait             Time in scheduler queue
├── cnet-llm.tokenize               Prompt tokenization
├── cnet-llm.template               Chat template application
├── cnet-llm.prefix_lookup          Prefix cache lookup
├── cnet-llm.prefill                KV-cache computation
│   └── cnet-llm.layer.{n}         Per-layer (optional, verbose)
├── cnet-llm.decode                 Token generation loop
│   └── cnet-llm.sample            Sampling + constraint eval
└── cnet-llm.detokenize             Token-to-text
```

### Span Attributes

Each span carries: token counts, model name, adapter ID, constraint type, GPU device ID, batch position.

### Implementation

```csharp
private static readonly ActivitySource s_source = new("CNET.Llm.Engine");

using var activity = s_source.StartActivity("cnet-llm.prefill");
activity?.SetTag("cnet-llm.prompt_tokens", tokenCount);
activity?.SetTag("cnet-llm.model", modelName);
// ... do prefill ...
```

Zero-cost when no `ActivityListener` registered.

## Integration

- **Prometheus**: `OpenTelemetry.Exporter.Prometheus` package → `/metrics` endpoint.
- **Grafana**: Standard dashboards for LLM serving metrics.
- **Jaeger/Zipkin**: Trace visualization via OpenTelemetry trace exporters.
- **ASP.NET**: Automatically correlates HTTP request traces with inference spans.
