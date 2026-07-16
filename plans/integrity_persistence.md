# Integrity Slice — Crash-Safe Persistence

**Place:** `src/agent_memory.c`, registry/CNB restart and SoulHost.

**Dilemma:** the live knowledge base is truncated in place, writes are unchecked, and CNB restart does not restore persisted registry policy/expansion sidecars.

**Consequence:** process loss can destroy memory; restart can change planner behavior.

**Systematic component:** fault injection, checksummed publication and save/reopen comparison. **Noise:** filesystem scheduling, reduced to old-or-new atomic outcomes.

## TDD

1. Add a failure-injection persistence test; current direct rewrite must expose a partial publication path.
2. Publish through same-directory temp file, checked writes, flush/fsync and atomic replace; retain the old valid generation on failure.
3. Add configurable KB directory isolation.
4. Add save→SoulHost reopen coverage for global policy and per-entry expansion restoration.
5. Pick one documented canonical checkpoint path and delete/deprecate contradictory dead semantics.
6. Focused marker: `PERSISTENCE_INTEGRITY_PASS`.

## Verified Closure

- Agent KB writes use checked writes to a same-directory exclusive temp file, `fflush`/`fsync`, atomic replacement and parent-directory sync.
- A hard-linked verified previous generation survives successful publication; corrupt-current reopen recovers that generation, while injected write failure leaves the current generation untouched.
- Registry global policy and per-entry expansion recipes restore together; malformed global policy and malformed sidecars fail closed during SoulHost reopen.
- Focused normal and ASan/UBSan runs pass; `make persistence_integrity` emits `PERSISTENCE_INTEGRITY_GATE_PASS`.
