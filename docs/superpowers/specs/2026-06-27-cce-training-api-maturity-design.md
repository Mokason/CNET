# CCE Training API Maturity — Serialization, Model Abstraction, Extras

- **Date:** 2026-06-27
- **Status:** Approved design (pre-implementation)
- **Scope:** Three feature areas on the `.NET 10 + C` CCE training surface: (1) finish model serialization, (2) a model-definition abstraction above raw `IntPtr`, (3) a bounded set of "extras" (loss types, metrics, callbacks, optimizer knobs).
- **Note:** CNET has no git (`.git` was deleted). This document is written to disk only; **no git commands are run** and it is **not** committed.

## 1. Context & Problem

The CCE engine has a mature C core (frozen primitives, contracts, router, archive-backed forests with mmap views) and an early `.NET 10` training wrapper (`CceModel` + `CceDataset` + `CceScheduler`). An external comparison flagged three gaps versus PyTorch. We deliberately address only the gaps that **serve CCE's own thesis** (verifiable, composable, archive-backed artifacts), not PyTorch parity for its own sake.

Grounded current state:

- **Serialization** — `cce_model_save`/`cce_model_load` are pure stubs returning `CCE_ERR_UNSUPPORTED` ([cce_model.c:217](../../../src/cce/cce_model.c)), and the `.NET` side has no `Save`/`Load` at all. **But** the weight-level primitives already exist and work: `cce_cascade_save_to_archive` / `cce_cascade_load_from_archive` / `cce_cascade_view_from_archive` ([cce_cascade.h:49-65](../../../include/cce/cce_cascade.h)) over a named-section archive (`cce_archive_append_section` / `find_section` / `read_raw`). The gap is the **aggregation layer**, not byte I/O.
- **Model abstraction** — `CceModel.AddForest(IntPtr, name)` is the only entry. `cce_forest_open` exists in C but is **not exported / not P/Invoked**. The engine's real composition verbs (`cce_forest_connect`, `cce_forest_merge`, typed connections sub/refines/composes/specializes) are ignored by `.NET`.
- **Extras** — the learner already supports `classify` (MSE vs softmax-CE) and `grad_clip`/`dfa_strength`/`goodness_threshold` knobs ([cce_learn.h:14-21](../../../include/cce/cce_learn.h)) that the model API never surfaces. Metrics/callbacks/transforms are pure host-side concerns. No swappable optimizer "zoo" is feasible or aligned.

Key architectural constraint discovered: `cce_forest_open` **requires** a backing archive file, and `cce_forest_add_branch` **re-serializes** the cascade into the forest archive on add ([cce_forest.c:17,79](../../../src/cce/cce_forest.c)). Therefore a loaded model cannot reconstruct forests by replaying `add_branch`. This drives the load design (Section 4).

## 2. Goals / Non-Goals

**Goals**
- Real, round-trippable `cce_model_save`/`cce_model_load` producing **one self-contained `.cce` bundle**.
- `.NET` `CceModel.Save(path)` / `CceModel.Load(path)`.
- A safe-handle `CceForest` type + first-class composition surface, replacing raw `IntPtr` as the primary path.
- Honest loss values + loss-type selection; exposure of existing learner knobs; managed metrics; managed callback-able `Fit()` loop.
- Test-first; a save→load→infer round-trip is the regression gate.

**Non-Goals**
- Optimizer zoo / swappable host-side optimizers (baked into the C learner; out of scope and misaligned).
- Native/monolithic data transforms pipeline. A small host-side .NET transform subset was added later: one-hot labels, row L2 normalization, and `CceDataset.FromLabels`.
- Cross-machine / cross-endianness archive portability beyond same-target builds (version-gated, single MinGW target assumed).
- Replacing the C fast-path `cce_model_train`; `Fit()` is additive.

## 3. Decisions (the three forks)

1. **Save format → self-contained bundle.** One `.cce` file containing a manifest + all forests' weights re-serialized inline. Portable like `torch.save`; one verifiable artifact consistent with the "one archive, not 10k loose files" ethos. Costs more C than a linked-manifest approach, accepted.
2. **Model API → safe-handle + composition.** `CceForest` safe handle + factories, plus surfacing the engine's typed composition verbs as managed API. Explicitly **not** an `nn.Sequential` clone (rejected as cargo-culting that hides the composition/contract nature).
3. **Extras -> all four** of {loss types + real loss, metrics, callbacks + managed `Fit()`, optimizer knobs}. Updated later with bounded host-side transforms; still no native transform pipeline.

## 4. Detailed Design

### 4.1 Serialization — self-contained `.cce` bundle

**Bundle layout** (existing `cce_archive` sections):

- Section `MODEL_MANIFEST` — versioned, magic-gated, written **field-by-field little-endian** (not a raw `struct` write, to avoid padding/layout surprises and to allow version evolution):
  - Header: `magic='CMDL'`, `version=1`, `name[64]`, `diff_mode`, `classify`, `goodness_threshold`, `dfa_strength`, `grad_clip`, `has_scheduler`, full `cce_scheduler` state (if present), `num_forests`.
  - Per forest: `name[64]`, `num_branches`, `centroid_dim`, `sealed`, `diff_mode`, `default_exact_tail_length`.
  - Per branch: `name[64]`, `centroid[32]`, `centroid_dim`, `tier`, `diff_mode`, `exact_tail_length`, `num_connections`, `conn_names[8][64]`, `conn_types[8]`, and `cascade_offset` (u64) — the offset of this branch's cascade section within the same bundle.
- Per HOT branch: one cascade section named e.g. `f{fi}_b{bi}` written via `cce_cascade_save_to_archive`; the returned offset is recorded as `cascade_offset` in the manifest.

**`cce_model_save(model, path)`**
1. `cce_archive_open(&arc, path)` (create/truncate).
2. For each forest, for each branch: ensure the cascade is materialized HOT (promote if needed); `cce_cascade_save_to_archive(cas, arc, sec_name, &off)`; stash `off`.
3. Build and append the `MODEL_MANIFEST` section (now that all offsets are known).
4. Close archive. Return `CCE_OK` / error.

**`cce_model_load(model, path)`**
1. `cce_archive_open(&arc, path)`; `cce_archive_find_section("MODEL_MANIFEST", ...)`; read + validate magic/version (`CCE_ERR_UNSUPPORTED` on mismatch) and bounds (`num_forests <= 16`, `num_branches <= max`).
2. Restore model scalar state (name, diff_mode, classify, learn knobs). If `has_scheduler`, allocate an **owned** `cce_scheduler`, restore its state, set `model->scheduler`, set `owns_scheduler=1`.
3. For each forest: allocate an **owned**, **archive-less** forest (HOT-only: `archive=NULL`, branches/centroids `calloc`'d). For each branch: alloc an owned `cce_cascade`, `cce_cascade_load_from_archive(cas, arc, cascade_offset)`, restore branch metadata (centroid, tier→HOT, diff_mode, connections, name). Register the forest in `model->forests[]`, set `owns_forests=1`.
4. Close the bundle archive (all data already copied into HOT RAM). Return `CCE_OK`.

**New C state on `cce_model`:** `int owns_forests;`, `int owns_scheduler;`. `cce_model_destroy` frees owned forests (`cce_forest_close`) and the owned scheduler. Pre-load behavior (forests/scheduler added by caller) keeps `owns_*=0` → destroy does not free them, preserving current ownership semantics.

**NULL-archive guard:** a loaded forest has `archive=NULL`. Every `f->archive` dereference in the forest/inference path must be audited and guarded; `cce_forest_close` must tolerate `archive==NULL` (and `cce_archive_close(NULL)` must be a no-op — verify). A loaded model supports inference, continued training, and re-save (re-serializes from HOT cascades); adding a *new* branch to a loaded archive-less forest is out of scope (documented limitation).

**Isolation:** bundle read/write lives in a new `src/cce/cce_model_io.c` (+ internal `cce_model_io.h`); `cce_model_save`/`load` in `cce_model.c` delegate to it. Keeps `cce_model.c` focused on training/dispatch.

**Regression gate (C test):** create a model with a small trained forest → save → load into a fresh model → assert (a) manifest scalars restored, (b) inference output/label matches the original on a fixed input set, (c) clean free with no leak. This mirrors the project's "byte-identical regen is the gate" discipline.

### 4.2 Model abstraction — safe-handle + composition

**C exports (new `CCE_API` + accessors):** mark `cce_forest_open`, `cce_forest_close`, `cce_forest_connect`, `cce_forest_merge`, `cce_forest_get_connections`, `cce_forest_set_diff_mode`, `cce_forest_set_branch_diff_mode`, `cce_forest_set_branch_exact_tail_length` as `CCE_API` (currently unexported → invisible to P/Invoke under `CCE_BUILD_DLL` on Windows). Add small accessors `int cce_forest_branch_count(const cce_forest*)` and `cce_result cce_forest_branch_name(const cce_forest*, int idx, char* buf, int len)`.

**.NET:**
- `CceForest : SafeHandle` (own native `cce_forest*`; `ReleaseHandle` → `cce_forest_close`). Factory `static CceForest Open(string archivePath, int maxBranches)`.
- `CceModel.Add(CceForest forest, string name)` — new primary overload. `AddForest(IntPtr, string)` kept and marked `[Obsolete("Use Add(CceForest, name)")]` for advanced/interop use.
- Composition surface on `CceForest`: `Connect(int fromBranch, int toBranch, CceConnectionType type)`, `Merge(CceForest src, string namePrefix)`, `IReadOnlyList<(string Name, CceConnectionType Type)> GetConnections(int branch)`, `SetDiffMode(...)`, per-branch override setters, `int BranchCount`, `string BranchName(int idx)`.
- New enum `CceConnectionType { Sub = 0, Refines = 1, Composes = 2, Specializes = 3 }` (matches C `conn_types`).

### 4.3 Extras

**(a) Loss types + real loss.**
- C: `cce_model` stores `int classify` (0=MSE, 1=softmax-CE). `cce_model_set_loss(model, int classify)`. In the per-sample dispatch, set `learner.classify` and compute an **honest** scalar loss from a forward pass (MSE = ½·Σ(pred−tgt)²; CE = −Σ tgt·log softmax(pred)), replacing the current goodness-derived proxy returned from `dispatch_train_sample`.
- .NET: `enum CceLossType { MeanSquaredError = 0, CrossEntropy = 1 }`; `CceTrainingConfig.Loss`; wired through `Train`/`Fit`.

**(b) Optimizer knobs.**
- C: `cce_model_set_learn_params(model, float goodness_threshold, float dfa_strength, float grad_clip)`; applied where the dispatch currently hardcodes `cce_learner_init(&learner, 0.7f)`. Zero defaults preserve current behavior.
- .NET: `CceTrainingConfig` gains `GoodnessThreshold`, `DfaStrength`, `GradClip` (nullable / sentinel = "leave default").

**(c) Metrics — pure managed.**
- `static class CceMetrics`: `float Accuracy(CceModel, CceDataset)` and `float TopK(CceModel, CceDataset, int k)` — iterate batches, `InferBatch`, compare predicted label to target argmax. No C changes.

**(d) Callbacks + managed `Fit()` — pure managed.**
- `CceModel.Fit(CceDataset train, CceTrainingConfig config, CceCallbacks? callbacks = null, CceDataset? validation = null)`: managed epoch loop over the existing `NextBatch` + `TrainBatch`; accumulates loss; optional validation metrics; invokes `callbacks.OnEpochEnd(EpochInfo)`.
- `CceCallbacks` — delegate-based record: `Func<EpochInfo, bool>? OnEpochEnd` where **returning `false` requests early stop** (returning `true`/`null` continues). The user may call `model.Save(...)` for checkpointing inside the hook.
- `readonly record struct EpochInfo(int Epoch, double TrainLoss, float? ValAccuracy, float CurrentLr)` — passed by value (no mutable back-channel; stop is signalled only via the return value).
- The C `cce_model_train` (`CceModel.Train`) remains the no-callback fast path; `Fit()` is the hookable path. Both coexist.

## 5. Testing Strategy

**C (`tests/test_cce_model_save.c`, `make cce_model_test`):**
- Round-trip gate (Section 4.1).
- Manifest version/magic rejection (corrupt/old → `CCE_ERR_UNSUPPORTED`).
- Owned-free path: load then destroy with no leak / no double-free; archive-less forest close.
- Real-loss sanity: MSE and CE produce finite, decreasing-with-training values on a toy table.

**.NET (`Cce.Tests`):**
- Save/Load round-trip via managed API; inference parity post-load.
- `CceForest.Open` + `CceModel.Add(CceForest)` + `Connect`/`GetConnections` round-trip.
- `Fit()` invokes `OnEpochEnd` each epoch; early-stop halts; checkpoint callback writes a loadable file.
- `CceMetrics.Accuracy` ≈ 1.0 on a trivially separable dataset.
- Loss-type switch (MSE ↔ CE) changes reported loss appropriately.

**Process:** test-first (TDD) — failing test before each implementation unit, sequenced by the implementation plan.

## 6. Files

**New**
- `src/cce/cce_model_io.c`, `src/cce/cce_model_io.h`
- `dotnet/Cce/CceForest.cs`
- `dotnet/Cce/CceMetrics.cs`
- `dotnet/Cce/CceCallbacks.cs`
- `tests/test_cce_model_save.c`

**Edited**
- `src/cce/cce_model.c` (save/load delegation, owns_* state, classify + learn-param fields, real-loss in dispatch, destroy frees owned)
- `include/cce/cce_model.h` (`cce_model_set_loss`, `cce_model_set_learn_params` decls)
- `include/cce/cce_forest.h` (+`CCE_API` exports, branch_count/branch_name accessors)
- `src/cce/cce_forest.c` (NULL-archive guards, accessor impls)
- `dotnet/Cce/CceModel.cs` (`Save`/`Load`, `Add(CceForest)`, `Fit`, obsolete `AddForest`)
- `dotnet/Cce/CceEnums.cs` (`CceLossType`, `CceConnectionType`, config fields)
- `dotnet/Cce/CceNative.cs` (P/Invoke for save/load, forest open/close/connect/merge/accessors, set_loss/set_learn_params)
- `Makefile` (DLL export build includes `cce_model_io.c`; new C test target)

## 7. Risks & Mitigations

- **Load-side forest reconstruction without re-serialize** — handled by HOT-materialize + owned, archive-less forest. Requires a careful audit/guard of every `f->archive` deref; covered by the owned-free + NULL-archive tests.
- **Struct layout drift** — `cce_scheduler` is mirrored in the manifest writer, the C struct, and `CceSchedulerNative` (C#). Field-by-field manifest + version gate contains the risk; a test asserts scheduler state restores exactly.
- **DLL export omissions** — Windows export requires `CCE_API`; missing it surfaces as a P/Invoke `EntryPointNotFound`. Mitigated by the `.NET` `CceForest` test exercising each exported entry.
- **No git** — design and code are filesystem-only; verification is by tests and inspection, not version control.

## 8. Open Questions

None blocking. Deferred-by-decision: data transforms, optimizer zoo, cross-endianness portability, adding new branches to a loaded archive-less forest.
