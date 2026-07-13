# Code Review: Unified Specialist Implementation (commit 0e07717)

## Scope
- `include/specialist.h`, `src/specialist.c`, `src/specialist_adapters.c`
- `include/specialist_adapters.h`, `include/cce/cce_contract_adapter.h`, `src/cce/cce_contract_adapter.c`
- `tests/test_heterogeneous_plan.c` (the only specialist test; `tests/test_specialist.c` does NOT exist)
- `Makefile` wiring (`unified_specialist`, `cnet_dll`, `unified_native` targets)
- `src/router/registry.c` (one-line change: `btn_cost` adapter shortcut)
- Related: `include/router.h`, `include/contract/contract.h`, `include/acquire.h`, `include/nn.h`

## Build & Test Status
- `make unified_specialist` → **HET_PLAN_PASS** (24/24 checks pass)
- `make cnet_dll` → all specialist symbols exported in `cnet.so` (nm-verified)
- No compiler errors (only pre-existing warnings in unrelated files)

---

## Issues Found

### ISSUE 1 — HIGH: `specialist_axes` trust logic misclassifies certified+PROVISIONAL as EVIDENCED
**File:** `src/specialist.c`, lines 93–100  
**Symbol:** `specialist_axes`

The trust axis check order is:
```c
if (e->state == PRIM_RESET)
    *trust = SPECIALIST_TRUST_DEMOTED;
else if (e->certified && e->state == PRIM_FROZEN)
    *trust = SPECIALIST_TRUST_CERTIFIED;
else if (e->state == PRIM_PROVISIONAL)
    *trust = SPECIALIST_TRUST_EVIDENCED;
else
    *trust = SPECIALIST_TRUST_UNCERTIFIED;
```

If an entry has `certified=1` but `state=PRIM_PROVISIONAL` (achievable by calling `registry_set_state(reg, name, PRIM_PROVISIONAL)` after certification), the function returns EVIDENCED, silently dropping the certification signal. A certified entry should never read as merely "evidenced" — the certification is a stronger guarantee than provisional evidence.

Similarly, `certified=1 && state=PRIM_FUZZY` (achievable via `registry_set_state`) returns UNCERTIFIED, which is also wrong for a certified entry.

**Impact:** Any caller relying on `specialist_axes` for trust decisions will under-report the trust of a certified entry whose state was manually adjusted. The planner itself checks `certified` directly (in `entry_usable_base`), so planning is unaffected — but the axis view is inconsistent with the planner's own truth.

**Minimal RED test:**
```c
/* RED: certified entry in PROVISIONAL state must NOT read as EVIDENCED */
void test_certified_provisional_trust(void) {
    PrimitiveRegistry reg;
    BinaryTransformNetwork btn;
    Contract c;
    Specialist s;
    SpecialistTrust trust;

    memset(&btn, 0, sizeof btn);
    memset(&c, 0, sizeof c);
    registry_init(&reg);
    btn_init(&btn, 4, 4, 8, 64, 0.5, 42);
    /* ... set ports, author contract with known exemplars ... */
    specialist_wrap_btn(&s, &btn, "test");
    specialist_admit(&reg, &s, &c);   /* certified=1, state=PRIM_FROZEN */

    registry_set_state(&reg, "test", PRIM_PROVISIONAL);
    specialist_axes(&reg, "test", &trust, NULL);

    /* This FAILS today: returns EVIDENCED, should return CERTIFIED */
    assert(trust == SPECIALIST_TRUST_CERTIFIED);
}
```

**Fix (minimal):** Check `certified` before `PRIM_PROVISIONAL`:
```c
else if (e->certified)
    *trust = SPECIALIST_TRUST_CERTIFIED;  /* certified regardless of non-RESET state */
else if (e->state == PRIM_PROVISIONAL)
    *trust = SPECIALIST_TRUST_EVIDENCED;
else
    *trust = SPECIALIST_TRUST_UNCERTIFIED;
```

---

### ISSUE 2 — MEDIUM: No dedicated unit test for `specialist_*` API edge cases
**File:** `tests/test_specialist.c` — **does not exist**  
**File:** `tests/test_heterogeneous_plan.c` — the only test, covers happy paths

The heterogeneous plan test is an acceptance gate (24 checks, all pass), but it has **zero coverage** for:
- NULL/empty-arg rejection in `specialist_wrap_btn`, `specialist_wrap_cce_model`, `specialist_wrap_oracle`, `specialist_admit`, `specialist_axes`
- `specialist_wrap_btn` refusing an adapter BTN (`btn_is_adapter` path)
- `specialist_wrap_oracle` with `behavior_digest=0` fallback to `entry->behavior_digest`
- `specialist_wrap_oracle` when both digests are 0 (should fail)
- `specialist_axes` on unknown name (should return -1)
- `specialist_axes` with NULL trust/role out-pointers (should still succeed)
- `specialist_residency_from_cce_tier` / `specialist_residency_from_model_state` with unknown/negative values
- `specialist_*_name` functions for each enum value
- `specialist_admit` on an already-admitted same-name entry (replace path via `contract_better_if`)
- `specialist_admit` when `registry_add_certified` fails (certification fails → digest stays 0)

**Impact:** Regression in any of these edge-case paths would go undetected.

**Minimal RED test (skeleton):**
```c
/* RED: edge-case coverage that test_heterogeneous_plan.c does NOT provide */
void test_specialist_null_args(void) {
    PrimitiveRegistry reg;
    BinaryTransformNetwork btn;
    Specialist s;
    Contract c;
    SpecialistTrust t;
    SpecialistRole r;

    registry_init(&reg);
    memset(&btn, 0, sizeof btn);
    memset(&c, 0, sizeof c);
    memset(&s, 0, sizeof s);

    /* wrap_btn rejects NULL s, NULL btn, NULL name, empty name */
    assert(specialist_wrap_btn(NULL, &btn, "x") == -1);
    assert(specialist_wrap_btn(&s, NULL, "x") == -1);
    assert(specialist_wrap_btn(&s, &btn, NULL) == -1);
    assert(specialist_wrap_btn(&s, &btn, "") == -1);

    /* wrap_btn refuses an adapter BTN */
    btn_init(&btn, 4, 4, 8, 64, 0.5, 42);
    btn_set_ports(&btn, p_in, p_out);
    /* make btn an adapter via btn_init_adapter ... */
    assert(specialist_wrap_btn(&s, &btn, "x") == -1);  /* RED: not tested */

    /* admit rejects NULL reg, NULL s, NULL contract */
    assert(specialist_admit(NULL, &s, &c) == -1);
    assert(specialist_admit(&reg, NULL, &c) == -1);
    assert(specialist_admit(&reg, &s, NULL) == -1);

    /* axes on unknown name returns -1 */
    assert(specialist_axes(&reg, "nope", &t, &r) == -1);
    /* axes with NULL out-pointers succeeds */
    assert(specialist_axes(&reg, "nope", NULL, NULL) == -1); /* still unknown */
}
```

---

### ISSUE 3 — MEDIUM: `nm` audit in `unified_native` only checks 2 of 11 exported specialist symbols
**File:** `Makefile`, line 1343–1344

```
@nm -D cnet.so | grep -q " specialist_admit$$"
@nm -D cnet.so | grep -q " specialist_axes$$"
```

The audit verifies only `specialist_admit` and `specialist_axes` are exported. It does NOT check:
- `specialist_wrap_btn`, `specialist_wrap_cce_model`, `specialist_wrap_oracle`
- `specialist_residency_from_cce_tier`, `specialist_residency_from_model_state`
- `specialist_kind_name`, `specialist_trust_name`, `specialist_residency_name`, `specialist_role_name`

All 11 symbols are actually exported (nm-verified), so this is a test-coverage gap, not a functional defect. But a future build change that accidentally hides one of the unchecked symbols would go unnoticed.

**Fix (minimal):** Add a single line:
```makefile
@nm -D cnet.so | grep -q " specialist_wrap_btn$$"
```
or audit the full set with a loop.

---

### ISSUE 4 — LOW (pre-existing, not introduced by this commit): `registry_init` does not zero `streamer` field
**File:** `src/router/registry.c`, lines 385–403

`registry_init` explicitly sets every field of `PrimitiveRegistry` except `streamer`. Any caller that declares `PrimitiveRegistry reg;` on the stack (without `memset`) and calls `registry_init` will have an uninitialized `reg.streamer`. The heterogeneous plan test does exactly this (line 136: `registry_init(&reg);` with no preceding memset for `reg`).

Currently harmless because the router code never reads `streamer` — only narrative contract code does, and the test doesn't exercise that path. But it's a latent UB (calling through an uninitialized function pointer) for any future test or caller that uses narrative contracts without memset.

**Not introduced by this commit** (the `streamer` field predates it), but the test exercises the path.

**Fix:** Add `reg->streamer = NULL;` to `registry_init`.

---

### ISSUE 5 — LOW: `specialist_admit` does not check `s->kind` is valid
**File:** `src/specialist.c`, line 70

`specialist_admit` checks `!s->btn` and `!s->name` but not `s->kind`. If a caller passes a zero-initialized `Specialist` (kind=0=SPECIALIST_KIND_BTN) with a valid `btn` and `name`, the admit proceeds. This is arguably fine since kind=BTN is valid, but there's no guard against a completely uninitialized Specialist struct where `kind` is garbage. In practice, the `wrap_*` functions set kind, so this is defensive only.

**Impact:** Minimal — misuse requires skipping the wrap step.

---

### ISSUE 6 — INFO: `walk_dag` test utility has no depth guard
**File:** `tests/test_heterogeneous_plan.c`, lines 103–120

`walk_dag` recurses without a depth limit. Planner-built DAGs are bounded by `DAG_MAX_DEPTH=8`, so this is safe for all current usage. A hand-built cyclic DAG would stack-overflow, but `walk_dag` is a test-only helper, not production code.

No action needed.

---

## Summary Table

| # | Severity | File:Line | Symbol | Issue |
|---|----------|-----------|--------|-------|
| 1 | HIGH | specialist.c:93-100 | `specialist_axes` | certified+PROVISIONAL misclassified as EVIDENCED |
| 2 | MEDIUM | (missing) | test suite | no edge-case unit tests for specialist API |
| 3 | MEDIUM | Makefile:1343 | `unified_native` | nm audit checks only 2/11 exported symbols |
| 4 | LOW | registry.c:385 | `registry_init` | `streamer` not zeroed (pre-existing) |
| 5 | LOW | specialist.c:70 | `specialist_admit` | no `s->kind` validation |
| 6 | INFO | test_het:103 | `walk_dag` | no depth guard (test-only) |

## Verdict

The unified Specialist implementation is **functionally correct** for its designed happy paths — the heterogeneous plan gate passes cleanly with all three kinds (BTN, CCE, Oracle) admitted, planned, executed, and lifecycle-managed through the unified type. The one HIGH issue (`specialist_axes` trust misclassification on state-manually-adjusted certified entries) is a semantic inconsistency in the view, not a planner-affecting bug. The main gap is test coverage: edge cases for the specialist API have no dedicated unit tests, and the nm audit is incomplete.