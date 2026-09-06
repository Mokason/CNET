# Managed sample server

The sample defaults to an authenticated, loopback-only API. Startup refuses
missing/invalid security settings before opening the model. It never binds an
external interface; URL/Kestrel endpoint configuration cannot override this.
This is a bounded single-owner sample, not an approved public deployment.

## Owner configuration

For protected operation, supply `CNET_SERVER_INFERENCE_KEY` through your process
secret manager and set `CNET_SERVER_TRUSTED_LOOPBACK_PROXY=1`. Keys must be
independently generated random secrets, 32–256 printable non-space ASCII
characters. There is no built-in/default key. An optional
`CNET_SERVER_ADMIN_KEY` must be different; omitting it disables administration.
Environment/process access remains within the owner's trust boundary. Do not
put keys in source, command-line arguments, URLs, browser storage or logs.

The proxy flag is an owner assertion, **not evidence that a proxy exists**.
Before exposure, the owner must approve and configure a local reverse proxy
that terminates HTTPS, connects over actual loopback, preserves the bearer
header without logging it, and applies its own connection/upload/rate limits.
Keep the backend inaccessible to untrusted local users. This slice does not
install a proxy, certificate, firewall rule or live service. It does not trust
`Forwarded` or `X-Forwarded-*` headers as identity or transport evidence.

For explicit private development only, `CNET_SERVER_LOOPBACK_DEVELOPMENT=1`
allows unauthenticated inference/read requests on a loopback socket with a
loopback Host header. It never grants administrator access. Do not expose this
mode through a proxy. Default sample startup is API-only.

With an existing owner-approved local model and one of the above configurations:

```sh
dotnet run --project dotnet/Llm/samples/CNET.Llm.Sample.Server -c Release -- \
  /absolute/local/model.gguf --port 8080
```

This starts a service and may restore packages. It is not a documentation test.
HTTP model-loading requests cannot select GPU execution; inherited host model
settings remain owner-controlled. This closure makes no GPU support or
performance claim.

## Authorization and errors

Send `Authorization: Bearer <secret>`. No cookie/query authentication is used.
The administrator credential also permits inference, but inference credentials
never permit administration.

| Surface | Required role |
| --- | --- |
| GET `/`, `/app.js`, `/app.css` when UI explicitly mapped | Public static bootstrap only |
| POST chat/completions, completions, tokenize, detokenize | Inference |
| GET health, ready, models, config | Inference |
| POST config, cache/clear, models/load | Administrator |
| GET props, models/available, models/inspect | Administrator |
| Unknown paths/methods | Administrator before routing |

Missing credentials return 401; insufficient role or disallowed origin returns
403. Invalid input returns 400, oversized body 413, exhausted rate/admission
429 with `Retry-After`, and a deadline before response headers returns 504.
Unexpected errors use generic responses, not exception details. Once an SSE
response has started, failure/cancellation aborts the stream without a success
`[DONE]` marker; clients must treat a missing marker as incomplete.

Cross-origin requests are denied by default. Set
`CNET_SERVER_ALLOWED_ORIGINS` to at most 16 comma-separated exact HTTPS origins
if required. No wildcards or credentialed CORS are enabled. Development also
permits its exact same origin and explicitly configured loopback HTTP origins.
Preflights allow GET/POST and Authorization/Content-Type only. CORS is not
authentication. Responses carry no-store, nosniff, frame denial and a local-only
content security policy.

## Resource contract

| Control | Default | Supported owner bounds |
| --- | --- | --- |
| `CNET_SERVER_MAX_BODY_BYTES` | 65,536 bytes | 1,024–1,048,576 |
| `CNET_SERVER_MAX_PROMPT_CHARACTERS` | 32,768 UTF-16 characters | 1–262,144 |
| `CNET_SERVER_MAX_OUTPUT_TOKENS` | 2,048 | 1–8,192 |
| `CNET_SERVER_REQUESTS_PER_MINUTE` | 120 per role | 1–10,000 |
| `CNET_SERVER_TIMEOUT_SECONDS` | 30 seconds | 1–300 via environment |
| Concurrent stateful HTTP requests | 1 | Fixed; no waiting queue; constant UI assets exempt |
| Connected sockets / header bytes / request line bytes | 32 / 8,192 / 4,096 | Fixed |
| Query length / JSON depth | 2,048 characters / 16 | Fixed |

Three fixed rate counters cover anonymous, inference and administrator roles;
there is no attacker-keyed dictionary. Fixed one-minute windows permit a burst
at a window boundary. Requests denied for invalid transport/origin are not
metered by these role counters; the owner proxy must cover network-level abuse.
Constant UI assets use the same bounded anonymous role counter, reject request
bodies, retain deadlines, and can load without the model permit. No API endpoint
inherits that exception. Health checks can receive 429 while another request is active. No FIFO fairness,
continuous batching, priority admission or preemption is promised.

Known-length and chunked bodies are capped before DTO binding. The deadline
starts before body reading, so stalled uploads do not hold admission forever.
JSON root keys must use the documented lower-case names; duplicate keys and
malformed structures are refused. Chat permits 1–64 messages, at most 16 tools
and 16 calls per message. Message content has a cumulative character cap;
tool metadata also remains within the total body cap. Stop sequences are at
most eight strings of 256 characters. Detokenization accepts at most 8,192
valid vocabulary IDs. Sampling values are bounded and `n` must be one.

Requested `max_tokens` above the owner limit is refused; omitted/default values
are clamped to it and then to model context. Arbitrary JSON schemas/grammars are
not accepted by this HTTP boundary until their compilers have work budgets;
`response_format` supports only `text` and `json_object`.

Cancellation is cooperative. Generation checks cancellation at token boundaries;
a running native kernel, prefill, tokenizer, template or model load cannot be
forcibly interrupted. Admission remains held until that work actually exits,
even after the client disconnects or the deadline expires. A canceled completed
model load is disposed rather than published. Model preparation failure or
cancellation retains the usable incumbent. Preparation and adoption share the
canonical request gate; the complete candidate (including its paged cache
factory) is adopted before old resources are retired. Partial loader failure
also releases already acquired model/GGUF resources.

If retirement fails after adoption, the new model remains active. The load
response status is `activated_with_retirement_error_restart_required`, and
administrator `/props` exposes `retirement_cleanup_failed: true`. Further model
load requests return 409 until owner restart, bounding failed retirement to one
swap. Cleanup emits a fixed warning without exception details. This is not the
native certified-capsule lifecycle or an OS-level resource sandbox.

## Verification and integration

Run the private, real-loopback HTTP fixtures without models or live endpoints:

```sh
dotnet test dotnet/Llm/tests/CNET.Llm.Tests.ServerSecurity/CNET.Llm.Tests.ServerSecurity.csproj -c Release
```

Custom hosts must use both `AddCnetLlm(state, security)` and
`MapCnetLlmEndpoints()`. The latter installs the boundary for mapped routes;
`ServerStartup.BuildApp` also enforces Kestrel transport limits. Custom host
middleware/listeners are owner code and require their own deployment review.

Direct callers migrate the old mutable-action swap callback to
`SwapModelAsync(Func<CancellationToken, Task<ServerState>>, token)`: return a
fresh, complete, exclusively owned state, without mutating the incumbent or
sharing its owned resources. The method takes ownership of a returned candidate;
the loader owns cleanup before return. Sampling defaults remain server-wide.
Execute/swap callbacks must not call synchronous `Dispose` on their owning
state. Disposal waits for the canonical gate, is idempotent and refuses new
execute/swap calls. Direct access to public mutable fields outside that gate
does not acquire a model lifetime lease.

The API does not provide `/v1/embeddings` or full protocol equivalence. Parsed
tool calls do not authorize execution; see [TOOL_CALLING.md](TOOL_CALLING.md).

## Optional offline chat page

Add `--ui` to the sample invocation, or explicitly pass `serveUi: true` to
`BuildApp`, to serve the three bundled UI assets. No CDN, fonts, inline script,
browser storage or runtime frontend dependency is required. In protected mode,
configure the page's exact HTTPS origin in `CNET_SERVER_ALLOWED_ORIGINS` and use
the owner-approved TLS proxy described above. Static bootstrap is anonymous;
inference still requires its bearer credential.

Enter an inference key in the password field and select **Use inference key**.
The field is cleared immediately; the page keeps only an in-memory reference and
sends it only in a same-origin Authorization header. Redirects are refused;
cookies and credentialed fetches are disabled. **Forget key** clears access and
conversation, and a reload forgets both. Do not ask a browser/password manager
to save this key. The development-mode button merely requests anonymous access;
the server must independently permit explicit loopback development.

The page supports supervised text chat, stop, new conversation and accessible
error/loading/empty states. Every reply is rendered with `textContent`; markup,
links and tool-looking output remain plain text. It never executes tools or
makes administrator requests. The former browser model/config/inspection and
telemetry panels are replaced by this deliberately small chat surface; use the
separately authorized API for administration. Full old-UI feature parity is not
claimed.

Client limits are 8,192 characters per prompt, 60,000 serialized request bytes,
12 retained history messages/24,000 history characters, 24 visible messages,
and 65,536 characters per reply or pending SSE frame buffer. Server limits may
be stricter. A truncated/failed stream is visibly incomplete, never a successful
reply. No messages or keys survive page reload through application storage.

The browser fixture uses only a fake model and an ephemeral self-signed HTTPS
loopback proxy, not a deployed service or production certificate. With an
already installed compatible Playwright Core and Chromium:

```sh
dotnet build dotnet/Llm/tests/CNET.Llm.Tests.UiHost/CNET.Llm.Tests.UiHost.csproj -c Release
CNET_TEST_PLAYWRIGHT_MODULE=/absolute/path/to/installed/playwright-core \
  node dotnet/Llm/tests/CNET.Llm.Tests.UiHost/browser-security.cjs
```

The script does not download dependencies. It blocks all off-fixture browser
requests, proves protected chat/key/plain-text rendering/stop behavior, checks
keyboard focus and four viewport widths, and shuts down its private test host.
This is not a full screen-reader or WCAG conformance certification.
