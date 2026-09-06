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

## Offline browser follow-up

The parent rejected permanent removal of the existing chat surface and approved
an explicit opt-in, offline, supervised chat-only replacement. `--ui` and the
existing `serveUi: true` enable three constant embedded assets. Metadata on those
exact routes permits anonymous bootstrap but not API calls; static requests
refuse bodies and retain transport/origin/rate/deadline controls while avoiding
the model permit. API authentication/admin separation remains unchanged.

The page uses local CSS/JS only, labeled native controls, an in-memory inference
key, same-origin bearer headers, no cookie/redirect authentication, bounded
history/transcript/stream parsing, and `textContent` for all model text. The old
CDN/inline script/Markdown renderer and browser administrator/telemetry panels
were replaced rather than weakening CSP. The administrator APIs remain usable;
the smaller UI does not claim old-panel feature parity. Old assets remain
recoverable in git history.

Actual RED before UI behavior edits:

- `/tmp/cnet-managed-ui-http-red.log`: 3 failures, including
  `MANAGED_UI_RED bootstrap_requires_preexisting_header`.
- `/tmp/cnet-managed-ui-browser-red.log`: actual Chromium against the private
  HTTPS proxy returned 401, `MANAGED_UI_RED anonymous_static_bootstrap`.

GREEN `/tmp/cnet-managed-ui-http-green.log`: 48 focused tests passed, none failed
or skipped. Existing server tests remained 18/18
(`/tmp/cnet-managed-ui-existing.log`). Sample and test-host builds had zero
warnings/errors (`/tmp/cnet-managed-ui-sample-build.log`,
`/tmp/cnet-managed-ui-host-final-build.log`). No package dependencies were added.

The browser proof uses installed Playwright Core and cached Chromium, with a
fake four-token model and an in-memory self-signed certificate on an ephemeral
loopback test proxy. It does not configure a real proxy or relax production
origin validation. Certificate verification is bypassed only for that isolated
browser fixture; off-origin requests are actively blocked. The test host is
stopped at the end. The fixture outputs HTML/event-handler and javascript-link
payloads that the browser must display solely as text.

`/tmp/cnet-managed-ui-browser-proof.log` records
`MANAGED_UI_BROWSER_GREEN protected_https_chat=1 text_only=1 ephemeral_key=1 no_admin=1 stop=1 viewports=4 keyboard=1 external_requests=0`.
The proof checks key header use/cleared input/no persistent storage, unauthorized
mutation refusal, successful DONE handling, stop/incomplete state, reload/forget,
no page errors, keyboard focus, 320/768/1024/1440-pixel overflow, and helper-text
contrast. A first browser selector matched both "Message" and "Conversation
messages"; narrowing the test to the exact accessible label fixed that fixture
ambiguity without changing production behavior.

Screenshots `/tmp/cnet-managed-ui-reviewed.png` and
`/tmp/cnet-managed-ui-reviewed.png.mobile.png` are private fake-data artifacts;
visual inspection checks readable layout and literal untrusted output. No full
screen-reader or WCAG certification is claimed. The frontend skill's optional
accessibility reference was missing, so its complete built-in checklist and
actual browser checks were used. Browser API use was checked against the
[official Playwright browser documentation](https://playwright.dev/docs/api/class-browsertype).

A final deterministic browser cleanup barrier found a stale-session race before
commit: `/tmp/cnet-managed-ui-forget-red.log` records
`MANAGED_UI_RED forgotten_session_revived`, where a completion changed the
forgotten-access status back to "Reply complete" and could restore old history.
Access changes now increment a generation counter; stale success/error
continuations cannot restore history/status. The full browser proof including
that barrier passed in `/tmp/cnet-managed-ui-forget-green.log`.

## Final sequential managed verification

At `448592cb290982096b7b756e6823399acf7cc6b5`, the parent requested a final
cross-managed run. Commands ran sequentially, with no production edits during
verification. SDK: 10.0.203. Installed runtimes used: Microsoft.NETCore.App
10.0.7 and 8.0.30; Microsoft.AspNetCore.App 10.0.7.

| Suite | Passed | Failed | Skipped | Test-run duration | Log |
| --- | ---: | ---: | ---: | --- | --- |
| Full Cce.Llm, including scriptlets | 426 | 0 | 4 | 1m 29s | `/tmp/cnet-managed-final-cce-tests.log` |
| Control-plane | 80 | 0 | 0 | 72ms | `/tmp/cnet-managed-final-control-tests.log` |
| Server security/lifetime/UI HTTP | 48 | 0 | 0 | 724ms | `/tmp/cnet-managed-final-server-tests.log` |
| Existing server unit subset | 18 | 0 | 0 | 28ms | `/tmp/cnet-managed-final-existing-server-tests.log` |

The four skips are `NativeGhostMemoryTests` fixture-gated cases (local GGUF and
native harness required), not passes. This run did not exercise their native
integration; the later explicit prebuilt-harness run below covers that subset.
Full Cce.Llm emitted one existing xUnit2013 analyzer warning in
`GhostOrchestrationTests.cs:244`; no failure was hidden or threshold relaxed.

Final real-browser receipt `/tmp/cnet-managed-final-browser.log` passed with
`forgotten_session_retained=1` in addition to the protected chat/rendering/key/
stop/viewport/keyboard checks. SHA-256:
`b798d7b5addc939ec5b356b0d1e563f26349470485720b39e2f86a002d38915a`.
The full Cce.Llm log SHA-256 is
`d3bd2384ff696ae514b699b7d35ae0e1f4256974bf929777511203d1845c46c5`;
the final server-security log SHA-256 is
`b61ba726b6e1695b2bcd0c59bb639e9c3f543c1a638123c11302691a79be3170`.

Five separate `dotnet list <project> package --vulnerable --include-transitive
--format json` queries exited 0, reporting no vulnerable packages from the
configured `https://api.nuget.org/v3/index.json` source:

- Cce.Llm.Tests: `/tmp/cnet-managed-final-audit-cce.json`.
- Standalone Cce.Scriptlet.Worker: `/tmp/cnet-managed-final-audit-worker.json`.
- CnetControlPlane.Tests: `/tmp/cnet-managed-final-audit-control.json`.
- ServerSecurity tests: `/tmp/cnet-managed-final-audit-server.json`.
- UiHost fixture: `/tmp/cnet-managed-final-audit-ui.json`.

These are NuGet project-graph audit results, not a certification of OS/runtime,
browser or deployment security. No package version or shared build file was
changed as part of this final verification.

## Existing-asset native memory interoperability supplement

Read-only prerequisite investigation found both exact `TestModel.Resolve`
fixtures already present in
`/home/marble/.cnet-llm/test-cache/QuantFactory/SmolLM-135M-GGUF/`.
The resolver selects `SmolLM-135M.Q8_0.gguf` before `SmolLM-135M.Q4_K_M.gguf`.
The closure worktree did not contain `bin/libcnet_harness.so`, and
`CNET_HARNESS_LIBRARY` was unset; this missing discovery path caused the four
skips, not missing model bytes. `NativeFactAttribute` in
`dotnet/Cce.Llm.Tests/NativeGhostMemoryTests.cs` accepts an explicit existing
harness path before searching test-binary ancestors.

After the parent approved the exact bounded command, the existing Release test
assembly ran without build/restore from new private directory
`/tmp/cnet-native-ghost-closure.upWXspdA`, also used as `TMPDIR`:

```sh
timeout --signal=TERM --kill-after=10s 180s \
  env -u CNET_ROUTE_LOG -u GGML_BACKEND_PATH -u LD_LIBRARY_PATH -u LD_PRELOAD \
  TMPDIR=/tmp/cnet-native-ghost-closure.upWXspdA \
  CNET_HARNESS_LIBRARY=/home/marble/AI/CNET/bin/libcnet_harness.so \
  DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 \
  dotnet vstest \
  /home/marble/AI/CNET-worktrees/product-closure-20260906/dotnet/Cce.Llm.Tests/bin/Release/net10.0/CNET.Cce.Llm.Tests.dll \
  --TestCaseFilter:FullyQualifiedName~NativeGhostMemoryTests \
  --ResultsDirectory:/tmp/cnet-native-ghost-closure.upWXspdA/results \
  '--logger:console;verbosity=normal'
```

**4 passed, 0 failed, 0 skipped; 4.3571 seconds total**:

- `GhostMemory_CarriesFactsAcrossNativeSessions`.
- `GhostMemory_WrittenByManaged_RecalledByNative`.
- `NativeCountTokens_AgreesWithManagedTokenizer`.
- `NativeBackend_IrrelevantQuestion_GetsNoMemoryBlock`.

The tests use CPU, a 2 GiB native model budget, context 2048, batch 512 and
8 threads. Memory stores use fresh private `cnet-ghost-native/<GUID>` paths;
their owning tests removed those stores. No model, existing memory, service,
registry or configuration was changed. No download or teacher call occurred.

Retained receipt:
`/tmp/cnet-native-ghost-closure.upWXspdA/native-ghost-tests.log`, SHA-256
`77997d28901bbe5a1a1fa344f320baa43a6a8d2d4ac8d049a0ee8a5f1495c069`.
The executed test assembly SHA-256 is
`1f713c940a8ad8f6bc9cf2d08a6d52d0d736581a855b1090775af3665931809a`.
Input identities, unchanged before/after the run:

- Q8_0 GGUF (144,810,528 bytes):
  `7d4afa1d8a5587f4beb8b75dab755f90d13d3ae2b8b9f148545b414ecfb9123a`.
- Prebuilt `libcnet_harness.so`:
  `ce7a0b212cd624f94e964a7209674ce6e0ca34b576eea2483fe1322cedbc91ae`.
- `libcnet.so.5` resolves to `/home/marble/AI/CNET/cnet.so`, byte-identical to
  the closure worktree's existing `cnet.so`:
  `b6496ef269ff78bb138711682b9bcd4e11b2c4c21d51bb9ba0e9695b90788012`.

Cleared-environment loader `--list` output is retained in
`/tmp/cnet-native-ghost-closure.upWXspdA/library-bindings.txt`. The plugin's
RUNPATH and resolved `libllama.so.0`, `libggml.so.0`, `libggml-base.so.0` and
`libggml-cpu.so.0` point to `/home/marble/llama.cpp/build-cpu/bin`; the actual
test log reports CPU tensor placement and mapped model buffers. The harness
source files in the main checkout and closure worktree were byte-identical.

This supplements, rather than rewrites, the earlier full-suite skip receipt.
It proves the four selected assertions against fingerprinted **prebuilt CPU
artifacts**: memory prompt/provenance transfer, nonempty generation, tokenizer
agreement within the existing BOS allowance, and irrelevant-query exclusion.
It is not fresh native-build provenance, GPU evidence, broad answer-quality
evaluation or live-memory acceptance; those claims remain WITHHELD.
