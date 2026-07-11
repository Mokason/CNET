# CCE Training API Maturity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish CCE model serialization (self-contained `.cce` bundle), add a safe-handle + composition-first model-definition API above raw `IntPtr`, and expose a bounded set of training extras (loss types + real loss, optimizer knobs, accuracy metric, callback-able `Fit()`).

**Architecture:** A `cce_model` is serialized to one `.cce` archive: a `MODEL_MANIFEST` section (model + scheduler + per-forest/branch metadata) plus each branch's cascade re-serialized inline via the existing `cce_cascade_save_to_archive`. Load materializes every branch as a HOT owned copy into archive-less, model-owned forests, then closes the bundle. The `.NET` layer wraps forests in a `SafeHandle`, surfaces the engine's typed composition verbs, and adds a managed `Fit()` loop for callbacks/metrics while leaving the C fast-path `Train()` intact.

**Tech Stack:** C (MinGW gcc, `make`), `.NET 10` (`LibraryImport` P/Invoke, xUnit). Design doc: `docs/superpowers/specs/2026-06-27-cce-training-api-maturity-design.md`.

---

## Conventions for this plan (read first)

- **NO GIT on CNET** (`.git` was deleted). There are **no commit steps**. Each task ends with a **Checkpoint**: build + run the relevant tests and confirm green. Verification is by test output, not version control.
- **C tests** follow the in-repo idiom (see `tests/cce_view_test.c`): a local `CHECK(cond,msg)` macro with `checks`/`fails` counters, `main` returns `fails ? 1 : 0`, built+run via a `make` target.
- **C build:** `make cce_model_test` (added below) compiles the whole `$(CCE)` source set + the test and runs it. `make cce_dll` builds `cce.dll` for `.NET`.
- **.NET tests:** xUnit in `dotnet/Cce.Tests`. Run with `dotnet test dotnet/Cce.Tests/Cce.Tests.csproj`. The native resolver in `CceNative.cs` finds `cce.dll` next to the test output or up the tree; **rebuild `cce.dll` before `dotnet test`** after any C change.
- **Endianness/padding:** single MinGW/x86 target; the manifest is written **field-by-field via memcpy** (avoids struct padding), host-endian, version-gated. Not cross-endian portable (out of scope).
- **Behavior preservation:** new `cce_model` fields default to today's hardcoded values, so existing training is byte-for-byte unchanged.

---

## File Structure

**New**
- `src/cce/cce_model_internal.h` — definition of `struct cce_model` (shared by `cce_model.c` + `cce_model_io.c`).
- `src/cce/cce_model_io.h` / `src/cce/cce_model_io.c` — bundle manifest writer/reader; `cce_model_io_save/load`.
- `tests/test_cce_model_save.c` — C round-trip + loss regression gate.
- `dotnet/Cce/CceForest.cs` — `SafeHandle` forest wrapper + composition verbs.
- `dotnet/Cce/CceMetrics.cs` — managed `Accuracy`.
- `dotnet/Cce/CceCallbacks.cs` — `CceCallbacks` + `EpochInfo`.

**Edited**
- `src/cce/cce_model.c` — include internal header; new state + defaults; `save`/`load` delegate; `destroy` frees owned; `set_loss`/`set_learn_params`; real loss in dispatch.
- `include/cce/cce_model.h` — declare `cce_model_set_loss`, `cce_model_set_learn_params`.
- `include/cce/cce_forest.h` — add `CCE_API` to exported funcs; declare `cce_forest_branch_count` / `cce_forest_branch_name`.
- `src/cce/cce_forest.c` — implement the two accessors (no other behavior change).
- `dotnet/Cce/CceModel.cs` — `Save`/`Load`/`Add(CceForest)`/`Fit`; `[Obsolete]` on raw `AddForest`.
- `dotnet/Cce/CceEnums.cs` — `CceLossType`, `CceConnectionType`, config fields.
- `dotnet/Cce/CceNative.cs` — P/Invoke for new C entry points.
- `src/cce/cce_archive.c` — (A3, out of original plan) reserve the 20-byte `CCE1` header zone for fresh archives so the first appended section no longer lands at offset 0 and gets clobbered by the close-time header write. Pre-existing latent bug first exposed by the bundle's append→close→reopen→read path. Read-safe: only changes `next_write_offset` for empty files.
- `Makefile` — register `cce_model_io.c` in `$(CCE)`; add `cce_model_test` target.

---

## Manifest binary layout (reference — used by Tasks A2/A3)

All multi-byte values little-endian via `memcpy`. Fixed-size char fields are zero-padded.

```
HEADER:
  char     magic[4]      = "CMDL"
  int32    version       = 1
  char     name[64]
  int32    diff_mode
  int32    classify
  float    goodness_threshold
  float    dfa_strength
  float    grad_clip
  int32    has_scheduler            (0/1)
  [if has_scheduler] cce_scheduler fields, each written individually:
     int32 type; float initial_lr; int32 warmup_epochs; float decay_factor;
     int32 step_size; float plateau_factor; int32 plateau_patience;
     float current_lr; float best_loss; int32 patience_counter; int32 total_steps
  int32    num_forests

PER FOREST (num_forests times):
  char     fname[64]               (from model->forest_names[fi])
  int32    num_branches
  int32    centroid_dim
  int32    sealed
  int32    diff_mode
  int32    default_exact_tail_length

PER BRANCH (num_branches times):
  char     bname[64]
  float    centroid[32]
  int32    centroid_dim
  int32    tier
  int32    diff_mode
  int32    exact_tail_length
  int32    num_connections
  char     conn_names[8][64]
  int32    conn_types[8]
  uint64   cascade_offset          (offset of this branch's cascade section in THIS bundle)
```

Each branch cascade is stored as its own archive section named `f{fi}_b{bi}` via `cce_cascade_save_to_archive`, and its returned offset is recorded as `cascade_offset`.

---

# Phase A — Serialization

### Task A1: Failing C round-trip test + build target

**Files:**
- Create: `tests/test_cce_model_save.c`
- Modify: `Makefile` (add `cce_model_test` target)

- [ ] **Step 1: Write the failing test**

Create `tests/test_cce_model_save.c`:

```c
/* Round-trip + loss regression gate for cce_model save/load. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_tensor.h"
#include "../include/cce/cce_block.h"
#include "../include/cce/cce_cascade.h"
#include "../include/cce/cce_forest.h"
#include "../include/cce/cce_model.h"

static int checks = 0, fails = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } } while (0)

/* Build a model with one forest holding one 2-block cascade. Returns model + the
   forest path used (caller removes it). The forest OWNS the cascade after add_branch
   (shallow copy transfers block ownership) -- do NOT free the local cascade. */
static cce_model* build_model(const char* forest_path, int IN, int HID, int OUT) {
    cce_cascade cas; cce_cascade_init(&cas, 4);
    cce_block b0, b1;
    cce_block_init_linear(&b0, IN, HID, 0.01f);
    cce_block_init_linear(&b1, HID, OUT, 0.01f); b1.type = CCE_BLOCK_LINEAR_HEAD;
    cce_cascade_append(&cas, &b0);
    cce_cascade_append(&cas, &b1);

    cce_forest* f = NULL;
    remove(forest_path);
    if (cce_forest_open(&f, forest_path, 4) != CCE_OK) return NULL;
    if (cce_forest_add_branch(f, &cas, "b0") != CCE_OK) return NULL;
    /* forest owns the cascade blocks now; do not free `cas`. */

    cce_model* m = NULL;
    if (cce_model_create(&m, "savetest") != CCE_OK) return NULL;
    if (cce_model_add_forest(m, f, "f0") != CCE_OK) return NULL;
    return m;
}

int main(void) {
    printf("=== cce_model save/load round-trip ===\n");
    const int IN = 8, HID = 6, OUT = 3;
    float x[8] = { 0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f, 0.7f, -0.8f };
    const char* forest_path = "test_model_forest.cce";
    const char* bundle_path = "test_model_bundle.cce";

    cce_model* m = build_model(forest_path, IN, HID, OUT);
    CHECK(m != NULL, "build model");

    int lbl = -99; float conf = -1.0f;
    CHECK(cce_model_infer(m, x, IN, &lbl, &conf) == CCE_OK, "reference infer ok");

    remove(bundle_path);
    CHECK(cce_model_save(m, bundle_path) == CCE_OK, "model save ok");

    cce_model* m2 = NULL;
    CHECK(cce_model_create(&m2, "loadtest") == CCE_OK, "create dest model");
    CHECK(cce_model_load(m2, bundle_path) == CCE_OK, "model load ok");

    int lbl2 = -99; float conf2 = -1.0f;
    CHECK(cce_model_infer(m2, x, IN, &lbl2, &conf2) == CCE_OK, "loaded infer ok");
    CHECK(lbl2 == lbl, "loaded label == reference label");
    CHECK(fabsf(conf2 - conf) < 1e-5f, "loaded confidence == reference confidence");

    cce_model_destroy(m);
    cce_model_destroy(m2);   /* must free owned forest/cascade cleanly, no double-free */
    remove(forest_path);
    remove(bundle_path);

    printf("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    return fails ? 1 : 0;
}
```

- [ ] **Step 2: Add the Makefile target**

In `Makefile`, after the `forest_view:` target (around line 693), add:

```make
cce_model_test: $(CCE) $(CCE_CUDA_OBJ) tests/test_cce_model_save.c
	$(CC) $(CFLAGS) $(CUDA_CFLAGS) -o $@ $(CCE) $(CCE_CUDA_OBJ) tests/test_cce_model_save.c $(LDFLAGS) $(CUDA_LDFLAGS)
	./cce_model_test
```

- [ ] **Step 3: Run to verify it fails**

Run: `make cce_model_test`
Expected: builds, runs, **FAIL** — "model save ok" fails (current `cce_model_save` returns `CCE_ERR_UNSUPPORTED`), and the load/infer-parity checks fail.

- [ ] **Step 4: Checkpoint**

The red test is the checkpoint for this task. Do not proceed until it builds and fails for the expected reason (save returns unsupported), not a compile error.

---

### Task A2: Internal header + io module + implement `cce_model_save`

**Files:**
- Create: `src/cce/cce_model_internal.h`
- Create: `src/cce/cce_model_io.h`, `src/cce/cce_model_io.c`
- Modify: `src/cce/cce_model.c` (include internal header; new fields/defaults; `save` delegates; `destroy` frees owned)
- Modify: `Makefile` (`CCE` source set)

- [ ] **Step 1: Create `src/cce/cce_model_internal.h`** (move the struct out of `cce_model.c`)

```c
#ifndef CCE_MODEL_INTERNAL_H
#define CCE_MODEL_INTERNAL_H

#include "../../include/cce/cce_forest.h"
#include "../../include/cce/cce_learn.h"
#include "../../include/cce/cce_router.h"

struct cce_model {
    char name[64];
    cce_forest* forests[16];
    char        forest_names[16][64];
    int num_forests;
    cce_scheduler* scheduler;
    cce_diff_mode_t diff_mode;
    cce_router    router;

    /* training knobs (default to today's hardcoded behavior) */
    int   classify;             /* 0=MSE, 1=softmax cross-entropy */
    float goodness_threshold;   /* default 0.7 */
    float dfa_strength;         /* 0 => leave learner default */
    float grad_clip;            /* 0 => disabled */

    /* ownership for loaded models */
    int owns_forests;
    int owns_scheduler;
};

#endif
```

- [ ] **Step 2: Edit `src/cce/cce_model.c`** — replace the inline `struct cce_model { ... };` (lines 13-22) with an include, and update create/destroy:

Replace the struct block with:
```c
#include "cce_model_internal.h"
```
(Keep all other existing includes.)

In `cce_model_create`, after `cce_router_init(...)`, set defaults:
```c
    m->classify = 0;
    m->goodness_threshold = 0.7f;
    m->dfa_strength = 0.0f;
    m->grad_clip = 0.0f;
    m->owns_forests = 0;
    m->owns_scheduler = 0;
```

Replace `cce_model_destroy` with:
```c
void cce_model_destroy(cce_model* model) {
    if (!model) return;
    if (model->owns_forests) {
        for (int i = 0; i < model->num_forests; ++i) {
            if (model->forests[i]) cce_forest_close(model->forests[i]);
        }
    }
    if (model->owns_scheduler && model->scheduler) {
        free(model->scheduler);
    }
    free(model);
}
```

Replace the `cce_model_save` stub body with a delegation:
```c
#include "cce_model_io.h"
/* ... */
cce_result cce_model_save(cce_model* model, const char* path) {
    return cce_model_io_save(model, path);
}
cce_result cce_model_load(cce_model* model, const char* path) {
    return cce_model_io_load(model, path);
}
```
(Place the `#include "cce_model_io.h"` with the other includes at the top.)

- [ ] **Step 3: Create `src/cce/cce_model_io.h`**

```c
#ifndef CCE_MODEL_IO_H
#define CCE_MODEL_IO_H

#include "../../include/cce/cce_defs.h"
#include "../../include/cce/cce_model.h"

cce_result cce_model_io_save(cce_model* model, const char* path);
cce_result cce_model_io_load(cce_model* model, const char* path);

#endif
```

- [ ] **Step 4: Create `src/cce/cce_model_io.c`** (writer half now; reader added in A3)

```c
#include "cce_model_io.h"
#include "cce_model_internal.h"
#include "../../include/cce/cce_archive.h"
#include "../../include/cce/cce_cascade.h"
#include "../../include/cce/cce_forest.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* ---- growable byte buffer ---- */
typedef struct { unsigned char* p; size_t len, cap; } buf_t;
static int buf_reserve(buf_t* b, size_t extra) {
    if (b->len + extra <= b->cap) return 1;
    size_t nc = b->cap ? b->cap * 2 : 4096;
    while (nc < b->len + extra) nc *= 2;
    unsigned char* np = (unsigned char*)realloc(b->p, nc);
    if (!np) return 0;
    b->p = np; b->cap = nc; return 1;
}
static int buf_put(buf_t* b, const void* d, size_t n) {
    if (!buf_reserve(b, n)) return 0;
    memcpy(b->p + b->len, d, n); b->len += n; return 1;
}
static int put_i32(buf_t* b, int32_t v)  { return buf_put(b, &v, 4); }
static int put_f32(buf_t* b, float v)    { return buf_put(b, &v, 4); }
static int put_u64(buf_t* b, uint64_t v) { return buf_put(b, &v, 8); }
static int put_fixed(buf_t* b, const char* s, size_t n) {
    char tmp[512]; if (n > sizeof(tmp)) return 0;
    memset(tmp, 0, n); if (s) strncpy(tmp, s, n - 1);
    return buf_put(b, tmp, n);
}

cce_result cce_model_io_save(cce_model* m, const char* path) {
    if (!m || !path) return CCE_ERR_INVALID_ARG;

    remove(path);                          /* OPEN_ALWAYS appends; start fresh */
    cce_archive* arc = NULL;
    if (cce_archive_open(&arc, path) != CCE_OK) return CCE_ERR_IO;

    /* Pass 1: persist each branch cascade into the bundle, remember offsets.
       Flat array sized to total branch count (forests capped at 16 by the model,
       but a forest may hold many branches -- no fixed per-forest cap). */
    size_t total_branches = 0;
    for (int fi = 0; fi < m->num_forests; ++fi)
        total_branches += (size_t)m->forests[fi]->num_branches;
    size_t* offs = total_branches ? (size_t*)malloc(total_branches * sizeof(size_t)) : NULL;
    if (total_branches && !offs) { cce_archive_close(arc); return CCE_ERR_OOM; }
    size_t k = 0;
    for (int fi = 0; fi < m->num_forests; ++fi) {
        cce_forest* f = m->forests[fi];
        for (int bi = 0; bi < f->num_branches; ++bi) {
            if (cce_forest_promote_to_hot(f, bi) != CCE_OK || !f->branches[bi].cascade) {
                free(offs); cce_archive_close(arc); return CCE_ERR_IO;
            }
            char sec[64]; snprintf(sec, sizeof(sec), "f%d_b%d", fi, bi);
            size_t off = 0;
            if (cce_cascade_save_to_archive(f->branches[bi].cascade, arc, sec, &off) != CCE_OK) {
                free(offs); cce_archive_close(arc); return CCE_ERR_IO;
            }
            offs[k++] = off;
        }
    }

    /* Pass 2: build manifest. */
    buf_t b = {0};
    buf_put(&b, "CMDL", 4);
    put_i32(&b, 1);                        /* version */
    put_fixed(&b, m->name, 64);
    put_i32(&b, (int)m->diff_mode);
    put_i32(&b, m->classify);
    put_f32(&b, m->goodness_threshold);
    put_f32(&b, m->dfa_strength);
    put_f32(&b, m->grad_clip);
    int has_sched = m->scheduler ? 1 : 0;
    put_i32(&b, has_sched);
    if (has_sched) {
        cce_scheduler* s = m->scheduler;
        put_i32(&b, (int)s->type);     put_f32(&b, s->initial_lr);
        put_i32(&b, s->warmup_epochs); put_f32(&b, s->decay_factor);
        put_i32(&b, s->step_size);     put_f32(&b, s->plateau_factor);
        put_i32(&b, s->plateau_patience);
        put_f32(&b, s->current_lr);    put_f32(&b, s->best_loss);
        put_i32(&b, s->patience_counter); put_i32(&b, s->total_steps);
    }
    put_i32(&b, m->num_forests);

    k = 0;
    for (int fi = 0; fi < m->num_forests; ++fi) {
        cce_forest* f = m->forests[fi];
        put_fixed(&b, m->forest_names[fi], 64);
        put_i32(&b, f->num_branches);
        put_i32(&b, f->centroid_dim);
        put_i32(&b, f->sealed);
        put_i32(&b, (int)f->diff_mode);
        put_i32(&b, f->default_exact_tail_length);
        for (int bi = 0; bi < f->num_branches; ++bi) {
            cce_branch* br = &f->branches[bi];
            put_fixed(&b, br->name, 64);
            buf_put(&b, br->centroid, sizeof(br->centroid));   /* float[32] */
            put_i32(&b, br->centroid_dim);
            put_i32(&b, (int)br->tier);
            put_i32(&b, (int)br->diff_mode);
            put_i32(&b, br->exact_tail_length);
            put_i32(&b, br->num_connections);
            buf_put(&b, br->conn_names, sizeof(br->conn_names)); /* char[8][64] */
            buf_put(&b, br->conn_types, sizeof(br->conn_types)); /* int[8] */
            put_u64(&b, (uint64_t)offs[k++]);
        }
    }

    size_t moff = 0;
    cce_result rc = cce_archive_append_section(arc, "MODEL_MANIFEST", b.p, b.len, &moff);
    free(b.p);
    free(offs);
    cce_archive_close(arc);                /* flushes the directory */
    return rc == CCE_OK ? CCE_OK : CCE_ERR_IO;
}

/* cce_model_io_load implemented in Task A3 */
```

- [ ] **Step 5: Register the io source in the Makefile**

In `Makefile`, after line 52 (`CCE_MODEL := src/cce/cce_model.c`) add:
```make
CCE_MODEL_IO := src/cce/cce_model_io.c
```
Then in the `CCE :=` line (line 77), append `$(CCE_MODEL_IO)` (e.g. after `$(CCE_MODEL)`).

- [ ] **Step 6: Build to verify it compiles** (load still stubbed → round-trip still red)

Run: `make cce_model_test`
Expected: compiles cleanly; "model save ok" now **passes**, but "model load ok" / parity checks **fail** (load returns `CCE_ERR_UNSUPPORTED`). This confirms save works in isolation.

- [ ] **Step 7: Checkpoint** — save-only green, load still red, no compile errors.

---

### Task A3: Implement `cce_model_io_load` → round-trip passes

**Files:**
- Modify: `src/cce/cce_model_io.c` (append the reader + a HOT-forest builder)

- [ ] **Step 1: Append the reader to `src/cce/cce_model_io.c`**

```c
/* ---- reader over a fixed buffer ---- */
typedef struct { const unsigned char* p; size_t len, pos; } rdr_t;
static int rd(rdr_t* r, void* out, size_t n) {
    if (r->pos + n > r->len) return 0;
    memcpy(out, r->p + r->pos, n); r->pos += n; return 1;
}
static int rd_i32(rdr_t* r, int32_t* v) { return rd(r, v, 4); }
static int rd_f32(rdr_t* r, float* v)   { return rd(r, v, 4); }
static int rd_u64(rdr_t* r, uint64_t* v){ return rd(r, v, 8); }

cce_result cce_model_io_load(cce_model* m, const char* path) {
    if (!m || !path) return CCE_ERR_INVALID_ARG;

    cce_archive* arc = NULL;
    if (cce_archive_open(&arc, path) != CCE_OK) return CCE_ERR_IO;

    size_t moff = 0, msize = 0;
    if (cce_archive_find_section(arc, "MODEL_MANIFEST", &moff, &msize) != CCE_OK || msize == 0) {
        cce_archive_close(arc); return CCE_ERR_NOT_FOUND;
    }
    unsigned char* buf = (unsigned char*)malloc(msize);
    if (!buf) { cce_archive_close(arc); return CCE_ERR_OOM; }
    if (cce_archive_read_raw(arc, moff, buf, msize) != CCE_OK) {
        free(buf); cce_archive_close(arc); return CCE_ERR_IO;
    }

    rdr_t r = { buf, msize, 0 };
    char magic[4]; int32_t version = 0;
    if (!rd(&r, magic, 4) || memcmp(magic, "CMDL", 4) != 0 ||
        !rd_i32(&r, &version) || version != 1) {
        free(buf); cce_archive_close(arc); return CCE_ERR_UNSUPPORTED;
    }

    char name[64]; rd(&r, name, 64);
    strncpy(m->name, name, sizeof(m->name) - 1);
    int32_t dm = 0; rd_i32(&r, &dm); m->diff_mode = (cce_diff_mode_t)dm;
    rd_i32(&r, &m->classify);
    rd_f32(&r, &m->goodness_threshold);
    rd_f32(&r, &m->dfa_strength);
    rd_f32(&r, &m->grad_clip);

    int32_t has_sched = 0; rd_i32(&r, &has_sched);
    if (has_sched) {
        cce_scheduler* s = (cce_scheduler*)calloc(1, sizeof(cce_scheduler));
        if (!s) { free(buf); cce_archive_close(arc); return CCE_ERR_OOM; }
        int32_t t;
        rd_i32(&r, &t); s->type = (cce_sched_type_t)t; rd_f32(&r, &s->initial_lr);
        rd_i32(&r, &s->warmup_epochs); rd_f32(&r, &s->decay_factor);
        rd_i32(&r, &s->step_size);     rd_f32(&r, &s->plateau_factor);
        rd_i32(&r, &s->plateau_patience);
        rd_f32(&r, &s->current_lr);    rd_f32(&r, &s->best_loss);
        rd_i32(&r, &s->patience_counter); rd_i32(&r, &s->total_steps);
        m->scheduler = s; m->owns_scheduler = 1;
    }

    int32_t num_forests = 0; rd_i32(&r, &num_forests);
    if (num_forests < 0 || num_forests > 16) {
        free(buf); cce_archive_close(arc); return CCE_ERR_UNSUPPORTED;
    }
    m->num_forests = 0;

    for (int fi = 0; fi < num_forests; ++fi) {
        char fname[64]; rd(&r, fname, 64);
        int32_t nb, cdim, sealed, fdm, etl;
        rd_i32(&r, &nb); rd_i32(&r, &cdim); rd_i32(&r, &sealed);
        rd_i32(&r, &fdm); rd_i32(&r, &etl);

        int maxb = nb > 0 ? nb : 1;
        cce_forest* f = (cce_forest*)calloc(1, sizeof(cce_forest));
        f->archive = NULL;                 /* HOT-only, archive-less */
        f->max_branches = maxb;
        f->branches = (cce_branch*)calloc(maxb, sizeof(cce_branch));
        f->centroid_dim = cdim;
        f->centroids = (float*)calloc((size_t)maxb * (cdim > 0 ? cdim : 1), sizeof(float));
        f->num_branches = 0;
        f->sealed = 0;
        f->diff_mode = (cce_diff_mode_t)fdm;
        f->default_exact_tail_length = etl;

        for (int bi = 0; bi < nb; ++bi) {
            cce_branch* br = &f->branches[bi];
            char bname[64]; rd(&r, bname, 64);
            rd(&r, br->centroid, sizeof(br->centroid));
            int32_t bcdim, tier, bdm, betl, nconn;
            rd_i32(&r, &bcdim); rd_i32(&r, &tier); rd_i32(&r, &bdm);
            rd_i32(&r, &betl);  rd_i32(&r, &nconn);
            rd(&r, br->conn_names, sizeof(br->conn_names));
            rd(&r, br->conn_types, sizeof(br->conn_types));
            uint64_t coff = 0; rd_u64(&r, &coff);

            br->cascade = (cce_cascade*)malloc(sizeof(cce_cascade));
            memset(br->cascade, 0, sizeof(cce_cascade));
            if (cce_cascade_load_from_archive(br->cascade, arc, (size_t)coff) != CCE_OK) {
                free(br->cascade); br->cascade = NULL;
                cce_forest_close(f); free(buf); cce_archive_close(arc);
                return CCE_ERR_IO;
            }
            strncpy(br->name, bname, sizeof(br->name) - 1);
            br->centroid_dim = bcdim;
            br->tier = CCE_TIER_HOT;
            br->is_view = 0;
            br->persisted = 0;
            br->archive_offset = 0;
            br->diff_mode = (cce_diff_mode_t)bdm;
            br->exact_tail_length = betl;
            br->num_connections = nconn;
            for (int d = 0; d < cdim && d < 32; ++d)
                f->centroids[(size_t)bi * cdim + d] = br->centroid[d];
            f->num_branches++;
        }

        cce_model_add_forest(m, f, fname);
    }

    m->owns_forests = 1;                    /* loaded model owns its forests */
    free(buf);
    cce_archive_close(arc);                 /* all data copied into HOT RAM */
    return CCE_OK;
}
```

- [ ] **Step 2: Run the round-trip test**

Run: `make cce_model_test`
Expected: **OK** — all checks pass (label + confidence parity after load), clean exit (no double-free).

- [ ] **Step 3: Run the broader suite to confirm no regression**

Run: `make cce_smoke && make cce_view && make forest_view`
Expected: each prints its `... OK` line and exits 0 (model/forest changes didn't break cascade/forest behavior).

- [ ] **Step 4: Checkpoint** — `cce_model_test` green; smoke/view/forest_view green.

---

### Task A4: `.NET` Save/Load + xUnit test

**Files:**
- Modify: `dotnet/Cce/CceNative.cs` (P/Invoke for save/load)
- Modify: `dotnet/Cce/CceModel.cs` (`Save`, static `Load`)
- Create/Modify: `dotnet/Cce.Tests/SerializationTests.cs`

- [ ] **Step 1: Add P/Invoke** — in `CceNative.cs`, in the `// Model` region, add:

```csharp
    [LibraryImport(LibraryName, EntryPoint = "cce_model_save")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial CceResult CceModelSave(IntPtr model, [MarshalAs(UnmanagedType.LPStr)] string path);

    [LibraryImport(LibraryName, EntryPoint = "cce_model_load")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial CceResult CceModelLoad(IntPtr model, [MarshalAs(UnmanagedType.LPStr)] string path);
```

- [ ] **Step 2: Add `Save`/`Load` to `CceModel.cs`** (before `Dispose`):

```csharp
    /// <summary>Saves the entire model (forests + scheduler + config) to one self-contained .cce bundle.</summary>
    public void Save(string path)
    {
        ThrowIfDisposed();
        if (string.IsNullOrWhiteSpace(path)) throw new ArgumentException("Path required", nameof(path));
        var rc = CceNative.CceModelSave(_handle, path);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"Save failed: {rc}");
    }

    /// <summary>Loads a model previously written by <see cref="Save"/>. Returns a fully RAM-resident model that owns its forests.</summary>
    public static CceModel Load(string path)
    {
        if (string.IsNullOrWhiteSpace(path)) throw new ArgumentException("Path required", nameof(path));
        var model = new CceModel("loaded");
        var rc = CceNative.CceModelLoad(model._handle, path);
        if (rc != CceNative.CceResult.Ok)
        {
            model.Dispose();
            throw new InvalidOperationException($"Load failed: {rc}");
        }
        return model;
    }
```

- [ ] **Step 3: Write the xUnit test** — create `dotnet/Cce.Tests/SerializationTests.cs`:

```csharp
using System;
using System.IO;
using CNET.Cce;
using Xunit;

namespace CNET.Cce.Tests;

public class SerializationTests
{
    [Fact]
    public void EmptyModel_Save_Then_Load_RoundTrips_Metadata()
    {
        string path = Path.Combine(Path.GetTempPath(), $"cce_model_{Guid.NewGuid():N}.cce");
        try
        {
            using (var model = new CceModel("roundtrip"))
            {
                model.SetDiffMode(CceDiffMode.Hybrid);
                using var sched = new CceScheduler(new CceSchedulerConfig(CceSchedulerType.Warmup, 0.02f));
                model.SetScheduler(sched);
                model.Save(path);
            }

            Assert.True(File.Exists(path));

            using var loaded = CceModel.Load(path);   // must not throw; 0 forests is valid
            Assert.NotNull(loaded);
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }

    [Fact]
    public void Load_Of_Missing_File_Throws()
    {
        Assert.Throws<InvalidOperationException>(() =>
            CceModel.Load(Path.Combine(Path.GetTempPath(), "does_not_exist_cce_bundle.cce")));
    }
}
```

- [ ] **Step 4: Rebuild native + run**

Run: `make cce_dll`
Then: `dotnet test dotnet/Cce.Tests/Cce.Tests.csproj`
Expected: build succeeds; `SerializationTests` pass alongside the existing `ApiSurfaceTests`.

- [ ] **Step 5: Checkpoint** — `cce_model_test` green (C), `dotnet test` green (.NET).

---

# Phase B — Model abstraction (safe-handle + composition)

### Task B1: C forest exports + branch accessors

**Files:**
- Modify: `include/cce/cce_forest.h` (add `CCE_API`, declare accessors)
- Modify: `src/cce/cce_forest.c` (implement accessors)
- Modify: `tests/test_cce_model_save.c` (add accessor checks) — reuse the existing target

- [ ] **Step 1: Add the failing accessor checks** to `tests/test_cce_model_save.c`, just before the final `printf` summary (the model `m` is still alive there; reach its forest via a fresh build to stay independent):

```c
    /* --- branch accessors --- */
    {
        const char* fp = "test_accessor_forest.cce";
        cce_cascade c2; cce_cascade_init(&c2, 4);
        cce_block bb0, bb1;
        cce_block_init_linear(&bb0, IN, HID, 0.01f);
        cce_block_init_linear(&bb1, HID, OUT, 0.01f); bb1.type = CCE_BLOCK_LINEAR_HEAD;
        cce_cascade_append(&c2, &bb0); cce_cascade_append(&c2, &bb1);
        cce_forest* f2 = NULL; remove(fp);
        cce_forest_open(&f2, fp, 4);
        cce_forest_add_branch(f2, &c2, "alpha");
        CHECK(cce_forest_branch_count(f2) == 1, "branch_count == 1");
        char nm[64] = {0};
        CHECK(cce_forest_branch_name(f2, 0, nm, sizeof(nm)) == CCE_OK, "branch_name ok");
        CHECK(strcmp(nm, "alpha") == 0, "branch_name == alpha");
        cce_forest_close(f2);
        remove(fp);
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `make cce_model_test`
Expected: **compile/link error** — `cce_forest_branch_count` / `cce_forest_branch_name` undefined. (Red.)

- [ ] **Step 3: Declare in `include/cce/cce_forest.h`**

Add the `CCE_API` macro block near the top (mirror `cce_learn.h` lines 78-87) **before** the function declarations:
```c
#ifdef CCE_BUILD_DLL
#ifdef _WIN32
#define CCE_API __declspec(dllexport)
#else
#define CCE_API __attribute__((visibility("default")))
#endif
#else
#define CCE_API
#endif
```
Prefix these existing declarations with `CCE_API`: `cce_forest_open`, `cce_forest_close`, `cce_forest_connect`, `cce_forest_merge`, `cce_forest_get_connections`, `cce_forest_set_diff_mode`, `cce_forest_set_branch_diff_mode`, `cce_forest_set_branch_exact_tail_length`. Then add:
```c
/* Number of branches currently in the forest. */
CCE_API int cce_forest_branch_count(const cce_forest* forest);

/* Copy branch `idx`'s name into buf (NUL-terminated, truncated to len). */
CCE_API cce_result cce_forest_branch_name(const cce_forest* forest, int idx, char* buf, int len);
```

- [ ] **Step 4: Implement in `src/cce/cce_forest.c`** (append near the other accessors):

```c
int cce_forest_branch_count(const cce_forest* f) {
    return f ? f->num_branches : 0;
}

cce_result cce_forest_branch_name(const cce_forest* f, int idx, char* buf, int len) {
    if (!f || !buf || len <= 0 || idx < 0 || idx >= f->num_branches) return CCE_ERR_INVALID_ARG;
    strncpy(buf, f->branches[idx].name, (size_t)len - 1);
    buf[len - 1] = '\0';
    return CCE_OK;
}
```

- [ ] **Step 5: Run to verify pass**

Run: `make cce_model_test`
Expected: **OK** — accessor checks pass, round-trip still green.

- [ ] **Step 6: Checkpoint** — `cce_model_test` green; rebuild `make cce_dll` succeeds (confirms exports compile under `CCE_BUILD_DLL`).

---

### Task B2: `.NET` `CceForest` safe-handle + composition + `Add(CceForest)`

**Files:**
- Modify: `dotnet/Cce/CceEnums.cs` (`CceConnectionType`)
- Modify: `dotnet/Cce/CceNative.cs` (forest P/Invoke)
- Create: `dotnet/Cce/CceForest.cs`
- Modify: `dotnet/Cce/CceModel.cs` (`Add(CceForest)`, `[Obsolete]` raw)
- Create: `dotnet/Cce.Tests/ForestApiTests.cs`

- [ ] **Step 1: Add `CceConnectionType`** to `CceEnums.cs`:

```csharp
/// <summary>Typed composition relation between two branches (matches C conn_types).</summary>
public enum CceConnectionType
{
    Sub = 0,
    Refines = 1,
    Composes = 2,
    Specializes = 3,
}
```

- [ ] **Step 2: Add forest P/Invoke** to `CceNative.cs` (new region):

```csharp
    // ============================================
    // Forest (model-definition / composition)
    // ============================================

    [LibraryImport(LibraryName, EntryPoint = "cce_forest_open")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial CceResult CceForestOpen(out IntPtr forest, [MarshalAs(UnmanagedType.LPStr)] string archivePath, int maxBranches);

    [LibraryImport(LibraryName, EntryPoint = "cce_forest_close")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial void CceForestClose(IntPtr forest);

    [LibraryImport(LibraryName, EntryPoint = "cce_forest_connect")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial CceResult CceForestConnect(IntPtr forest, int fromIdx, int toIdx, int connType);

    [LibraryImport(LibraryName, EntryPoint = "cce_forest_merge")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int CceForestMerge(IntPtr dst, IntPtr src, [MarshalAs(UnmanagedType.LPStr)] string namePrefix);

    [LibraryImport(LibraryName, EntryPoint = "cce_forest_set_diff_mode")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial CceResult CceForestSetDiffMode(IntPtr forest, CceDiffMode mode);

    [LibraryImport(LibraryName, EntryPoint = "cce_forest_branch_count")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial int CceForestBranchCount(IntPtr forest);

    [LibraryImport(LibraryName, EntryPoint = "cce_forest_branch_name")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial CceResult CceForestBranchName(IntPtr forest, int idx, byte[] buf, int len);
```

- [ ] **Step 3: Create `dotnet/Cce/CceForest.cs`**

```csharp
using System;
using System.Runtime.InteropServices;
using System.Text;
using CNET.Cce.Interop;

namespace CNET.Cce;

/// <summary>
/// Safe-handle wrapper over a native cce_forest*. A forest is the model-definition unit:
/// a set of archive-backed branches plus typed composition relations between them.
/// This replaces passing raw IntPtr into <see cref="CceModel.Add(CceForest, string)"/>.
/// </summary>
public sealed class CceForest : SafeHandle
{
    private CceForest() : base(IntPtr.Zero, ownsHandle: true) { }

    public override bool IsInvalid => handle == IntPtr.Zero;

    internal IntPtr DangerousHandle => handle;

    /// <summary>Opens (or creates) a forest backed by a .cce archive on disk.</summary>
    public static CceForest Open(string archivePath, int maxBranches = 64)
    {
        if (string.IsNullOrWhiteSpace(archivePath)) throw new ArgumentException("Path required", nameof(archivePath));
        var rc = CceNative.CceForestOpen(out IntPtr h, archivePath, maxBranches);
        if (rc != CceNative.CceResult.Ok || h == IntPtr.Zero)
            throw new InvalidOperationException($"CceForest.Open failed: {rc}");
        var f = new CceForest();
        f.SetHandle(h);
        return f;
    }

    public int BranchCount => CceNative.CceForestBranchCount(handle);

    public string BranchName(int index)
    {
        var buf = new byte[64];
        var rc = CceNative.CceForestBranchName(handle, index, buf, buf.Length);
        if (rc != CceNative.CceResult.Ok)
            throw new ArgumentOutOfRangeException(nameof(index), $"branch_name failed: {rc}");
        int n = Array.IndexOf(buf, (byte)0);
        return Encoding.ASCII.GetString(buf, 0, n < 0 ? buf.Length : n);
    }

    /// <summary>Connect two branches with a typed composition relation.</summary>
    public void Connect(int fromBranch, int toBranch, CceConnectionType type)
    {
        var rc = CceNative.CceForestConnect(handle, fromBranch, toBranch, (int)type);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"Connect failed: {rc}");
    }

    /// <summary>Merge another forest's branches into this one (with an optional name prefix).</summary>
    public int Merge(CceForest src, string namePrefix = "")
    {
        ArgumentNullException.ThrowIfNull(src);
        int n = CceNative.CceForestMerge(handle, src.handle, namePrefix ?? "");
        if (n < 0) throw new InvalidOperationException($"Merge failed: {n}");
        return n;
    }

    public void SetDiffMode(CceDiffMode mode)
    {
        var rc = CceNative.CceForestSetDiffMode(handle, mode);
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"SetDiffMode failed: {rc}");
    }

    protected override bool ReleaseHandle()
    {
        CceNative.CceForestClose(handle);
        return true;
    }
}
```

- [ ] **Step 4: Add `Add(CceForest)` and obsolete the raw path** in `CceModel.cs`

Add above the existing `AddForest`:
```csharp
    /// <summary>Adds a forest as a named module of the model. Preferred over the raw-pointer overload.</summary>
    public void Add(CceForest forest, string name)
    {
        ThrowIfDisposed();
        ArgumentNullException.ThrowIfNull(forest);
        if (forest.IsInvalid) throw new ArgumentException("Forest handle is invalid", nameof(forest));
        var rc = CceNative.CceModelAddForest(_handle, forest.DangerousHandle, name ?? "unnamed");
        if (rc != CceNative.CceResult.Ok)
            throw new InvalidOperationException($"Add failed: {rc}");
    }
```
Annotate the existing method:
```csharp
    [Obsolete("Use Add(CceForest, name). Raw IntPtr is for advanced/interop scenarios only.")]
    public void AddForest(IntPtr nativeForestHandle, string forestName)
```
(Leave its body unchanged.)

- [ ] **Step 5: Write `dotnet/Cce.Tests/ForestApiTests.cs`**

```csharp
using System;
using System.IO;
using CNET.Cce;
using Xunit;

namespace CNET.Cce.Tests;

public class ForestApiTests
{
    [Fact]
    public void Forest_Open_Creates_Empty_Forest_With_Zero_Branches()
    {
        string path = Path.Combine(Path.GetTempPath(), $"cce_forest_{Guid.NewGuid():N}.cce");
        try
        {
            using var forest = CceForest.Open(path, maxBranches: 8);
            Assert.False(forest.IsInvalid);
            Assert.Equal(0, forest.BranchCount);

            // A freshly-opened forest can be added to a model without raw IntPtr.
            using var model = new CceModel("compose-test");
            model.Add(forest, "subforest_a");
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }

    [Fact]
    public void Forest_SetDiffMode_Does_Not_Throw()
    {
        string path = Path.Combine(Path.GetTempPath(), $"cce_forest_{Guid.NewGuid():N}.cce");
        try
        {
            using var forest = CceForest.Open(path);
            forest.SetDiffMode(CceDiffMode.Hybrid);   // exercises an exported entry point
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }
}
```

- [ ] **Step 6: Rebuild native + run**

Run: `make cce_dll`
Then: `dotnet test dotnet/Cce.Tests/Cce.Tests.csproj`
Expected: green. (If an entry point throws `EntryPointNotFound`, a `CCE_API` prefix was missed in Task B1 — fix and rebuild `cce.dll`.)

- [ ] **Step 7: Checkpoint** — C suite green; `dotnet test` green including `ForestApiTests`.

---

# Phase C — Extras

### Task C1: C loss-type + real loss + learner knobs

**Files:**
- Modify: `include/cce/cce_model.h` (declarations)
- Modify: `src/cce/cce_model.c` (setters + real loss in `dispatch_train_sample`)
- Modify: `tests/test_cce_model_save.c` (loss checks)

- [ ] **Step 1: Add failing loss checks** to `tests/test_cce_model_save.c` (before the summary). This trains a tiny learnable table and asserts the returned loss is real (finite, ≥0) and the CE/MSE paths differ:

```c
    /* --- real loss + loss-type wiring --- */
    {
        const char* fp = "test_loss_forest.cce";
        cce_cascade c3; cce_cascade_init(&c3, 4);
        cce_block d0, d1;
        cce_block_init_linear(&d0, IN, HID, 0.05f);
        cce_block_init_linear(&d1, HID, OUT, 0.05f); d1.type = CCE_BLOCK_LINEAR_HEAD;
        cce_cascade_append(&c3, &d0); cce_cascade_append(&c3, &d1);
        cce_forest* f3 = NULL; remove(fp); cce_forest_open(&f3, fp, 4);
        cce_forest_add_branch(f3, &c3, "b");
        cce_model* lm = NULL; cce_model_create(&lm, "lossmodel");
        cce_model_add_forest(lm, f3, "f");

        /* tiny dataset: 4 samples, one-hot targets */
        float xs[4*8] = {0}; float ys[4*3] = {0};
        for (int i = 0; i < 4; i++) { xs[i*8 + (i%8)] = 1.0f; ys[i*3 + (i%3)] = 1.0f; }
        cce_dataset* ds = NULL;
        cce_dataset_from_arrays(&ds, xs, ys, 4, 8, 3, 2);

        cce_model_set_loss(lm, 0);                 /* MSE */
        double mse = cce_model_train(lm, ds, 5, 0.0f);
        CHECK(mse == mse && mse >= 0.0, "MSE loss is finite and >= 0");

        cce_dataset_reset(ds);
        cce_model_set_loss(lm, 1);                 /* cross-entropy */
        double ce = cce_model_train(lm, ds, 5, 0.0f);
        CHECK(ce == ce && ce >= 0.0, "CE loss is finite and >= 0");
        CHECK(fabs(ce - mse) > 1e-9, "CE and MSE produce different loss (paths wired)");

        cce_model_set_learn_params(lm, 0.6f, 0.5f, 1.0f);  /* must not crash */
        cce_dataset_reset(ds);
        double l2 = cce_model_train(lm, ds, 2, 0.0f);
        CHECK(l2 == l2 && l2 >= 0.0, "train with custom knobs is finite");

        cce_dataset_destroy(ds);
        cce_model_destroy(lm);
        remove(fp);
    }
```
Add the dataset include at the top of the test file: `#include "../include/cce/cce_dataset.h"`.

- [ ] **Step 2: Run to verify it fails**

Run: `make cce_model_test`
Expected: **compile/link error** — `cce_model_set_loss` / `cce_model_set_learn_params` undefined. (Red.)

- [ ] **Step 3: Declare setters** in `include/cce/cce_model.h` (after `cce_model_set_diff_mode`):

```c
/* Output loss for training: 0 = MSE/regression, 1 = softmax cross-entropy. */
CCE_API cce_result cce_model_set_loss(cce_model* model, int classify);

/* Learner knobs. Pass <=0 to leave a knob at its default.
   goodness_threshold default 0.7; dfa_strength 0 = learner default; grad_clip 0 = disabled. */
CCE_API cce_result cce_model_set_learn_params(cce_model* model,
                                              float goodness_threshold,
                                              float dfa_strength,
                                              float grad_clip);
```

- [ ] **Step 4: Implement setters + real loss** in `src/cce/cce_model.c`

Add `#include <math.h>` at the top. Add the setters (near `cce_model_set_diff_mode`):
```c
cce_result cce_model_set_loss(cce_model* model, int classify) {
    if (!model) return CCE_ERR_INVALID_ARG;
    model->classify = classify ? 1 : 0;
    return CCE_OK;
}

cce_result cce_model_set_learn_params(cce_model* model, float goodness_threshold,
                                      float dfa_strength, float grad_clip) {
    if (!model) return CCE_ERR_INVALID_ARG;
    if (goodness_threshold > 0.0f) model->goodness_threshold = goodness_threshold;
    if (dfa_strength > 0.0f)       model->dfa_strength = dfa_strength;
    if (grad_clip > 0.0f)          model->grad_clip = grad_clip;
    return CCE_OK;
}
```

Add a loss helper above `dispatch_train_sample`:
```c
static double cce_compute_loss(int classify, const float* pred, const float* tgt, int n) {
    if (classify) {
        float maxv = pred[0];
        for (int i = 1; i < n; i++) if (pred[i] > maxv) maxv = pred[i];
        double sum = 0.0;
        for (int i = 0; i < n; i++) sum += exp((double)pred[i] - maxv);
        double ce = 0.0;
        for (int i = 0; i < n; i++) {
            double p = exp((double)pred[i] - maxv) / (sum + 1e-12);
            if (tgt[i] > 0.0f) ce -= (double)tgt[i] * log(p + 1e-12);
        }
        return ce;
    }
    double mse = 0.0;
    for (int i = 0; i < n; i++) { double d = (double)pred[i] - (double)tgt[i]; mse += d * d; }
    return 0.5 * mse;
}
```

In `dispatch_train_sample`, replace the learner setup + loss proxy. Change:
```c
    cce_learner learner;
    cce_learner_init(&learner, 0.7f);
    learner.diff_mode = dm;
```
to:
```c
    cce_learner learner;
    cce_learner_init(&learner, m->goodness_threshold);
    learner.diff_mode = dm;
    learner.classify = m->classify;
    if (m->dfa_strength > 0.0f) learner.dfa_strength = m->dfa_strength;
    if (m->grad_clip   > 0.0f) learner.grad_clip   = m->grad_clip;
```
Then, after the tensors `x`/`y` are built and before `cce_learner_adapt`, compute the real loss from a forward pass:
```c
    double sample_loss;
    cce_tensor pred; memset(&pred, 0, sizeof(pred));
    if (cce_cascade_forward(br->cascade, &x, &pred) == CCE_OK && pred.numel >= (size_t)out_dim) {
        sample_loss = cce_compute_loss(m->classify, pred.data, y.data, (int)out_dim);
    } else {
        sample_loss = 0.05 + 0.5 * (1.0 - br->cascade->goodness);  /* fallback proxy */
    }
    cce_tensor_free(&pred);

    cce_result rc = cce_learner_adapt(&learner, br->cascade, &x, &y, lr);
    if (rc != CCE_OK) sample_loss = 0.8;
```
Delete the old `double sample_loss = (rc == CCE_OK) ? ... : 0.8;` line (the loss is now computed above; keep the existing `cce_tensor_free(&x); cce_tensor_free(&y); return sample_loss;`).

- [ ] **Step 5: Run to verify pass**

Run: `make cce_model_test`
Expected: **OK** — loss checks pass (finite MSE/CE, different values, knobs don't crash), round-trip + accessors still green.

- [ ] **Step 6: Checkpoint** — `make cce_model_test` green; `make cce_smoke` green; `make cce_dll` rebuilds.

---

### Task C2: `.NET` loss/knobs config + metrics + callbacks + `Fit()`

**Files:**
- Modify: `dotnet/Cce/CceEnums.cs` (`CceLossType`, config fields)
- Modify: `dotnet/Cce/CceNative.cs` (P/Invoke for set_loss / set_learn_params)
- Create: `dotnet/Cce/CceCallbacks.cs`
- Create: `dotnet/Cce/CceMetrics.cs`
- Modify: `dotnet/Cce/CceModel.cs` (`Fit`; apply loss/knobs in `Train` + `Fit`)
- Create: `dotnet/Cce.Tests/TrainingExtrasTests.cs`

- [ ] **Step 1: Add `CceLossType` + config fields** to `CceEnums.cs`

```csharp
/// <summary>Output loss used during training.</summary>
public enum CceLossType
{
    MeanSquaredError = 0,
    CrossEntropy = 1,
}
```
Extend `CceTrainingConfig` with:
```csharp
    public CceLossType Loss { get; init; } = CceLossType.MeanSquaredError;
    /// <summary>Learner knobs. Null leaves the engine default.</summary>
    public float? GoodnessThreshold { get; init; }
    public float? DfaStrength { get; init; }
    public float? GradClip { get; init; }
```

- [ ] **Step 2: Add P/Invoke** to `CceNative.cs` (Model region):

```csharp
    [LibraryImport(LibraryName, EntryPoint = "cce_model_set_loss")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial CceResult CceModelSetLoss(IntPtr model, int classify);

    [LibraryImport(LibraryName, EntryPoint = "cce_model_set_learn_params")]
    [UnmanagedCallConv(CallConvs = new[] { typeof(CallConvCdecl) })]
    internal static partial CceResult CceModelSetLearnParams(IntPtr model, float goodness, float dfa, float gradClip);
```

- [ ] **Step 3: Create `dotnet/Cce/CceCallbacks.cs`**

```csharp
namespace CNET.Cce;

/// <summary>Per-epoch information passed to training callbacks.</summary>
public readonly record struct EpochInfo(int Epoch, double TrainLoss, float? ValAccuracy, float CurrentLr);

/// <summary>
/// Training callbacks for the managed <see cref="CceModel.Fit"/> loop.
/// <see cref="OnEpochEnd"/> returns false to request early stop (true/null continues).
/// </summary>
public sealed record CceCallbacks
{
    public System.Func<EpochInfo, bool>? OnEpochEnd { get; init; }
}
```

- [ ] **Step 4: Create `dotnet/Cce/CceMetrics.cs`**

```csharp
using System;

namespace CNET.Cce;

/// <summary>Host-side evaluation metrics (no native changes).</summary>
public static class CceMetrics
{
    /// <summary>Top-1 accuracy: predicted label (argmax) vs target argmax over the whole dataset.</summary>
    public static float Accuracy(CceModel model, CceDataset dataset)
    {
        ArgumentNullException.ThrowIfNull(model);
        ArgumentNullException.ThrowIfNull(dataset);

        int correct = 0, total = 0;
        dataset.Reset();
        while (dataset.NextBatch(out var batch))
        {
            var labels = new int[batch.BatchSize];
            var confs = new float[batch.BatchSize];
            model.InferBatch(batch, labels, confs);

            for (int s = 0; s < batch.BatchSize; s++)
            {
                var tgt = batch.Targets.Slice(s * batch.OutputDim, batch.OutputDim);
                int tgtLabel = ArgMax(tgt);
                if (labels[s] == tgtLabel) correct++;
                total++;
            }
        }
        return total > 0 ? (float)correct / total : 0f;
    }

    private static int ArgMax(ReadOnlySpan<float> v)
    {
        int best = 0; float bv = v.Length > 0 ? v[0] : 0f;
        for (int i = 1; i < v.Length; i++) if (v[i] > bv) { bv = v[i]; best = i; }
        return best;
    }
}
```
> Updated 2026-06-27: `TopK` is now provided. The model exposes raw routed cascade outputs through `cce_model_forward` / `cce_model_forward_batch` and the .NET `Forward` / `ForwardBatch` wrappers, so `CceMetrics.TopK` no longer depends on label-only inference.

- [ ] **Step 5: Add `Fit` + apply config knobs** in `CceModel.cs`

Add a private helper and the `Fit` method (after `Train`):
```csharp
    private void ApplyTrainingConfig(CceTrainingConfig config)
    {
        CceNative.CceModelSetLoss(_handle, (int)config.Loss);
        if (config.GoodnessThreshold is not null || config.DfaStrength is not null || config.GradClip is not null)
        {
            CceNative.CceModelSetLearnParams(
                _handle,
                config.GoodnessThreshold ?? 0f,
                config.DfaStrength ?? 0f,
                config.GradClip ?? 0f);
        }
        SetDiffMode(config.DiffMode);
    }

    /// <summary>
    /// Managed training loop with per-epoch callbacks and optional validation accuracy.
    /// Uses the per-batch path so callbacks can log / early-stop / checkpoint. The C
    /// <see cref="Train"/> remains the no-callback fast path.
    /// </summary>
    public double Fit(CceDataset train, CceTrainingConfig config,
                      CceCallbacks? callbacks = null, CceDataset? validation = null)
    {
        ThrowIfDisposed();
        ArgumentNullException.ThrowIfNull(train);
        config ??= new CceTrainingConfig();

        ApplyTrainingConfig(config);

        CceScheduler? sched = null;
        if (config.Scheduler is not null)
        {
            sched = new CceScheduler(config.Scheduler);
            SetScheduler(sched);
        }

        double lastLoss = double.PositiveInfinity;
        for (int epoch = 0; epoch < config.MaxEpochs; epoch++)
        {
            if (config.ShuffleEachEpoch) train.Shuffle();
            train.Reset();

            double sum = 0; int nb = 0;
            while (train.NextBatch(out var batch))
            {
                sum += TrainBatch(batch);
                nb++;
            }
            lastLoss = nb > 0 ? sum / nb : lastLoss;

            float? valAcc = validation is null ? null : CceMetrics.Accuracy(this, validation);
            float lr = sched?.CurrentLr ?? 0f;
            var info = new EpochInfo(epoch, lastLoss, valAcc, lr);

            bool keepGoing = callbacks?.OnEpochEnd?.Invoke(info) ?? true;
            if (!keepGoing) break;
            if (lastLoss < config.TargetLoss) break;
        }
        return lastLoss;
    }
```
Also call `ApplyTrainingConfig`-equivalent loss/knob wiring in the existing `Train` method: in `Train`, right after `config ??= new CceTrainingConfig();`, replace the lone `SetDiffMode(config.DiffMode);` with `ApplyTrainingConfig(config);`.

- [ ] **Step 6: Write `dotnet/Cce.Tests/TrainingExtrasTests.cs`**

```csharp
using System;
using System.Collections.Generic;
using CNET.Cce;
using Xunit;

namespace CNET.Cce.Tests;

public class TrainingExtrasTests
{
    [Fact]
    public void Fit_Invokes_OnEpochEnd_Each_Epoch()
    {
        var inputs = new float[16 * 4];
        var targets = new float[16 * 2];
        for (int i = 0; i < 16; i++) targets[i * 2 + (i % 2)] = 1f;

        using var ds = CceDataset.FromArrays(inputs, targets, 16, 4, 2, 8);
        using var model = new CceModel("fit-callback");   // empty model: TrainBatch returns -1 but does not throw

        var seen = new List<int>();
        var callbacks = new CceCallbacks
        {
            OnEpochEnd = info => { seen.Add(info.Epoch); return true; }
        };

        model.Fit(ds, new CceTrainingConfig { MaxEpochs = 3, ShuffleEachEpoch = false }, callbacks);

        Assert.Equal(new[] { 0, 1, 2 }, seen.ToArray());
    }

    [Fact]
    public void Fit_EarlyStops_When_Callback_Returns_False()
    {
        var inputs = new float[16 * 4];
        var targets = new float[16 * 2];
        for (int i = 0; i < 16; i++) targets[i * 2 + (i % 2)] = 1f;

        using var ds = CceDataset.FromArrays(inputs, targets, 16, 4, 2, 8);
        using var model = new CceModel("fit-earlystop");

        int epochs = 0;
        var callbacks = new CceCallbacks
        {
            OnEpochEnd = _ => { epochs++; return false; }   // stop after first
        };

        model.Fit(ds, new CceTrainingConfig { MaxEpochs = 10, ShuffleEachEpoch = false }, callbacks);

        Assert.Equal(1, epochs);
    }

    [Fact]
    public void TrainingConfig_LossType_Defaults_To_MSE()
    {
        var cfg = new CceTrainingConfig();
        Assert.Equal(CceLossType.MeanSquaredError, cfg.Loss);

        var ce = cfg with { Loss = CceLossType.CrossEntropy };
        Assert.Equal(CceLossType.CrossEntropy, ce.Loss);
    }
}
```

- [ ] **Step 7: Rebuild native + run full `.NET` suite**

Run: `make cce_dll`
Then: `dotnet test dotnet/Cce.Tests/Cce.Tests.csproj`
Expected: all tests green (`ApiSurfaceTests`, `SerializationTests`, `ForestApiTests`, `TrainingExtrasTests`).

- [ ] **Step 8: Final checkpoint** — run the full gate:
  - `make cce_model_test` → OK
  - `make cce_smoke && make cce_view && make forest_view` → each OK
  - `make cce_dll` → builds
  - `dotnet test dotnet/Cce.Tests/Cce.Tests.csproj` → all green

---

## Self-Review (completed during authoring)

- **Spec coverage:** serialization bundle (A1-A4) ✓; model abstraction safe-handle + composition (B1-B2) ✓; loss types + real loss (C1, C2 step 1) ✓; optimizer knobs (C1) ✓; metrics — **Accuracy ✓, TopK ✓** via raw routed outputs; callbacks + `Fit()` (C2) ✓.
- **Deviation from spec:** added `src/cce/cce_model_internal.h` (opaque-struct access) and scoped metrics to `Accuracy`. Both noted at the top and in C2 step 4.
- **Type consistency:** `cce_model_set_loss(int)`, `cce_model_set_learn_params(float,float,float)`, `cce_forest_branch_count/branch_name`, `CceForest.DangerousHandle`, `EpochInfo(int,double,float?,float)`, `CceCallbacks.OnEpochEnd : Func<EpochInfo,bool>` — used consistently across C and C# tasks.
- **No git:** every task ends in a build/test checkpoint, no commit steps.

## Post-implementation review (2026-06-27)

All 8 tasks landed; final gate green (C `cce_model_test` 15/0; `.NET` 13/13). A holistic review found and the following were **fixed**:
- **Load-path bounds + null-checks** (`cce_model_io.c`): `num_branches`/`centroid_dim` are bounded (≤65536 / ≤32) and every `calloc` is null-checked before use — `CceModel.Load` takes an arbitrary path, so manifest values are untrusted.
- **Partial-load leak** (`cce_model_io.c`): `owns_forests` is set to 1 *before* the forest loop so a mid-load failure still lets `cce_model_destroy` free already-adopted forests.
- **.NET forest keep-alive** (`CceModel.cs`): the model retains added `CceForest` wrappers in `_addedForests` so a forest can't be finalized (releasing its native handle) while the native model still holds its pointer; `Add` documents the lifetime contract.
- **`Fit` config nullability** (`CceModel.cs`): `config` is now `CceTrainingConfig?` to match `Train` and justify `config ??= new()`.

**Resolved follow-up (2026-06-27):** the `.NET` scheduler no longer passes a raw pointer into movable managed struct storage. `CceScheduler` now owns stable unmanaged native memory, applies full scheduler config after native init, and `CceModel` / `CceHandle` retain attached schedulers while native code references them. Covered by `.NET` scheduler tests (21/21).
```
