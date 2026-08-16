# Core Attribution Layer — telling proposer failure apart from system failure

Status: DESIGN (no code yet). Date: 2026-08-16.
Origin: "we be need to differentiate it in parts. else one part will
constantly reject other because it does not align with its capsules and
certification while other can end up hallucinating, to a point we get
incoherent responses that could poison whole system. so, that is why we need
to make a solid core that can work as middle ground."

Scope: v1 of the **core** in the brain-split architecture — the mediator
between the creative hemisphere (LLM proposer/oracle) and the logical
hemisphere (certified capsules). This spec covers attribution bookkeeping and
calibration measurement ONLY. It is report-only by construction.

---

## 1. The gap (grounded in the tree)

Both failure modes the origin describes have already occurred in this repo:

| failure | occurrence |
|---|---|
| logical side rejects everything | the fuzzy-tier rc-vs-verdict drain bug — every SAMPLED cert deferred; visible only as "no gaps closed" |
| creative side emits degenerate output | gemma draft globally constant → PAIR tier degenerate |

Neither was diagnosable from the recorded state, and that is the actual defect.
Verified against source:

| claimed capability | reality |
|---|---|
| per-oracle outcome counters | **REAL** — `OracleEntry.calls/rejects/abstains/seals_produced/unfit_closes` ([include/acquire.h](../../../include/acquire.h)) |
| auto-retire on bad teachers | **REAL** — `OraclePolicy.retire_unfit_rate` + `acquire_oracle_apply_retire_policy` |
| teacher leasing | **REAL** — `acquire_oracle_bind`, `lease_gen`, `require_lease_to_teach` |
| per-domain attribution | **ABSENT** — every counter above is global per oracle |
| refusal cause detail | **THIN** — `last_defer_reason` is one `ACQUIRE_REASON_MAX` (64) atom; the histogram in `AcquireReport` is 4 buckets: `waiting_oracle`, `oracle_unfit`, `certify_failed`, `other` |

The consequence: a proposer excellent on one signature and hallucinating on
another carries one blended number, so `retire_unfit_rate` can only retire it
everywhere or nowhere. And `certify_failed` — the bucket that absorbs most
real failures — cannot distinguish *bad labels* from *student too small* from
*domain not learnable*. `GapRecord.recipe_fp` is the only causal attribution
in the system and it is one bit.

**One claim from the original framing does not survive contact:** the fix is
not "a bigger core." The core is already the mediator — `acquire_drain` sits
between the hemispheres today and enforces total DEFER. What it lacks is
*memory of the disagreement*. This spec adds that memory, nothing else.

## 2. The key structural finding

**The blame boundary already exists in the drain pipeline. It is the stage at
which the attempt died.**

`acquire_drain`'s documented order is: mine → oracle-evidence gate → train →
holdout check (sampled mode) → certify exhaustive → seal → register → replan.

The oracle-evidence gate (`AcquireConfig.evidence_threshold`, default 0.9,
and `min_evidence`, default 16) fires **before a single training step**. So:

- dying at or before that gate ⇒ the labels were unusable ⇒ **proposer fault**
- dying after clearing it ⇒ the labels were fine and the student could not fit
  ⇒ **system fault** (recipe or domain)

Attribution is therefore *recording which stage killed the attempt*, not a new
judgment step. No new inference, no heuristic, no model.

## 3. Data model

### 3.1 Key

`(proposer name, goal Port signature)` where the signature is the full
`(family, field_width, field_count, tag)` tuple compared under the existing
exact-equality semantics of `acquire_port_eq_public`.

The `input_port` is recorded in the event log but is NOT part of the key.
Keeping it in the raw events means the data can be re-keyed later without
re-running a campaign (see §6).

### 3.2 Verdicts

Every attempt terminates in exactly one verdict:

| verdict | stage of death |
|---|---|
| `ADMITTED` | reached CLOSED — sealed and registered |
| `BLAMELESS` | `waiting_oracle` — no proposer matched the signature |
| `PROPOSER_FAULT` | evidence gate (validity below `evidence_threshold`, or usable exemplars below `min_evidence`), pilot `class_imbalance`, `tag_collision`, `oracle_unfit` |
| `SYSTEM_FAULT` | holdout or certify failure *after* clearing the evidence gate |

`BLAMELESS` is excluded from every denominator. Charging a proposer for a gap
it was never offered would make the trust number meaningless.

### 3.3 The two posteriors

Both use the Laplace prior already used for unit reliability, so a cold key
reads 0.5 (neutral) rather than blind:

```
proposer_trust    = (admitted + system_fault + 1)
                    / (admitted + system_fault + proposer_fault + 2)

signature_yield   = (admitted + 1)
                    / (admitted + system_fault + 2)
```

`SYSTEM_FAULT` sits in the trust numerator because it is explicitly not the
proposer's fault — the labels cleared the evidence gate.

This is the differentiation the origin asks for, made numeric. Low
`proposer_trust` with healthy `signature_yield` elsewhere means the creative
side is hallucinating on this signature. Healthy `proposer_trust` with low
`signature_yield` means the logical side cannot learn this signature — and
that is not a reason to distrust the proposer.

Recipe-vs-domain splits off `GapRecord.recipe_fp`: certify failures spread
across several distinct fingerprints indicate the domain; repeated failure
under a single fingerprint indicates the recipe.

### 3.4 Anti-triviality

Each `ADMITTED` also records the certified domain's cardinality — the same
count the coverage layer uses to decide PROOF vs SAMPLED, including abstained
points — and the attempt's `AcquireReport.last_min_margin`, together with
whether the verdict was PROOF or SAMPLED. Reported as columns so that a perfect
admission rate over constant or tiny domains does not read as excellence.
This is the sensor for the reward-hacking pattern the gemma PAIR run already
produced, and it matters ahead of any future RL loop over the creative side.

v1 reports these; it does not weight the posteriors by them.

## 4. Conformal calibration — and its honest limit

The oracle is the ground-truth source, so it cannot be calibrated against
itself. Usable ground truth, in order of availability:

1. **`port_validate` failure** — definitively wrong, always available, free
   (already counted in `OracleEntry.rejects`). High `confidence` on a point
   that fails port validation is unambiguous miscalibration.
2. **Registered `CnetOracleValidateFn` verdict** — `CNET_ORACLE_VALID` /
   `CNET_ORACLE_INVALID`; semantic validity, independent of the answer.
   `CNET_ORACLE_VALIDITY_UNDETERMINED` is excluded from calibration.
3. **Neither available** ⇒ report `uncalibratable`. Do NOT emit a threshold.

With labels from (1) and/or (2), `conformal_quantile`
([include/contract/conformal.h](../../../include/contract/conformal.h)) over
`score = 1 - CnetOracleResult.confidence` yields the threshold below which
the proposer *should* abstain to hold marginal error at or below alpha.

Reported per key: the threshold `q`, `n_calib`, and the divergence between
that threshold and the proposer's self-reported abstain behaviour
(`CNET_ORACLE_ABSTAIN_AMBIGUOUS` counts).

**Why this is in v1 at all:** the `ABSTAIN` path is the system's anti-poison
valve — `CNET_ORACLE_ABSTAIN_AMBIGUOUS` excludes a point from the certified
domain rather than forcing the student to memorize a coin-flip. That valve
depends entirely on the proposer's decision margin meaning something.
Aggressive quantization is precisely what destroys margin calibration, so a
future ternary creative hemisphere would disable the valve *silently*. This
measures the valve before anything is swapped underneath it.

**Stated limitations, not to be papered over:**
- Path (1) alone gives a weak threshold when few outputs fail port validation.
  `n_calib` is reported so this is visible rather than implied.
- Split conformal assumes exchangeability of calibration and test points.
  Mined points drawn by deterministic stride sampling are not adversarially
  ordered, but this assumption is stated, not proven.
- No calibration is possible for a proposer with no validator and no
  representation rejects. `uncalibratable` is the correct output.

Side effect worth naming: this makes registering a `CnetOracleValidateFn`
directly valuable, which the existing `OraclePolicy.require_validator` knob
already wants for production.

## 5. Interface

A single callback on `AcquireConfig`, mirroring the existing `on_close`
pattern:

```c
void (*on_attempt)(const AttributionEvent *ev, void *ctx);
void *on_attempt_ctx;
```

`NULL` = off, and that is the default. The event carries: proposer name,
input port, goal port, stage, verdict, `recipe_fp`, domain cardinality,
min margin, oracle confidence, and the calibration ground-truth label where
one was available.

New unit: `include/attribution.h` + `src/attribution.c`. It is a pure sink —
`acquire_drain` never reads back from it. That is what makes the report-only
guarantee provable rather than asserted (§7.1).

Deliberately NOT extending `OracleEntry`: it lives in a fixed
`ACQUIRE_MAX_ORACLES` (32) array and is currently flat, so a per-signature
table inside it forces either a hard cap or a heap pointer into a struct that
is copied. More importantly, it would weld "who may teach" onto "who is
trusted" — the exact entanglement this spec exists to separate.

## 6. Persistence

Two files, both sidecars under CNET_STATS rules — never inside a weight file,
load REPLACES, missing or malformed returns -1 with state untouched:

- **`CNET_ATTRIB 1`** — the posterior table. Loaded on the hot path.
- **`CNET_ATTRIB_LOG 1`** — append-only raw events. Never loaded on the hot
  path; read only by the report tool.

Checkpoint order: base → gap ledger → attribution. Attribution is written
last so a persisted count never claims work the ledger does not hold. This
mirrors the existing rule that the gap ledger is saved after the base.

The event log exists specifically because the blame taxonomy in §3.2 is the
part most likely to be wrong on the first attempt. With raw events retained,
posteriors can be recomputed under revised rules — including re-keying to
include `input_port` — without re-running a mining campaign to regather
evidence.

## 7. Gates

### 7.1 The no-op gate (the load-bearing one)
Run an identical drain with and without `on_attempt` attached. Base bytes,
registry contents, all counters, and the gap ledger must be byte-identical.
This *is* the report-only guarantee; without it, "report-only" is a claim
rather than a property.

### 7.2 Verdict classification
Synthetic drains constructed to die at each stage (oracle absent, evidence
gate, degenerate pilot, tag collision, holdout, certify) assert the resulting
verdict.

### 7.3 Posterior boundaries
Zero evidence reads exactly 0.5. `BLAMELESS` appears in neither denominator.
A key with only `SYSTEM_FAULT` records has `proposer_trust` above 0.5 and
`signature_yield` below it.

### 7.4 Conformal
`n_calib` below threshold reports `uncalibratable` rather than a fabricated
`q`. Score-level core behaviour is already covered by existing conformal
tests; this adds only the oracle-confidence adapter.

### 7.5 Sidecar
Round-trip digest identity. Malformed file leaves state untouched. Point the
existing `make mutate` byte-flip/truncation harness at the new parser — the
same harness found two real crash bugs in `cce_gguf.c`.

### 7.6 Integration
Wired into `make verify`.

## 8. Error handling

Governing rule: **attribution must never be able to break acquisition.**

- Sink allocation failure, full key table, or oversized event ⇒ bump
  `dropped_events` and continue. A lost observation is acceptable; a lost
  unit is not.
- Memory stays bounded by refusing new keys, never by evicting existing ones
  — eviction would silently corrupt a posterior.
- `dropped_events` is reported, so degraded observation is visible.

## 9. Explicitly out of scope for v1

- No throttling, gating, or change to oracle selection
- No enforcement of the conformal threshold
- No posterior weighting by cardinality or margin
- No tag minting by the creative hemisphere
- No change to the oracle ABI
- No RL loop over the proposer

## 10. What this unblocks

v2 authority (throttle a proposer on the signatures where it is untrusted
while leaving it teaching elsewhere) becomes a matter of reading numbers that
have already been validated in report-only mode — rather than tuning
attribution and observing its effects simultaneously.

It also puts measurement in place before the creative hemisphere is replaced.
The ternary-distillation arc depends on the `ABSTAIN` valve continuing to
work under quantization, and §4 is how that gets checked rather than assumed.
Sequencing follows from this: validate the core against the gemma oracle that
is already trusted, and only then swap the proposer underneath it.
