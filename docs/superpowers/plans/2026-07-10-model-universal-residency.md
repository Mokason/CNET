# Model-Universal Residency Implementation Plan

> **For Hermes:** Execute each production slice through strict RED → GREEN → refactor. Do not commit without user instruction.

**Goal:** Add a native backend-neutral catalog and memory-budgeted residency manager that governs dense, MoE, and SSM models under one CNET lifecycle.

**Architecture:** A small C module owns descriptors, resource budgets, admission, leases, load deduplication, and LRU eviction. Backend callbacks materialize opaque model handles but cannot select placement or change policy. CCE detection supplies header-only structural descriptors; existing async lanes and Oracle v2 remain the request/evidence layer.

**Tech stack:** C11, pthreads, existing CCE GGUF detection, Makefile gates.

---

### Task 1: Dense and MoE coexistence tracer

**Files:**
- Create: `tests/test_model_runtime.c`
- Create: `include/model_runtime.h`
- Create: `src/model_runtime.c`

1. Write a failing test against the desired manager API: register one mock backend, add dense and MoE descriptors, and assert catalog/state inspection.
2. Compile and confirm failure because the API/module is absent.
3. Add only descriptor validation, backend registration, catalog insertion, and stats.
4. Re-run until the focused test passes.

### Task 2: Lazy load, reuse, and maximum residency

**Files:**
- Modify: `tests/test_model_runtime.c`
- Modify: `src/model_runtime.c`

1. Add a failing test where two 10-byte dense models occupy separate 16-byte GPU budgets and repeated acquisition is a cache hit.
2. Implement one-resource placement, lazy backend load, leases, and persistent residency.
3. Verify exact resource accounting and load counts.

### Task 3: Safe LRU eviction and distributed MoE admission

**Files:** same as Task 2.

1. Add a failing test where a 12-byte-per-resource MoE model requiring `0b11` atomically evicts two inactive dense models.
2. Add failing cases proving leased and pinned models cannot be evicted.
3. Implement LRU victim selection, multi-resource accounting, pin/unpin, and explicit eviction.
4. Verify no callback/accounting leak on refusal.

### Task 4: Concurrent cold-load deduplication and failure recovery

**Files:** same as Task 2.

1. Add two-thread cold-acquire test; require exactly one backend load and the same handle/generation.
2. Add load-failure and actual-usage-over-estimate tests.
3. Implement loading state, condition waits, reservations, sticky failure, and explicit retry reset.
4. Run ASAN/UBSAN and leak checks on the focused binary.

### Task 5: Header-only real model descriptors

**Files:**
- Modify: `include/cce/cce_detect.h`
- Modify: `src/cce/cce_detect.c`
- Modify: `tests/cce_detect_test.c`
- Create: `include/cce/cce_model_catalog.h`
- Create: `src/cce/cce_model_catalog.c`
- Create: `tests/test_model_catalog.c`

1. Add failing synthetic dense/MoE GGUF detection checks.
2. Add structural `is_moe` evidence to `cce_model_info` without claiming runnable support.
3. Add a descriptor adapter that copies CCE metadata and filesystem size without loading weights.
4. Probe guarded local GGUF paths and print catalog descriptors.

### Task 6: Build and documentation integration

**Files:**
- Modify: `Makefile`
- Modify: `README.md`
- Modify: `DS4_EXTRACTION.md`

1. Add `unified_models` focused gate and include it in `unified_native`.
2. Document resource masks, dense single-GPU placement, distributed MoE placement, and backend authority boundaries.
3. Run focused gates, CCE detection, full native verification, sanitizer, `git diff --check`, and artifact cleanup.
