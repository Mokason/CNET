# Integrity Slice — Transactional Model Residency

**Place:** `src/model_runtime.c`.

**Dilemma:** unload callbacks run under the manager mutex and relocation destroys healthy residency before replacement succeeds.

**Consequence:** backend re-entry deadlocks; failed placement/load causes avoidable outage.

**Systematic component:** deterministic mock backends, generation/handle assertions and watchdogs. **Noise:** thread scheduling, bounded by condition-variable assertions and TSan.

## TDD

1. Add a reentrant unload callback test that calls a manager read API; current code must time out/fail.
2. Add a failed relocation test asserting old handle, generation and residency remain usable.
3. Refactor state transitions so external unload callbacks execute outside the mutex.
4. Implement prepare/load/commit-or-rollback replacement semantics.
5. Preserve lease, pin, accounting and cold-load dedup invariants.
6. Focused marker: existing `MODEL_RUNTIME_PASS` plus new named assertions.

## Verified Closure

- Selected bystanders are transaction-pinned but remain fully resident until commit.
- Failed replacement loads preserve every bystander handle, state, generation,
  eviction count and resource-accounting field.
- Successful replacements detach victims and publish the new residency under
  one mutex hold; all unload callbacks run afterward and can re-enter manager APIs.
- Actual-size rejection also defers handle destruction until rollback is visible.
- `-Werror` and ASan/UBSan pass 146 assertions. TSan compilation succeeds, but
  this host runtime aborts before tests with `unexpected memory mapping`.
- Focused authority: `make --no-print-directory model_runtime_integrity` emits
  `MODEL_RUNTIME_INTEGRITY_GATE_PASS`.
