# Managed server boundary closure — 2026-09-06

Scope: `CNET.Llm.Server`, standalone sample startup, and a new private
`CNET.Llm.Tests.ServerSecurity` project. No models, user data, registry, teacher,
GPU, live service or deployed proxy was used. Parent review accepted the
loopback-only proxy contract before implementation. The API-only default is
separate from the legacy browser UI's offline/auth follow-up.

## Decision and implementation

The closure plan's server-security slice replaces open mutation and permissive
CORS with explicit owner configuration. Protected startup requires an inference
secret and explicit trusted-local-TLS-proxy assertion; development is an
explicit loopback/Host-restricted alternative. Administration has a distinct
optional secret, absent means disabled. Keys have no defaults, are never placed
in URLs, and are redacted from policy formatting. Tests use clearly named public
fixture keys, not operational secrets.

`ServerStartup.BuildApp` owns loopback Kestrel listeners even when URL/endpoint
configuration attempts an external bind. Middleware checks actual local/remote
socket addresses and never grants authority from forwarded headers. No TLS
proxy was invented or installed. External deployment requires separate owner
approval, TLS proxy configuration, local-user isolation and upstream limits.

The boundary uses fixed three-role rate counters and one nonwaiting admission
permit. It caps bodies (including chunked/stalled uploads), query/JSON depth,
messages, output, sampling, stops and tokenization. Validation precedes DTO and
engine access. Unknown routes/methods require administration by default. Exact
origins only; no cookie authentication or wildcard/credentialed CORS. Arbitrary
structured-output schemas are withheld until compiler budgets exist.

Nonstream generation now checks the linked request/deadline cancellation token
at token boundaries. Streaming is cancellation-aware and uses no-store; failed
streams abort without a DONE sentinel or exception text. A canceled completed
model load is disposed rather than published. Admission remains owned until
the actual work exits, so timeout does not start a concurrent replacement.

Rationale: ASP.NET cancellation is cooperative, not forced interruption; a
timeout signal alone does not automatically abort a request. See Microsoft's
[request timeout documentation](https://learn.microsoft.com/en-us/aspnet/core/performance/timeouts?view=aspnetcore-10.0).
Kestrel's explicit listener configuration takes precedence over URL-based
configuration; the fixture additionally tests endpoint-configuration override
attempts against the actual bound socket. See Microsoft's
[Kestrel configuration documentation](https://learn.microsoft.com/en-us/aspnet/core/fundamentals/servers/kestrel?view=aspnetcore-10.0).

## Actual RED evidence

All logs are local `/tmp` proof artifacts, not committed runtime output. Initial
RED preceded production behavior changes; later regression REDs preceded their
corresponding guards.

| Log | Observed failure |
| --- | --- |
| `/tmp/cnet-managed-server-red.log` | Anonymous config mutation returned 200; `MANAGED_SERVER_RED anonymous_mutation status=200`; 1 failed |
| `/tmp/cnet-managed-server-resource-red.log` | Four unbounded/malformed requests were not rejected; config max_tokens=999999 returned 200; 4 failed, 14 passed |
| `/tmp/cnet-managed-server-schema-red.log` | Null config, mixed-case token field and wrong-type cases; 4 failed |
| `/tmp/cnet-managed-server-edge-red.log` | Two malformed nested tool shapes reached model availability and uppercase LOCALHOST failed parsing; 3 failed, 32 passed |
| `/tmp/cnet-managed-server-cache-red.log` | `MANAGED_SERVER_RED stream_must_not_be_stored`; SSE overrode no-store with no-cache; 1 failed |

Initial RED SHA-256:
`b827a51ddfcbb21741250713f51c184fa952138f7975a9d5a84c2d7210c10028`.
Resource RED SHA-256:
`b47ec2d948aadef567b4c56aca906746d197fc1e22633d63d268c355872c36b2`.

Two test-fixture issues were diagnosed rather than hidden: a null-conditional
callback had prevented the fake engine's call counter from incrementing; and
HttpClient disposal normally drains a response instead of disconnecting. The
fixture now counts unconditionally and disables response draining. The stream
disconnect proof waits for server-side cancellation while fake engine work is
still blocked, then releases that work and verifies exactly one model call.
These were not claimed as production defects or fixes.

## GREEN verification

SDK 10.0.203; Release builds. No dependencies added and no central package
versions changed. Source was the shared feature worktree (concurrent unrelated
parent/peer changes preserved); review predecessor was
`83ba11b56c17b8620a59b8e7138af0c8b914a77a`. This is not a clean-tree release claim.

```sh
dotnet test dotnet/Llm/tests/CNET.Llm.Tests.ServerSecurity/CNET.Llm.Tests.ServerSecurity.csproj -c Release --nologo --verbosity minimal
dotnet test dotnet/Llm/tests/CNET.Llm.Tests.Unit/CNET.Llm.Tests.Unit.csproj -c Release --nologo --verbosity minimal --filter FullyQualifiedName~Server
dotnet build dotnet/Llm/samples/CNET.Llm.Sample.Server/CNET.Llm.Sample.Server.csproj -c Release --nologo --verbosity minimal
dotnet list dotnet/Llm/tests/CNET.Llm.Tests.ServerSecurity/CNET.Llm.Tests.ServerSecurity.csproj package --vulnerable --include-transitive --format json
```

- Focused real-HTTP suite: 36 passed, 0 failed/skipped;
  `/tmp/cnet-managed-server-boundary-green.log`.
- Existing server unit subset: 18 passed, 0 failed/skipped;
  `/tmp/cnet-managed-server-final-existing.log`.
- Sample build: 0 warnings/errors;
  `/tmp/cnet-managed-server-final-sample-build.log`.
- NuGet direct/transitive vulnerability query: no vulnerable packages reported,
  exit 0; `/tmp/cnet-managed-server-audit.json`, SHA-256
  `61f66d1f00dde4083467ba994a8796fe2fffa1ab9007e330b76d5bfde32a0037`.
- `git diff --check`: passed.

The fake engine is a four-token, one-layer in-memory model exercised through the
real TextGenerator and real ephemeral loopback Kestrel sockets. Tests prove
anonymous/inference mutation refusal, administrator config success, inspection
authorization, startup/Host/origin/forwarded-header boundaries, bounded role
rates, body/query/schema limits, max output, stalled uploads, safe exception and
SSE failure responses, nonwaiting admission, client cancellation and deadlines.
They do not measure actual-model correctness, GPU behavior or deployment safety.

## Explicit limits

Native kernels, prefill, tokenization, templates and model opening remain
cooperative/in-process. Timeout does not provide hard CPU/memory isolation.
Fixed-window role quotas are coarse and allow a boundary burst. All active HTTP
work, including health, shares the one permit. Trusted administrators can still
inspect/load owner-accessible local models. Model replacement still disposes
the incumbent before loading in the initial boundary commit; the independently
reviewed lifecycle follow-up below supersedes that defect. Neither managed
model loading nor its swap is the native certified-capsule lifecycle.

The legacy UI's remote/inline assets and unchecked Markdown link interpolation
were found during review. CSP was not relaxed. The parent authorized a separate
small offline chat-only UI follow-up, with in-memory inference-key entry and
text-only model output; the sample API default remains secure in the meantime.

Skills influenced the work: security/API skills drove fail-closed boundaries and
explicit roles; incremental implementation preserved actual REDs; code-review
checks caught stream caching and the UI issue; documentation recorded the
cooperative/proxy limits instead of claiming a completed external deployment.

## Prepare-and-adopt model lifetime follow-up

The parent review required retaining the incumbent on failed/canceled loading.
Actual RED `/tmp/cnet-managed-server-swap-red.log` showed
`MANAGED_SERVER_SWAP_RED failed_prepare_disposed_incumbent` and
`MANAGED_SERVER_SWAP_RED double_dispose` (2 failures). A private 68-byte GGUF
fixture containing one dummy float tensor but no architecture metadata also
reproduced a retained mapping after loader failure:
`/tmp/cnet-managed-server-loader-red.log`,
`MANAGED_SERVER_SWAP_RED failed_load_mapping_leak` (1 failure). No real weights
were used; the fixture was deleted after the test. Mapping evidence is a Linux
`/proc/self/maps` assertion and is not run on other platforms.

`ServerState.SwapModelAsync` now takes a typed prepare callback returning a fresh
owned `ServerState`, retains the canonical gate for direct callers, validates
before adoption, and transfers all model fields including `PagedFactory`.
Failure/cancellation disposes only the candidate; successful transfer detaches
its ownership. Disposal is gate-synchronized/idempotent and rejects new work.
The only in-tree old callback consumer was the model-management endpoint; graph
and repository search verified its migration. The source-breaking callback
change and exclusive-ownership requirements are documented in `SERVER.md`.

ServerStartup tracks acquired resources during loading and cleans them on
failure. Retired-resource disposal attempts all resources once; failure logs a
fixed warning, leaves the adopted candidate active, exposes administrator
`retirement_cleanup_failed`, and refuses further swaps until restart. HTTP
activation uses the distinct status
`activated_with_retirement_error_restart_required`; subsequent model loads
return 409. The latter response has its own regression RED in
`/tmp/cnet-managed-server-retirement-red.log`.

GREEN `/tmp/cnet-managed-server-swap-final.log`: 45 passed, zero failed/skipped.
This adds real generator usability after failure, canceled candidate disposal,
complete successful transfer/detachment, canonical gate ordering, invalid/self
candidate refusal, degraded retirement/admin visibility, idempotent disposal,
and the private mapping-release check. The initial 36-test API boundary evidence
above remains its historical proof, not a claim that lifecycle was already
repaired in that commit.
