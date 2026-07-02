# Lifecycle Meltdown → Retrain Implementation Plan (scope C)

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.
> **NO GIT COMMITS** on this project — implement and verify against the working tree only (the user manages version control). Drop the per-task commit steps; verify each task with `make test` + `git diff HEAD`.

**Goal:** Close the continual-learning loop: a primitive that fails a runtime invariant goes `PRIM_RESET`, its failing case is captured, a **verified** target is obtained (invariant-derived / teacher / external-oracle), and a `registry_heal` wake pass retrains it on its contract ∪ the verified cases — restoring `FROZEN` **only** on a passing `btn_certify`, never on a guess.

**Architecture:** A `RetrainQueue` (labeled + unlabeled sections) hangs off each `RegistryEntry`. Queue machinery + fault recording + labeling live in `src/router.c` (registry operations, no contract dependency). `registry_heal` lives in `src/contract.c` (it needs `Contract` + `btn_certify` + `btn_train`, all already there). All opt-in / additive; nothing changes existing behavior.

**Scope (this MVP) / deferred:**
- Target source **A** = the primitive's **contract** (retraining on the contract IS the invariant-derived target). Property-law-derived targets are deferred.
- Target source **B** = teacher primitive (`registry_label_via_teacher`).
- Target source **C** = external-oracle hook (`registry_supply_label` + `registry_pending_labels`); the external labeler itself is out of scope.
- The runtime **law-violation trigger** (run `property_check` over an active route, RESET participants) is **deferred** — the strict-fault path (from self-healing's `ExecFault`, plus the explicit `registry_record_fault` API) is the trigger implemented here.
- `registry_heal` retrains with the fixed-epoch `btn_train` (deterministic) rather than `btn_train_dynamic`.
- The latent `NULL`-name guard in `registry_add_certified` (flagged by the spine review) is fixed first (Task M1) because `registry_heal` re-enters that certify path.
- `lifecycle.c` extraction still deferred (code stays in router.c / contract.c).

---

## File structure
- `include/router.h` — `RetrainQueue` struct (before `RegistryEntry`), `RegistryEntry.queue` field, declarations for `registry_record_fault` / `registry_pending_labels` / `registry_supply_label` / `registry_label_via_teacher`.
- `include/contract.h` — `registry_heal` declaration.
- `src/router.c` — queue init in `registry_add`, queue free in `registry_free`, and the four queue/labeling functions.
- `src/contract.c` — the `NULL`-name guard fix + `registry_heal`.
- `tests/test_lifecycle.c` — tests for each task (already includes `nn.h`, `router.h`, `contract.h`).

---

### Task M1: Fix the latent NULL-name guard in `registry_add_certified`

**Files:** `src/contract.c` (the replace loop), `tests/test_lifecycle.c`.

- [ ] **Step 1: Failing test** — add to `tests/test_lifecycle.c` before `run_test_lifecycle`:

```c
static void test_certified_add_null_name_safe(void) {
    BinaryTransformNetwork dummy = {0};
    BinaryTransformNetwork rawid = {0};
    PrimitiveRegistry reg;
    Contract c = {0};
    double in[4] = {1.0, 0.0, 1.0, 0.0};
    double tgt[5];
    const double *out;

    printf("lifecycle: registry_add_certified survives a NULL-named entry:\n");
    if (make_btn(&dummy, 4, 5, P(PORT_BINARY_MSB, 4, 1), P(PORT_RAW, 5, 1)) != 0 ||
        make_btn(&rawid, 4, 5, P(PORT_BINARY_MSB, 4, 1), P(PORT_RAW, 5, 1)) != 0) {
        CHECK(0, "synthetic primitive setup");
        return;
    }
    out = btn_forward(&rawid, in);
    memcpy(tgt, out, sizeof tgt);

    registry_init(&reg);
    registry_add(&reg, &dummy, NULL);   /* a NULL-named incumbent */
    CHECK(contract_init_borrowed(&c, "rawid", &rawid, in, tgt, 1) == 0,
          "contract_init_borrowed ok");
    /* Must not crash on strcmp(NULL, "rawid"); should append + certify. */
    CHECK(registry_add_certified(&reg, &rawid, "rawid", &c) == 0,
          "certified add appends past the NULL-named entry");
    CHECK(reg.count == 2 && reg.entries[1].state == PRIM_FROZEN,
          "new entry appended and FROZEN");

    contract_free(&c);
    registry_free(&reg);
    btn_free(&dummy);
    btn_free(&rawid);
}
```
Call it first in `run_test_lifecycle`.

- [ ] **Step 2: Build → fails** (`make test`): the run crashes / fails on `strcmp(NULL, "rawid")` in `registry_add_certified`.

- [ ] **Step 3: Fix** — in `src/contract.c`, the `registry_add_certified` replace loop currently reads:
```c
    for (i = 0; i < reg->count; ++i) {
        if (strcmp(reg->entries[i].name, name) == 0) {
```
change the condition to skip NULL names:
```c
    for (i = 0; i < reg->count; ++i) {
        if (reg->entries[i].name != NULL &&
            strcmp(reg->entries[i].name, name) == 0) {
```

- [ ] **Step 4: Build + run → pass.** `make test` ends `ALL TESTS PASSED (single exe)`.

---

### Task M2: `RetrainQueue` + fault recording

**Files:** `include/router.h`, `src/router.c`, `tests/test_lifecycle.c`.

- [ ] **Step 1: Add the struct + field + declarations (include/router.h).** Add this struct just BEFORE the `RegistryEntry` typedef:
```c
/* Per-primitive retraining queue (meltdown -> heal). A failing input is parked
   UNLABELED (we have the input + the bad raw output but no verified target).
   Labeling (teacher / external oracle) moves it to LABELED (input + verified
   target). registry_heal retrains on contract exemplars UNION the labeled set.
   Dims (input_count/output_count) are fixed from the primitive on first use. */
typedef struct {
    size_t input_count;
    size_t output_count;
    double *labeled_inputs;    /* labeled_count x input_count */
    double *labeled_targets;   /* labeled_count x output_count */
    size_t labeled_count;
    size_t labeled_cap;
    double *unlabeled_inputs;  /* unlabeled_count x input_count */
    double *unlabeled_raw;     /* unlabeled_count x output_count (the bad output) */
    size_t unlabeled_count;
    size_t unlabeled_cap;
} RetrainQueue;
```
Add the field to `RegistryEntry` (after `state`):
```c
    RetrainQueue *queue;  /* owned; NULL until the first fault is recorded */
```
Add declarations after `lifecycle_promote_provisional`:
```c
/* Record a runtime fault: park (input, raw_output) in the named primitive's
   UNLABELED queue (lazily allocating it) and set the primitive PRIM_RESET.
   input has the primitive's input_count values; raw_output its output_count.
   Returns 0, or -1 (reg/name NULL, unknown name, or OOM). */
int registry_record_fault(PrimitiveRegistry *reg, const char *name,
                          const double *input, const double *raw_output);

/* Number of UNLABELED (awaiting-a-target) failures parked for `name`. 0 if no
   queue / unknown name. */
size_t registry_pending_labels(const PrimitiveRegistry *reg, const char *name);
```

- [ ] **Step 2: Wire init + free (src/router.c).** In `registry_add`, after `reg->entries[reg->count].state = PRIM_FUZZY;`:
```c
    reg->entries[reg->count].queue = NULL;
```
Add a static queue-free helper and call it from `registry_free`. Add this helper ABOVE `registry_free`:
```c
static void retrain_queue_free(RetrainQueue *q) {
    if (q == NULL) return;
    free(q->labeled_inputs);
    free(q->labeled_targets);
    free(q->unlabeled_inputs);
    free(q->unlabeled_raw);
    free(q);
}
```
In `registry_free`, BEFORE `free(reg->entries);`, free each entry's queue:
```c
    {
        size_t i;
        for (i = 0; i < reg->count; ++i) {
            retrain_queue_free(reg->entries[i].queue);
            reg->entries[i].queue = NULL;
        }
    }
```

- [ ] **Step 3: Implement record_fault + pending_labels (src/router.c).** Add near the other lifecycle helpers (e.g. after `lifecycle_promote_provisional`):
```c
/* Find an entry by name (first match). Returns index or reg->count if absent. */
static size_t registry_find(const PrimitiveRegistry *reg, const char *name) {
    size_t i;
    for (i = 0; i < reg->count; ++i) {
        if (reg->entries[i].name != NULL &&
            strcmp(reg->entries[i].name, name) == 0) {
            return i;
        }
    }
    return reg->count;
}

int registry_record_fault(PrimitiveRegistry *reg, const char *name,
                          const double *input, const double *raw_output) {
    size_t idx, ic, oc;
    RetrainQueue *q;
    if (reg == NULL || name == NULL || input == NULL || raw_output == NULL) {
        return -1;
    }
    idx = registry_find(reg, name);
    if (idx == reg->count || reg->entries[idx].btn == NULL) return -1;
    ic = reg->entries[idx].btn->input_count;
    oc = reg->entries[idx].btn->output_count;

    q = reg->entries[idx].queue;
    if (q == NULL) {
        q = calloc(1, sizeof *q);
        if (q == NULL) return -1;
        q->input_count = ic;
        q->output_count = oc;
        reg->entries[idx].queue = q;
    }
    if (q->unlabeled_count == q->unlabeled_cap) {
        size_t ncap = q->unlabeled_cap == 0 ? 4 : q->unlabeled_cap * 2;
        double *ni = realloc(q->unlabeled_inputs, ncap * ic * sizeof(double));
        double *nr = realloc(q->unlabeled_raw, ncap * oc * sizeof(double));
        if (ni == NULL || nr == NULL) { free(ni == NULL ? NULL : ni); return -1; }
        q->unlabeled_inputs = ni;
        q->unlabeled_raw = nr;
        q->unlabeled_cap = ncap;
    }
    memcpy(q->unlabeled_inputs + q->unlabeled_count * ic, input, ic * sizeof(double));
    memcpy(q->unlabeled_raw + q->unlabeled_count * oc, raw_output, oc * sizeof(double));
    q->unlabeled_count++;
    reg->entries[idx].state = PRIM_RESET;
    return 0;
}

size_t registry_pending_labels(const PrimitiveRegistry *reg, const char *name) {
    size_t idx;
    if (reg == NULL || name == NULL) return 0;
    idx = registry_find(reg, name);
    if (idx == reg->count || reg->entries[idx].queue == NULL) return 0;
    return reg->entries[idx].queue->unlabeled_count;
}
```
(Note: `registry_find` is shared by later tasks. If `string.h` isn't already included in router.c it is — `strcmp` is used elsewhere.)

- [ ] **Step 4: Test (tests/test_lifecycle.c).** Add before `run_test_lifecycle`, and call it:
```c
static void test_record_fault(void) {
    BinaryTransformNetwork p = {0};
    PrimitiveRegistry reg;
    double in_bits[4] = {1.0, 0.0, 1.0, 0.0};
    double raw[1] = {0.5};

    printf("lifecycle: record_fault parks unlabeled + RESETs:\n");
    if (make_btn(&p, 4, 1, P(PORT_BINARY_MSB, 4, 1), P(PORT_BINARY_MSB, 1, 1)) != 0) {
        CHECK(0, "synthetic primitive setup");
        return;
    }
    registry_init(&reg);
    registry_add(&reg, &p, "p");
    CHECK(registry_record_fault(&reg, "p", in_bits, raw) == 0, "record_fault ok");
    CHECK(reg.entries[0].state == PRIM_RESET, "primitive is RESET after fault");
    CHECK(registry_pending_labels(&reg, "p") == 1, "one unlabeled fault parked");
    CHECK(registry_record_fault(&reg, "missing", in_bits, raw) == -1, "unknown name -> -1");
    registry_free(&reg);   /* must free the queue without leaking */
    btn_free(&p);
}
```

- [ ] **Step 5: Build + run → pass.**

---

### Task M3: Labeling — external oracle (C) and teacher (B)

**Files:** `include/router.h`, `src/router.c`, `tests/test_lifecycle.c`.

- [ ] **Step 1: Declarations (include/router.h).** After the M2 declarations:
```c
/* External-oracle hook (target source C): supply a verified target for a parked
   UNLABELED failure whose input matches `input` (exact match per value). Moves
   it to the LABELED set. Returns 0, or -1 (no queue / no matching input / OOM).
   The label is TRUSTED by the caller -- registry_heal will retrain on it. */
int registry_supply_label(PrimitiveRegistry *reg, const char *name,
                          const double *input, const double *target);

/* Teacher labeling (target source B): for each UNLABELED failure of `name`, if
   any OTHER registry primitive with matching dims/ports produces an in-domain
   (cleanly canonicalizing) output for that input, adopt it as the target and
   move the failure to LABELED. Returns the number labeled (0 if none/unknown). */
size_t registry_label_via_teacher(PrimitiveRegistry *reg, const char *name);
```

- [ ] **Step 2: Implement (src/router.c).** Add after `registry_pending_labels`. First a shared helper to append a labeled pair and a helper to drop an unlabeled row:
```c
static int retrain_queue_add_labeled(RetrainQueue *q,
                                     const double *input, const double *target) {
    size_t ic = q->input_count, oc = q->output_count;
    if (q->labeled_count == q->labeled_cap) {
        size_t ncap = q->labeled_cap == 0 ? 4 : q->labeled_cap * 2;
        double *ni = realloc(q->labeled_inputs, ncap * ic * sizeof(double));
        double *nt = realloc(q->labeled_targets, ncap * oc * sizeof(double));
        if (ni == NULL || nt == NULL) { free(ni == NULL ? NULL : ni); return -1; }
        q->labeled_inputs = ni;
        q->labeled_targets = nt;
        q->labeled_cap = ncap;
    }
    memcpy(q->labeled_inputs + q->labeled_count * ic, input, ic * sizeof(double));
    memcpy(q->labeled_targets + q->labeled_count * oc, target, oc * sizeof(double));
    q->labeled_count++;
    return 0;
}

/* Remove unlabeled row `r` by swapping the last row into its slot. */
static void retrain_queue_drop_unlabeled(RetrainQueue *q, size_t r) {
    size_t ic = q->input_count, oc = q->output_count, last = q->unlabeled_count - 1;
    if (r != last) {
        memcpy(q->unlabeled_inputs + r * ic,
               q->unlabeled_inputs + last * ic, ic * sizeof(double));
        memcpy(q->unlabeled_raw + r * oc,
               q->unlabeled_raw + last * oc, oc * sizeof(double));
    }
    q->unlabeled_count--;
}

int registry_supply_label(PrimitiveRegistry *reg, const char *name,
                          const double *input, const double *target) {
    size_t idx, r, k;
    RetrainQueue *q;
    if (reg == NULL || name == NULL || input == NULL || target == NULL) return -1;
    idx = registry_find(reg, name);
    if (idx == reg->count) return -1;
    q = reg->entries[idx].queue;
    if (q == NULL) return -1;
    for (r = 0; r < q->unlabeled_count; ++r) {
        int match = 1;
        for (k = 0; k < q->input_count; ++k) {
            if (q->unlabeled_inputs[r * q->input_count + k] != input[k]) { match = 0; break; }
        }
        if (match) {
            if (retrain_queue_add_labeled(q, input, target) != 0) return -1;
            retrain_queue_drop_unlabeled(q, r);
            return 0;
        }
    }
    return -1;  /* no matching parked input */
}

size_t registry_label_via_teacher(PrimitiveRegistry *reg, const char *name) {
    size_t idx, t, r, labeled = 0;
    RetrainQueue *q;
    if (reg == NULL || name == NULL) return 0;
    idx = registry_find(reg, name);
    if (idx == reg->count) return 0;
    q = reg->entries[idx].queue;
    if (q == NULL) return 0;

    r = 0;
    while (r < q->unlabeled_count) {
        const double *in = q->unlabeled_inputs + r * q->input_count;
        int labeled_this = 0;
        for (t = 0; t < reg->count; ++t) {
            BinaryTransformNetwork *cand = reg->entries[t].btn;
            const double *raw;
            double target[64];
            if (t == idx || cand == NULL) continue;
            if (cand->input_count != q->input_count ||
                cand->output_count != q->output_count ||
                cand->input_port_count != 1 || cand->output_port_count != 1) continue;
            if (!port_compatible(reg->entries[idx].btn->input_ports[0], cand->input_ports[0])) continue;
            if (!port_validate(cand->input_ports[0], in)) continue;
            raw = btn_forward(cand, in);
            if (!port_validate(cand->output_ports[0], raw)) continue;
            if (q->output_count > 64) break;  /* target buffer guard */
            if (port_canonicalize(cand->output_ports[0], raw, target) != 0) continue;
            if (retrain_queue_add_labeled(q, in, target) != 0) break;
            retrain_queue_drop_unlabeled(q, r);  /* swaps last into r; do NOT ++r */
            labeled++;
            labeled_this = 1;
            break;
        }
        if (!labeled_this) ++r;
    }
    return labeled;
}
```

- [ ] **Step 3: Tests (tests/test_lifecycle.c).** Add both and call them:
```c
static void test_supply_label_external(void) {
    BinaryTransformNetwork p = {0};
    PrimitiveRegistry reg;
    double in_bits[4] = {1.0, 0.0, 1.0, 0.0};
    double raw[1] = {0.5};
    double target[1] = {1.0};

    printf("lifecycle: external-oracle supply_label:\n");
    if (make_btn(&p, 4, 1, P(PORT_BINARY_MSB, 4, 1), P(PORT_BINARY_MSB, 1, 1)) != 0) {
        CHECK(0, "synthetic primitive setup"); return;
    }
    registry_init(&reg);
    registry_add(&reg, &p, "p");
    registry_record_fault(&reg, "p", in_bits, raw);
    CHECK(registry_supply_label(&reg, "p", in_bits, target) == 0, "supply_label ok");
    CHECK(registry_pending_labels(&reg, "p") == 0, "no unlabeled left");
    CHECK(reg.entries[0].queue != NULL && reg.entries[0].queue->labeled_count == 1,
          "one labeled exemplar");
    double other[4] = {0.0, 0.0, 0.0, 0.0};
    CHECK(registry_supply_label(&reg, "p", other, target) == -1,
          "no matching parked input -> -1");
    registry_free(&reg);
    btn_free(&p);
}

static void test_label_via_teacher(void) {
    BinaryTransformNetwork p = {0};
    BinaryTransformNetwork teacher = {0};
    PrimitiveRegistry reg;
    double in_bits[4] = {1.0, 0.0, 1.0, 0.0};
    double raw[1] = {0.5};
    size_t h;

    printf("lifecycle: teacher labeling:\n");
    if (make_btn(&p, 4, 1, P(PORT_BINARY_MSB, 4, 1), P(PORT_BINARY_MSB, 1, 1)) != 0 ||
        make_btn(&teacher, 4, 1, P(PORT_BINARY_MSB, 4, 1), P(PORT_BINARY_MSB, 1, 1)) != 0) {
        CHECK(0, "synthetic primitive setup"); return;
    }
    /* teacher: deterministic in-domain output (strong bias -> 1.0). */
    for (h = 0; h < teacher.max_hidden_count; ++h) teacher.hidden_output_weights[h] = 0.0;
    teacher.output_bias[0] = 8.0;

    registry_init(&reg);
    registry_add(&reg, &p, "p");
    registry_add(&reg, &teacher, "teacher");
    registry_record_fault(&reg, "p", in_bits, raw);
    CHECK(registry_label_via_teacher(&reg, "p") == 1, "teacher labeled one fault");
    CHECK(registry_pending_labels(&reg, "p") == 0, "no unlabeled left");
    CHECK(reg.entries[0].queue->labeled_count == 1 &&
          reg.entries[0].queue->labeled_targets[0] == 1.0,
          "labeled target is the teacher's canonical output (1.0)");
    registry_free(&reg);
    btn_free(&p);
    btn_free(&teacher);
}
```

- [ ] **Step 4: Build + run → pass.**

---

### Task M4: `registry_heal` — the wake pass + the safety guarantee

**Files:** `include/contract.h`, `src/contract.c`, `tests/test_lifecycle.c`.

- [ ] **Step 1: Declaration (include/contract.h).** After `registry_add_certified`:
```c
/* Wake pass: attempt to repair the RESET primitive `name` against `contract`.
   If it has no LABELED retrain exemplars, it stays RESET (we never retrain
   without a verified target) and 0 is returned. Otherwise it is retrained
   (fixed-epoch btn_train) on the contract's exemplars UNION its labeled queue,
   then re-certified against `contract`. On a passing btn_certify it is restored
   to PRIM_FROZEN (certified = 1), its labeled queue is cleared, and its
   reliability counters are reset (the weights changed); 1 is returned. On a
   failing certify it stays PRIM_RESET and 0 is returned. The ONLY path back to
   FROZEN is a passing certify. Returns -1 on bad args / unknown name / OOM. */
int registry_heal(PrimitiveRegistry *reg, const char *name,
                  const Contract *contract, size_t max_epochs);
```

- [ ] **Step 2: Implement (src/contract.c).** Add after `registry_add_certified`. It reaches `RetrainQueue` via `router.h` (already included by contract.h) and `btn_train` via `nn.h`:
```c
int registry_heal(PrimitiveRegistry *reg, const char *name,
                  const Contract *contract, size_t max_epochs) {
    size_t idx, ic, oc, n_con, n_lab, total, i;
    RetrainQueue *q;
    BinaryTransformNetwork *btn;
    double *inputs = NULL;
    double *targets = NULL;
    int certified_ok;

    if (reg == NULL || name == NULL || contract == NULL) return -1;
    for (idx = 0; idx < reg->count; ++idx) {
        if (reg->entries[idx].name != NULL &&
            strcmp(reg->entries[idx].name, name) == 0) break;
    }
    if (idx == reg->count) return -1;
    if (reg->entries[idx].state != PRIM_RESET) return 0;   /* nothing to heal */

    q = reg->entries[idx].queue;
    if (q == NULL || q->labeled_count == 0) return 0;       /* no verified target: stay RESET */

    btn = reg->entries[idx].btn;
    if (btn == NULL) return -1;
    ic = btn->input_count;
    oc = btn->output_count;
    n_con = contract->exemplar_count;
    n_lab = q->labeled_count;
    total = n_con + n_lab;

    inputs = malloc(total * ic * sizeof(double));
    targets = malloc(total * oc * sizeof(double));
    if (inputs == NULL || targets == NULL) { free(inputs); free(targets); return -1; }

    /* contract exemplars first, then the labeled queue */
    memcpy(inputs, contract->inputs, n_con * ic * sizeof(double));
    memcpy(targets, contract->outputs, n_con * oc * sizeof(double));
    memcpy(inputs + n_con * ic, q->labeled_inputs, n_lab * ic * sizeof(double));
    memcpy(targets + n_con * oc, q->labeled_targets, n_lab * oc * sizeof(double));

    (void)btn_train(btn, inputs, targets, total, max_epochs);
    free(inputs);
    free(targets);

    certified_ok = (btn_certify(btn, contract, NULL) == 0);
    if (!certified_ok) {
        return 0;   /* stays RESET -- never restore FROZEN without a passing certify */
    }

    /* Heal succeeded: restore FROZEN, clear labeled queue, reset stale evidence. */
    reg->entries[idx].state = PRIM_FROZEN;
    reg->entries[idx].certified = 1;
    q->labeled_count = 0;
    btn->output_successes = 0;
    btn->output_failures = 0;
    for (i = 0; i < q->unlabeled_count; ++i) { /* keep any still-unlabeled rows */ }
    return 1;
}
```
(The trailing loop is a no-op placeholder for readability; you may omit it. Unlabeled rows are intentionally retained — they still lack verified targets.)

- [ ] **Step 3: Tests (tests/test_lifecycle.c).** Add all three and call them. They build tiny 1-bit identity contracts so training converges fast and deterministically.
```c
/* Build a 1-bit identity contract (0->0, 1->1) borrowed against `p`. */
static int make_identity_contract(Contract *c, const BinaryTransformNetwork *p,
                                  double *ins, double *outs) {
    ins[0] = 0.0; outs[0] = 0.0;
    ins[1] = 1.0; outs[1] = 1.0;
    return contract_init_borrowed(c, "id1", p, ins, outs, 2);
}

static void test_heal_restores_frozen(void) {
    BinaryTransformNetwork p = {0};
    PrimitiveRegistry reg;
    Contract c = {0};
    double ins[2], outs[2];
    double fin[1] = {1.0};
    double fraw[1] = {0.5};
    double ftgt[1] = {1.0};
    Port b1 = P(PORT_BINARY_MSB, 1, 1);

    printf("lifecycle: heal retrains + re-certifies -> FROZEN:\n");
    if (btn_init(&p, 1, 1, 4, 4, 0.5, 7u) != 0 || btn_set_ports(&p, b1, b1) != 0) {
        CHECK(0, "synthetic primitive setup"); return;
    }
    CHECK(make_identity_contract(&c, &p, ins, outs) == 0, "identity contract built");

    registry_init(&reg);
    registry_add(&reg, &p, "p");
    registry_record_fault(&reg, "p", fin, fraw);          /* RESET + unlabeled */
    CHECK(registry_supply_label(&reg, "p", fin, ftgt) == 0, "labeled the fault");

    CHECK(registry_heal(&reg, "p", &c, 20000) == 1, "heal succeeds");
    CHECK(reg.entries[0].state == PRIM_FROZEN, "primitive restored to FROZEN");
    CHECK(reg.entries[0].certified == 1, "primitive marked certified");
    CHECK(reg.entries[0].queue->labeled_count == 0, "labeled queue cleared");

    contract_free(&c);
    registry_free(&reg);
    btn_free(&p);
}

static void test_heal_no_label_stays_reset(void) {
    BinaryTransformNetwork p = {0};
    PrimitiveRegistry reg;
    Contract c = {0};
    double ins[2], outs[2];
    double fin[1] = {1.0};
    double fraw[1] = {0.5};
    Port b1 = P(PORT_BINARY_MSB, 1, 1);

    printf("lifecycle: heal without a verified target stays RESET:\n");
    if (btn_init(&p, 1, 1, 4, 4, 0.5, 7u) != 0 || btn_set_ports(&p, b1, b1) != 0) {
        CHECK(0, "synthetic primitive setup"); return;
    }
    make_identity_contract(&c, &p, ins, outs);
    registry_init(&reg);
    registry_add(&reg, &p, "p");
    registry_record_fault(&reg, "p", fin, fraw);   /* unlabeled only */

    CHECK(registry_heal(&reg, "p", &c, 20000) == 0, "heal is a no-op without a label");
    CHECK(reg.entries[0].state == PRIM_RESET, "primitive stays RESET");

    contract_free(&c);
    registry_free(&reg);
    btn_free(&p);
}

static void test_heal_cannot_certify_stays_reset(void) {
    BinaryTransformNetwork p = {0};
    PrimitiveRegistry reg;
    Contract c = {0};
    /* Contradictory contract: input 1 -> 0 AND input 1 -> 1. Cannot be certified. */
    double ins[2] = {1.0, 1.0};
    double outs[2] = {0.0, 1.0};
    double fin[1] = {1.0};
    double fraw[1] = {0.5};
    double ftgt[1] = {1.0};
    Port b1 = P(PORT_BINARY_MSB, 1, 1);

    printf("lifecycle: heal that can't certify stays RESET (the guarantee):\n");
    if (btn_init(&p, 1, 1, 4, 4, 0.5, 7u) != 0 || btn_set_ports(&p, b1, b1) != 0) {
        CHECK(0, "synthetic primitive setup"); return;
    }
    CHECK(contract_init_borrowed(&c, "bad1", &p, ins, outs, 2) == 0, "contradictory contract built");
    registry_init(&reg);
    registry_add(&reg, &p, "p");
    registry_record_fault(&reg, "p", fin, fraw);
    registry_supply_label(&reg, "p", fin, ftgt);

    CHECK(registry_heal(&reg, "p", &c, 20000) == 0, "heal returns 0 (certify failed)");
    CHECK(reg.entries[0].state == PRIM_RESET, "primitive stays RESET");
    CHECK(reg.entries[0].certified == 0, "primitive is NOT certified");

    contract_free(&c);
    registry_free(&reg);
    btn_free(&p);
}
```

- [ ] **Step 4: Build + run → pass.** If `test_heal_restores_frozen` does not certify within 20000 epochs, raise the epoch cap or simplify the contract (1-bit identity is near-trivial; report rather than weaken the assertion). `make test` ends `ALL TESTS PASSED (single exe)`.

---

## Self-review
- **Triggers:** strict-fault path via `registry_record_fault` (M2); runtime law-violation trigger deferred (documented). ✓
- **Scope C targets:** A = contract (used by `registry_heal`); B = `registry_label_via_teacher` (M3); C = `registry_supply_label` + `registry_pending_labels` (M2/M3). ✓
- **Retrain queue:** labeled + unlabeled sections, owned by `RegistryEntry`, freed in `registry_free`. ✓
- **Wake pass:** trains on contract ∪ labeled, re-certifies, FROZEN-or-stays-RESET, resets stale evidence; **never restores FROZEN without a passing `btn_certify`** (M4 + the `test_heal_cannot_certify_stays_reset` guarantee test). ✓
- **Prereq:** NULL-name guard fixed (M1). ✓
- **Type consistency:** `RetrainQueue` fields and `registry_*` signatures match between router.h declarations and router.c/contract.c definitions; `registry_find` shared within router.c; heal reads `q->labeled_*` consistent with how M2/M3 fill them. ✓
- **Placeholders:** none — every step has complete code. ✓
