# CNET .NET Inference Harness

Status: working vertical tracer with hermetic contract gates and an optional
real-GGUF CPU smoke executable. Synchronous non-streaming generation only.
Cancellation and streaming are explicitly deferred, not implemented. CPU is
a first-class resource; GPU resources are opt-in and unrelated to any
specific vendor stack outside the pinned llama.cpp build the plugin links.

## Boundary

Core `cnet.so` remains independent of `llama.cpp`. This harness ships as an
**optional** native plugin, `libcnet_harness.so`, that links `cnet.so` and the
libraries in the selected `LLAMA_CPP_BUILD/bin` directory. The plugin is the
only symbol boundary the managed .NET layer touches — it never sees a raw
`llama_model *` or any llama.cpp type.

The plugin reuses:

| CNET primitive                       | Role                                  |
|--------------------------------------|---------------------------------------|
| `cnet_model_descriptor_probe`        | Catalog/format probe                  |
| `CnetModelBackendSpec`               | Backend registration                  |
| `CnetModelManager`                   | Single residency/budget authority     |
| `cnet_model_acquire`/`_release`      | Lease lifecycle                       |
| `cce_aicimo_router` + `route_decision` | Role-biased adapter/policy routing  |

No duplicate residency manager. No parallel budget bookkeeping.

**AICIMO scope.** AICIMO performs adapter/policy routing — it selects a
sampling profile and reports uncertainty. It does *not* generate tokens and
does *not* expand the context window.

**Single-process global backend refcount.** `llama_backend_init/free` and
`ggml_backend_load_all` are process-global. The plugin protects them with a
C++ mutex and refcount: the first session that opens acquires global state;
the last session that closes releases it. Multiple concurrent sessions
therefore cannot free global state out from under one another.

**Per-session serialization.** A llama context is not reentrant. The managed
`CnetHarnessSession` serializes generation, route probes, and disposal on one
session. Separate sessions may execute concurrently. Native C callers must
provide the same serialization when sharing one `CnetHarnessSession *`.

## Versioned C ABI

Header: `include/cnet_harness.h`. Every struct crossing the ABI carries
`abi_version` and `struct_size` as the first two `uint32_t` fields. The plugin
rejects any input whose `abi_version != CNET_HARNESS_ABI_VERSION` or whose
`struct_size` does not match the current `sizeof`.

```c
#define CNET_HARNESS_ABI_VERSION 1u
```

### Session lifecycle

```c
typedef struct CnetHarnessSession CnetHarnessSession;

typedef enum {
    CNET_HARNESS_OK             = 0,
    CNET_HARNESS_ERR_INVALID    = -1,  /* bad abi_version, struct_size, args  */
    CNET_HARNESS_ERR_MODEL_LOAD = -2,  /* catalog/probe/acquire failed        */
    CNET_HARNESS_ERR_BACKEND    = -3,  /* llama.cpp context / decode failed   */
    CNET_HARNESS_ERR_STATE      = -4,  /* misuse of an already-closed session */
    CNET_HARNESS_ERR_INTERNAL   = -5   /* unexpected non-recoverable failure  */
} CnetHarnessStatus;

const char *cnet_harness_error_string(int status);  /* static storage, never NULL */
```

### Resources

Exactly one resource bit must be set in `config.resource_mask`. Multi-bit or
unknown masks are rejected during validation, before any backend or model
state is allocated.

```c
typedef enum {
    CNET_HARNESS_RESOURCE_CPU  = 1u << 0,
    CNET_HARNESS_RESOURCE_GPU0 = 1u << 1,
    CNET_HARNESS_RESOURCE_GPU1 = 1u << 2,
    CNET_HARNESS_RESOURCE_GPU2 = 1u << 3,
    CNET_HARNESS_RESOURCE_GPU3 = 1u << 4
} CnetHarnessResource;
```

For `CNET_HARNESS_RESOURCE_CPU` the llama.cpp model is loaded with
`n_gpu_layers = 0` and `main_gpu` is ignored. For GPU resources the plugin
uses `n_gpu_layers = -1` (offload every supported layer) and the caller's
`main_gpu` index (or 0 when it is negative).

### Open

```c
typedef struct {
    uint32_t abi_version;      /* = CNET_HARNESS_ABI_VERSION */
    uint32_t struct_size;      /* = sizeof(CnetHarnessConfig) */
    const char *model_id;      /* required, UTF-8, must be non-empty */
    const char *model_path;    /* required GGUF file path            */
    uint64_t resource_mask;    /* EXACTLY one CNET_HARNESS_RESOURCE_* bit */
    uint64_t budget_bytes;     /* resident budget, must be > 0       */
    int32_t main_gpu;          /* llama.cpp main_gpu index; -1 means "not applicable" */
    uint32_t n_ctx;            /* context window in tokens           */
    uint32_t n_batch;          /* llama.cpp n_batch                  */
    uint32_t n_threads;        /* generation thread count            */
    uint32_t aicimo_num_ops;   /* adapter bank size, >= 4            */
    uint32_t aicimo_base_dim;  /* adapter dim, >= 8                  */
} CnetHarnessConfig;

int cnet_harness_open(const CnetHarnessConfig *config,
                      CnetHarnessSession **session_out);
```

`open` validates the config *before* calling `llama_backend_init` or acquiring
any model. On any validation failure `*session_out` is NULL and the plugin has
allocated nothing. On any *later* failure (probe/acquire/context creation) all
partially-acquired state is released before returning.

### Generate

```c
typedef enum {
    CNET_HARNESS_SAMPLING_AUTO       = 0,  /* AICIMO decides the profile      */
    CNET_HARNESS_SAMPLING_DETERMINISTIC = 1,  /* greedy                       */
    CNET_HARNESS_SAMPLING_FOCUSED    = 2,  /* low temp, tight top_p           */
    CNET_HARNESS_SAMPLING_BALANCED   = 3,  /* mid temp                        */
    CNET_HARNESS_SAMPLING_EXPLORATORY = 4  /* high temp, wider top_p          */
} CnetHarnessSamplingMode;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *system;              /* optional, may be NULL              */
    const char *user;                /* required, UTF-8                    */
    const char *role;                /* required, UTF-8, feeds AICIMO      */
    uint32_t max_tokens;
    uint32_t seed;
    CnetHarnessSamplingMode sampling; /* AUTO honors AICIMO; others override */
} CnetHarnessGenerateOptions;
```

`system` and `user` are handed to the model's real chat template via
`llama_chat_apply_template`. The plugin never injects control tokens for
specific model families (no `/no_think`, no fabricated `<think>` blocks). If
template resolution or application fails, a plain `System:/User:/Assistant:`
transcript is used as fallback.

**Context bound.** Generation caps `max_tokens` at `n_ctx − prompt_tokens`
before the decode loop starts. If the tokenised prompt already fills the
window, `generate` returns `CNET_HARNESS_ERR_INVALID` without decoding. The
plugin never runs the decode loop past `n_ctx` and never discards a
partially-produced response.

### Result

```c
typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    char *text;                  /* heap, UTF-8, freed by generation_free */
    uint32_t prompt_tokens;
    uint32_t generated_tokens;
    double prompt_ms;
    double generation_ms;
    uint32_t selected_adapter;   /* AICIMO's chosen adapter index         */
    float route_uncertainty;     /* [0, 1] normalized entropy             */
    CnetHarnessSamplingMode effective_sampling; /* profile actually used  */
    int aicimo_override;         /* 1 if caller override bypassed AICIMO  */
    float effective_temperature;
    float effective_top_p;
    uint32_t effective_top_k;
    float effective_min_p;
} CnetHarnessGeneration;

int cnet_harness_generate(CnetHarnessSession *session,
                          const CnetHarnessGenerateOptions *options,
                          CnetHarnessGeneration **generation_out);

void cnet_harness_generation_free(CnetHarnessGeneration *generation);
int  cnet_harness_close(CnetHarnessSession *session);
```

The `effective_*` fields are the exact numeric parameters the backend fed
into the llama.cpp sampler chain, resolved from `effective_sampling` through
one central table in `cnet_harness_core.c`. Deterministic reports honest
zeros (`temperature=0`, `top_p=1`, `top_k=0`, `min_p=0`) — nothing is
smoothed after the fact. `CnetHarnessRouteInfo` carries the same four
fields so the non-generative probe returns the same evidence.

### Ownership

- `CnetHarnessGeneration *` is heap-owned by the plugin and freed *only* by
  `cnet_harness_generation_free`. The struct is opaque to callers except for
  reading fields; the `text` pointer is owned by the same allocation and must
  never be freed separately.
- The managed wrapper never releases the raw pointer twice: it holds it in a
  `SafeHandle` whose `ReleaseHandle` calls `cnet_harness_generation_free`
  exactly once. Copying the pointer out of the handle is not exposed.
- `cnet_harness_generation_free(NULL)` and `cnet_harness_close(NULL)` are
  safe no-ops.
- Session teardown runs in two phases: `harness_backend_prepare_close` frees
  per-session backend state (context, sampler) while the CNET model lease is
  still held. After the core releases the lease and closes the manager,
  `harness_backend_finish_close` runs and decrements the process-global
  backend refcount. This order guarantees the lease-owned model handle stays
  valid until every backend-owned object that references it is gone.

## AICIMO must materially affect generation

Each session owns a single persistent `cce_aicimo_router` with
`config.aicimo_num_ops` adapters (>= 4) at `config.aicimo_base_dim`.

`cnet_harness_generate` performs one call to `cce_aicimo_route_decision(role)`
per request. The returned `selected_adapter` is mapped 1:1 to a named sampling
profile:

| adapter index (mod 4) | profile              | temp | top_p | top_k | min_p |
|-----------------------|----------------------|------|-------|-------|-------|
| 0                     | DETERMINISTIC        | 0.0  | 1.0   | 0     | 0.0   |
| 1                     | FOCUSED              | 0.30 | 0.85  | 40    | 0.05  |
| 2                     | BALANCED             | 0.70 | 0.90  | 40    | 0.05  |
| 3                     | EXPLORATORY          | 0.95 | 0.95  | 80    | 0.03  |

**Uncertainty fails safe.** If `route_uncertainty >= 0.85`, the plugin
downgrades one profile step (EXPLORATORY→BALANCED, BALANCED→FOCUSED,
FOCUSED→DETERMINISTIC). This reduces exploration when the router is unsure.

Callers may override AICIMO by setting `options->sampling` to any non-AUTO
value; the result reports `aicimo_override == 1` and `effective_sampling`
equal to the override. The `effective_temperature`, `effective_top_p`,
`effective_top_k`, and `effective_min_p` fields carry the actual parameters
that reach the llama.cpp sampler.

## What this tracer does not claim

- No token streaming and no cancellation. The API is synchronous. Adding
  either is a later slice.
- No input-conditioned learned routing. The AICIMO decision is a real
  function of `role` and the router's persistent strength state — it is
  deterministic given both.
- AICIMO is not context expansion. The router's base_dim equals its output
  dim; adapters are identity-initialized.
- The plugin does not embed a duplicate llama.cpp or duplicate CNET catalog.

## Native tests

`tests/test_cnet_harness_contract.c` links `cnet_harness_core.c` into a
hermetic test binary and provides *its own strong* backend hooks so the
route-only surface can be exercised without llama.cpp. This is deliberately
**not** the real llama plugin; the optional real-model target is documented
below and remains outside hermetic release acceptance.

`tests/test_cnet_harness_failclosed.c` links the same core with NO strong
overrides. It proves that the plugin's default (weak) `harness_backend_open`
fails closed with `CNET_HARNESS_ERR_BACKEND` even when the model path is a
readable dummy file, and that `session_out` stays NULL on that failure.

Both tests are built and run by `make cnet_harness_contract_test`.

Together they exercise:

1. Rejected `abi_version` / `struct_size` on every input struct.
2. Multi-bit and unknown resource masks rejected.
3. Zero resident budget rejected before backend allocation.
4. `cnet_harness_open` with a non-existent `model_path` returns
   `CNET_HARNESS_ERR_MODEL_LOAD` and leaks no session.
5. `cce_aicimo_route_decision` distributes over at least two adapters and
   never all to adapter 0; uncertainties in `[0, 1]`.
6. Reported `effective_*` numeric parameters agree with the central
   profile table; deterministic reports honest zeros.
7. Invalid sampling enums fail before generation or route probing.
8. Two-phase teardown fires `prepare_close` before `finish_close` exactly
   once each per session.
9. `cnet_harness_error_string(bad_code)` returns a non-NULL static string.
10. `cnet_harness_generation_free(NULL)` and `cnet_harness_close(NULL)` do
   not crash.
11. The weak fail-closed default refuses to open a session even with a
   readable dummy model path.

Neither test loads a real model, links the llama.cpp runtime, or exercises
the real generation path. The plugin build target is a separate slice.

## Managed API

`dotnet/Cce/CnetHarness/CnetHarnessNative.cs` — source-generated
`LibraryImport` bindings and error-code marshalling.

`dotnet/Cce/CnetHarness/CnetHarnessSession.cs` — `CnetHarnessSession` public
class built on `SafeHandle`, disposable, serializes same-session calls, and
throws `CnetHarnessException` for every non-OK native status. Public
options/results expose adapter,
uncertainty, effective profile, effective temperature / top_p / top_k /
min_p, token counts, and timings.

`dotnet/Cce/CnetHarness/ICnetHarnessNative.cs` — injectable invoker interface
so managed unit tests do not require loading a real GGUF.

`dotnet/Cce/CnetHarness/CnetHarnessLibraryResolver.cs` — single
`DllImportResolver` registered once per AppDomain against the `CNET.Cce`
assembly. Only the `cnet_harness` library name is handled; any other
library name returns zero so unrelated P/Invokes keep the OS default
behavior. Candidate order:

1. `CNET_HARNESS_LIBRARY` (full-path override, deterministic).
2. `$CNET_HARNESS_BIN_DIR/libcnet_harness.so`.
3. `<AppBase>/libcnet_harness.so`.
4. `<AppBase>/bin/libcnet_harness.so`.
5. `<AppBase>/../bin/libcnet_harness.so`.
6. OS default (`LD_LIBRARY_PATH`, `RPATH`).

`dotnet/CceHost/CceHostConfig.cs` — parses the harness env vars
(`CNET_HARNESS_RESOURCE_MASK`, `CNET_HARNESS_MAIN_GPU`,
`CNET_HARNESS_BUDGET_BYTES`, `CNET_HARNESS_CONTEXT_TOKENS`,
`CNET_HARNESS_BATCH_TOKENS`, `CNET_HARNESS_THREADS`). Defaults are CPU +
`MainGpu = -1`; invalid values throw `ArgumentException` with a specific
message and the host prints the error and exits `2` — misconfiguration
never silently succeeds.

`dotnet/CceHost/IChatClient.cs` — small chat-client abstraction implemented by
`OllamaClient` (legacy, `--ollama`) and `CnetHarnessChatClient` (default when
configured, selected by `--agent`).

## Host mode selection

- `--agent` uses the CNET native harness. **Requires** `CNET_HARNESS_MODEL`
  (path to a GGUF file). Without it the host prints a config error and
  exits non-zero *before* any HTTP call or backend start.
- `--ollama` uses the legacy HTTP path. **Requires** `CNET_LLM_MODEL`
  explicitly — there is no baked-in default model name and this host never
  starts an Ollama backend for you.
- No silent fallback: `--agent` without `CNET_HARNESS_MODEL` fails; it does
  not quietly switch to HTTP.

## Build

- `make cnet_harness_contract_test` — hermetic. Builds and runs both the
  contract test (with strong fake hooks) and the fail-closed sibling.
  Does not require a GGUF or llama.cpp.
- `make LLAMA_CPP_BUILD=/home/marble/llama.cpp/build-cpu cnet_harness_plugin`
  — builds `bin/libcnet_harness.so` against the pinned CPU llama.cpp. The
  target compiles with `-Wall -Wextra -Werror` and sets RUNPATH to
  `$ORIGIN:$ORIGIN/..:$(LLAMA_CPP_BUILD)/bin` so both `cnet.so` (via its
  own `libcnet.so.$(CNET_ABI_VERSION)` symlink in `bin/`) and the llama
  libraries resolve without global `LD_LIBRARY_PATH` munging.
- `make dotnet_harness_test` — runs the managed unit tests. Uses fakes at
  the native boundary; requires neither `libcnet_harness.so` nor a GGUF.
- `make LLAMA_CPP_BUILD=/home/marble/llama.cpp/build-cpu \
  CNET_HARNESS_MODEL=/absolute/path/model.gguf cnet_harness_real_smoke` —
  builds the CPU plugin and committed `dotnet/CnetHarnessSmoke` tracer, then
  runs `.NET → plugin → CNET model manager → AICIMO → llama.cpp` under a
  300-second hard timeout with all accelerator visibility disabled. It starts
  no server and writes evidence to `logs/cnet_harness_real_smoke.log`.

The real-GGUF target is intentionally outside `unified` / `release_integrity`:
portable release acceptance must not depend on a private model file or an
external llama.cpp checkout. `unified_native` includes the AICIMO and harness
contract tests plus an ABI symbol assertion; `unified` runs the full managed
test project.
