# CNET .NET Inference Harness

Status: first vertical tracer. Synchronous non-streaming generation only.
Cancellation and streaming are explicitly deferred, not implemented.
AMD/ROCm only. No CUDA-specific claims or controls.

## Boundary

Core `cnet.so` remains independent of `llama.cpp`. This harness ships as an
**optional** native plugin, `libcnet_harness.so`, that links `cnet.so` and the
`/home/marble/llama.cpp/build-rocm/bin` libraries. The plugin is the only
symbol boundary the managed .NET layer touches — it never sees a raw
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

## Versioned C ABI

Header: `include/cnet/cnet_harness.h`. Every struct crossing the ABI carries
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

### Open

```c
typedef struct {
    uint32_t abi_version;      /* = CNET_HARNESS_ABI_VERSION */
    uint32_t struct_size;      /* = sizeof(CnetHarnessConfig) */
    const char *model_id;      /* required, UTF-8, must be non-empty */
    const char *model_path;    /* required GGUF file path            */
    uint64_t resource_mask;    /* CNET_MODEL_RESOURCE_GPU* bitmask   */
    uint64_t budget_bytes;     /* resident budget for this session   */
    int main_gpu;              /* llama.cpp main_gpu index           */
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
} CnetHarnessGeneration;

int cnet_harness_generate(CnetHarnessSession *session,
                          const CnetHarnessGenerateOptions *options,
                          CnetHarnessGeneration **generation_out);

void cnet_harness_generation_free(CnetHarnessGeneration *generation);
int  cnet_harness_close(CnetHarnessSession *session);
```

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

## AICIMO must materially affect generation

Each session owns a single persistent `cce_aicimo_router` with
`config.aicimo_num_ops` adapters (>= 4) at `config.aicimo_base_dim`.

`cnet_harness_generate` performs one call to `cce_aicimo_route_decision(role)`
per request. The returned `selected_adapter` is mapped 1:1 to a named sampling
profile:

| adapter index (mod 4) | profile              | temp | top_p | top_k |
|-----------------------|----------------------|------|-------|-------|
| 0                     | DETERMINISTIC        | 0.0  | 1.0   | 0     |
| 1                     | FOCUSED              | 0.30 | 0.85  | 40    |
| 2                     | BALANCED             | 0.70 | 0.90  | 40    |
| 3                     | EXPLORATORY          | 0.95 | 0.95  | 80    |

**Uncertainty fails safe.** If `route_uncertainty >= 0.85`, the plugin
downgrades one profile step (EXPLORATORY→BALANCED, BALANCED→FOCUSED,
FOCUSED→DETERMINISTIC). This reduces exploration when the router is unsure.

Callers may override AICIMO by setting `options->sampling` to any non-AUTO
value; the result reports `aicimo_override == 1` and `effective_sampling`
equal to the override.

## What this tracer does not claim

- No input-conditioned learned routing. The AICIMO decision is a real
  function of `role` and the router's persistent strength state — it is
  deterministic given both.
- No token streaming and no cancellation. The API is synchronous. Adding
  either is a later slice.
- AICIMO is not context expansion. The router's base_dim equals its output
  dim; adapters are identity-initialized.
- The plugin does not embed a duplicate llama.cpp or duplicate CNET catalog.

## Native tests

`tests/test_cnet_harness_contract.c` links the plugin at build time and:

1. Rejects mismatched `abi_version` and mismatched `struct_size` on every
   input struct.
2. Confirms `cnet_harness_open` with a non-existent `model_path` returns
   `CNET_HARNESS_ERR_MODEL_LOAD` and leaks no session.
3. Runs `cce_aicimo_route_decision` for a set of stable role strings against
   the *same* session-lifetime router and verifies that the resulting adapter
   set contains at least two distinct values (profiles are not all adapter 0)
   and that all reported uncertainties are in `[0, 1]`.
4. `cnet_harness_error_string(bad_code)` returns a non-NULL static string.
5. `cnet_harness_generation_free(NULL)` and `cnet_harness_close(NULL)` do not
   crash.

This test does not load a real model or link ggml/llama runtime symbols. The
plugin's AICIMO-only surface is separated from the llama-backed generation
path so the contract test can exercise it hermetically.

## Managed API

`dotnet/Cce/CnetHarness/CnetHarnessNative.cs` — source-generated
`LibraryImport` bindings and error-code marshalling.

`dotnet/Cce/CnetHarness/CnetHarness.cs` — `CnetHarnessSession` public class
built on `SafeHandle`, disposable, throws `CnetHarnessException` for every
non-OK native status. Public options/results expose adapter, uncertainty,
effective profile, token counts, and timings.

`dotnet/Cce/CnetHarness/ICnetHarnessNative.cs` — injectable invoker interface
so managed unit tests do not require loading a real GGUF.

`dotnet/CceHost/IChatClient.cs` — small chat-client abstraction implemented by
`OllamaClient` (legacy, `--ollama`) and `CnetHarnessChatClient` (default when
configured, selected by `--agent`).

## Build

- `make cnet_harness_contract_test` — hermetic, no GGUF required.
- `make dotnet_harness_test` — runs the managed unit tests (fakes at native
  boundary, real GGUF explicitly not required).
- Real GGUF inference remains outside `unified` / `release_integrity`.
