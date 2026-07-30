# CNET integrity remediation — 2026-07-30

Source of defects: `CNET_REANALYSIS_2026-07-30.md` (unified Hermes verdict) and
the Codex adversarial audit. Base commit `3edac49`, branch
`feature/cnet-integrity-remediation`.

Every slice below is strict vertical RED → GREEN. **No certification floor was
lowered, no negative test weakened, and no FAILED/WITHHELD result renamed.**
Where a claim could not be earned it is recorded as still failing rather than
softened.

Evidence labelling used throughout:

* **FRESH** — the command was executed during this patch and its output/exit
  code is transcribed here.
* **CARRIED** — a prior result quoted for context, not re-executed. Carried
  results are never used as acceptance evidence for a slice.

All destructive fixtures (corruption, truncation, overwrite, parser, health)
live under `mkdtemp` roots. No real base, capsule, runtime state, home/config
path, or live service was written by any test added here.

---

## A1 — Capability fixtures must be causal

### Defect

`tests/run_capability_cert.py` exported `CNET_HELD_OUT_FIXTURE`; repository-wide
search found no evaluator that read it. Declared held-out cases were decorative
metadata: the fixture SHA-256 in the report proved which file existed, never
which cases ran, and a capability with no `metric_regex` scored `1.0` from
marker presence alone.

### RED — FRESH

Pristine `HEAD` extracted read-only with `git archive HEAD | tar -x` into a
scratch tree, then the new causality gate and the id-bearing fixtures were
copied in so the mutation experiment could address stable case ids:

```
cd <scratch>/red-head
python3 tests/test_capability_fixture_causality.py \
    calibrated_abstention hybrid_skill_serve sleep_consolidation
```

Observed (exit **1**, 12 failures):

```
FAIL: calibrated_abstention: evaluator emits a HELDOUT_FIXTURE receipt
FAIL: calibrated_abstention: case low-margin-abstains reported as consumed
FAIL: calibrated_abstention: mutating case[0].expected -> 'answer_with_evidence' must fail the evaluator (rc=0)
FAIL: hybrid_skill_serve: evaluator emits a HELDOUT_FIXTURE receipt
FAIL: hybrid_skill_serve: mutating case[0].expected_authority -> 'certified' must fail the evaluator (rc=0)
FAIL: sleep_consolidation: evaluator emits a HELDOUT_FIXTURE receipt
FAIL: sleep_consolidation: mutating case[0].expected_semantic_promotions -> 4 must fail the evaluator (rc=0)
CAPABILITY_FIXTURE_CAUSALITY_FAIL failures=12
```

`rc=0` on every mutation run is the defect exactly as predicted by the
re-analysis: a semantically falsified fixture left the evaluator green.

### Fix

* `include/cnet_heldout.h` + `src/cnet_heldout.c` — a small total fixture reader
  with an inlined SHA-256 over the exact bytes parsed. Two modes: **standalone**
  (variable unset — every read returns the caller's fallback, nothing printed,
  so `make sleep_consolidate` by hand behaves as before) and **bound** (the
  fixture must load, must declare this capability, and every read must resolve).
  `cnet_heldout_finish()` prints the receipt and fails on any unresolved read,
  unconsumed case, or failed/unrecorded verdict.
* `dotnet/Cce.Llm.Tests/HeldOutFixture.cs` — the C# counterpart emitting
  byte-identical receipt lines, because `honest_memory_retrieval` is a `dotnet
  test` evaluator.
* Five C evaluators and one xUnit test now read their floors, shapes, queries
  and expectations **through** the fixture and record a per-case verdict.
* Every fixture case carries a stable unique `id`.
* `tests/run_capability_cert.py` refuses to certify without a receipt binding
  the exact fixture SHA-256, every declared case id, and `consumed == declared`;
  and binds each result to commit, working-tree digest, assume-unchanged count,
  evaluator argv, evaluator binary digest, declared source-set digest,
  environment digest, run UUID and start time.
* **The default metric is gone.** A manifest declares `metric_source`
  (`heldout_receipt` or `regex`); declaring neither, or both, is a validation
  error. Four capabilities that previously scored `1.0` from a marker now score
  `cases_passed / cases_declared` measured from the receipt.

### GREEN — FRESH

```
make capability_cert                        # exit 0
```

```
CAPABILITY_CERT_RUNNER_PASS shell=disabled receipt=required checks=21
CAPABILITY_FIXTURE_CAUSALITY_PASS capabilities=6 mutations_rejected=6 scope=all
CAPABILITY_CERT_RUN run_id=0841ffc0-8f35-4de1-b4a5-3f048afd3db7 started=2026-07-30T09:18:08Z commit=3edac49d706ec7fbc258b0ff59097d4941824929 worktree=93751a2625b32ce9 dirty=26 assume_unchanged=83
CAPABILITY id=calibrated_abstention status=certified metric=1.000 source=heldout_receipt receipt=ok
CAPABILITY id=cce_classification status=certified metric=0.861 source=regex receipt=ok
CAPABILITY id=honest_memory_retrieval status=certified metric=1.000 source=heldout_receipt receipt=ok
CAPABILITY id=hybrid_skill_serve status=certified metric=1.000 source=heldout_receipt receipt=ok
CAPABILITY id=json_toolcall_adapter status=certified metric=0.738 source=regex receipt=ok
CAPABILITY id=sleep_consolidation status=certified metric=1.000 source=heldout_receipt receipt=ok
CAPABILITY_CERT_PASS certified=6/6 run_id=0841ffc0-8f35-4de1-b4a5-3f048afd3db7 commit=3edac49d706ec7fbc258b0ff59097d4941824929
```

`assume_unchanged=83` is deliberate disclosure: this worktree contains a
pre-existing CRLF/index defect contained with assume-unchanged flags, and
`git status` cannot see those paths. A report that silently omitted them would
overstate how tightly the result is bound to the tree.

### Regression commands — FRESH

```
make heldout_fixture_test        # CNET_HELDOUT_TEST_PASS checks=42, exit 0
python3 tests/test_capability_cert_runner.py   # 21 tests OK, exit 0
make semantic_cortex             # SEMANTIC_CORTEX_PASS, exit 0 (standalone mode)
make sleep_consolidate           # SLEEP_CONSOLIDATE_PASS, exit 0 (standalone mode)
make calibrated_governance       # CALIBRATED_GOVERNANCE_PASS, exit 0 (standalone mode)
make jtc_adapter_bench           # JTC_ADAPTER_BENCH_PASS acc_on=0.7375, exit 0
```

### What this does NOT establish

The certificate now proves that the declared cases ran and that mutating one
fails it. It does **not** turn the four binary capabilities into graded
measurements: `metric=1.000` there means "all declared cases passed", and the
declared case count is small (1–3 per capability). The scientific weight of
`capability_cert` is still bounded by how good those cases are, and the
re-analysis verdict that *useful* capability remains WITHHELD is unchanged.

---

## A2 — Make execution semantics

### Defect

The Makefile declared no `SHELL`/`.SHELLFLAGS`, so no recipe had `pipefail`,
while 47 recipes ran `producer | tee log` followed by a marker `grep`. `tee`
exits 0 regardless of what the producer did, so a crash, timeout, sanitizer
teardown failure, or late cleanup error *after* the PASS marker was printed
became a green gate. The static `recipe_gate` missed all of it — it only looked
for `|| echo`, `|| true` and `|| :` — yet still emitted a broad
`RECIPE_GATE_PASS`. Separately, five headline action gates were not `.PHONY`, so
a same-named root file could make Make report them up to date without running
anything; and `vision_detection_bench_v2` accepted
`VISION_DETECTION_MECHANISM_PASS` **or** `..._WITHHELD` and exited 0 either way.

### RED — FRESH

```
bash tests/test_pipeline_status.sh      # exit 1
sh tests/test_recipe_gates.sh           # exit 1
```

Observed:

```
FAIL: .../Makefile declares no SHELL/.SHELLFLAGS; pipelines cannot propagate producer status
FAIL: Makefile must enable pipefail so producer status survives a pipe
FAIL: shape 'tee_stderr_grep' must reject a marker-then-exit-7 producer (exit 0)
FAIL: shape 'tee_plain' must reject a marker-then-exit-7 producer (exit 0)
FAIL: shape 'tee_append' must reject a marker-then-exit-7 producer (exit 0)
FAIL: shape 'pipe_grep' must reject a marker-then-exit-7 producer (exit 0)
FAIL: shape 'timeout_tee' must reject a marker-then-exit-7 producer (exit 0)
PIPELINE_STATUS_FAIL checks=12 failures=7

RECIPE_GATE: FAIL — no bash SHELL declared; pipefail is unavailable.
```

The mutation fixture is a producer that prints
`SYNTHETIC_GATE_PASS checks=1 metric=1.000` and then `exit 7`. Note that the
`status_capture` shape (`prog > log; rc=$?; cat log; test $rc -eq 0 && grep ...`)
already rejected it — the defect was specific to pipelines.

### Fix

* `SHELL := /bin/bash` and `.SHELLFLAGS := -o pipefail -c` at the top of the
  Makefile. Deliberately **not** `-e`: recipes rely on Make checking each line,
  and errexit would change control flow inside multi-command lines.
* `.PHONY` added for `knowledge_composition_bench`,
  `knowledge_accumulation_bench`, `knowledge_capsule`, `coverage_abstain`,
  `own_learning_health`, `port_raw_unit_seam`, `vision_coverage_test`,
  `vision_capsule_asset`, `vision_detection_bench_v2(_evidence)`,
  `vision_detection_prep_v2`.
* `tests/test_recipe_gates.sh` now *requires* the bash+pipefail declaration and
  enumerates 14 headline gates, rejecting any that is missing or not `.PHONY`.
  A renamed gate fails the check rather than passing vacuously.
* `scripts/benchmark_verdict.sh` maps a recorded verdict to an exit code:
  PASS 0, **WITHHELD 3**, BLOCKED 4, no verdict 1. The verdict word is not
  renamed — WITHHELD stays WITHHELD and simply stops sharing PASS's exit status.
  `vision_detection_bench_v2` is now the gate (uses the mapping) and
  `vision_detection_bench_v2_evidence` is the separately named collector that
  may return 0 while recording WITHHELD.
* `recipe_gate` runs all three checks, so the false-green mutation executes on
  every invocation instead of being a one-off experiment.

**A limitation worth stating plainly:** GNU Make exits **2** for any failed
recipe, so a `make` invocation cannot itself surface 3-for-WITHHELD or
4-for-BLOCKED. The distinct code is available where automation should read it —
`sh scripts/benchmark_verdict.sh <log> <prefix>` — and through `make` the
guarantee is the weaker but still correct one: WITHHELD and BLOCKED are
non-zero, and PASS is the only zero. The marker line names which it was.

### GREEN — FRESH

```
bash tests/test_pipeline_status.sh
PIPELINE_STATUS_PASS checks=11 shapes=6 producer=marker_then_exit_7      # exit 0

sh tests/test_recipe_gates.sh
RECIPE_GATE_PASS: no swallowed executable exits; pipefail on for 48 pipeline(s); 14 headline gate(s) .PHONY.   # exit 0

sh tests/test_benchmark_verdict.sh
BENCHMARK_VERDICT_PASS checks=12 pass=0 withheld=3 blocked=4 no_verdict=1  # exit 0
```

### Regression commands — FRESH

Enabling `pipefail` globally can only turn previously-hidden failures red, so
every headline gate was re-run against the changed shell:

```
make recipe_gate                    exit 0
make coverage_abstain               exit 0
make knowledge_capsule              exit 0
make knowledge_accumulation_bench   exit 0
make knowledge_composition_bench    exit 0
make port_raw_unit_seam             exit 0
make vision_coverage_test           exit 0
make vision_capsule_asset           exit 0
```

No gate regressed, i.e. none of them was relying on a masked producer failure.

### BLOCKED in this worktree

`make vision_detection_bench_v2` **could not be executed**: `data/` does not
exist in this worktree, so the VOC2007 v2 cache is absent and the recipe stops
at its own `test -f data/vision_cache_v2/test.pack` guard. The WITHHELD→exit-3
mapping is therefore proven by `tests/test_benchmark_verdict.sh` against
synthetic logs, **not** by an end-to-end benchmark run. Recorded as BLOCKED
rather than claimed.

---

## A3 — own_learning_health strict deployed mode

### Defect

The watchdog certified an uninspectable deployment as healthy. It left
`base_ok=0` silently when the requested base was missing or unloadable, omitted
that from its danger checks, and printed `OWN_LEARNING_HEALTH_PASS` on zero
dangers. It also read booleans loosely — only exact `"0"` meant the coverage
gate was off and only exact `"1"` meant mining was on — so `CNET_COVERAGE_ABSTAIN=off`
reported the gate as **on**. A nonempty residual URL counted as a configured
teacher regardless of whether anything was listening.

### RED — FRESH

The re-analysis's exact reproduction, plus the boolean case, plus the real gate:

```
make own_learning_health
CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE=1 CNET_COVERAGE_ABSTAIN=1 \
CNET_RESIDUAL_HTTP=http://127.0.0.1:1 \
  ./bin/cnet_own_learning_health --base /definitely/not/a/cnet/base.cnb
CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE=yes CNET_COVERAGE_ABSTAIN=off \
  ./bin/cnet_own_learning_health --base /definitely/not/a/cnet/base.cnb
```

Observed — all three **exit 0**:

```
{"base":"/home/marble/AI/CNET/logs/personal.cnb","base_loaded":0,...,"dangerous":[],"status":"ok"}
OWN_LEARNING_HEALTH_PASS
make own_learning_health EXIT=0

{"base":"/definitely/not/a/cnet/base.cnb","base_loaded":0,...,"status":"ok"}
OWN_LEARNING_HEALTH_PASS
repro EXIT=0

{"base":"/definitely/not/a/cnet/base.cnb","base_loaded":0,...,"mine_on_serve":0,"coverage_gate":"on",...}
OWN_LEARNING_HEALTH_PASS
invalid-bool EXIT=0
```

Note the middle line of the third run: `MINE_ON_SERVE=yes` was reported as
`mine_on_serve:0` and `COVERAGE_ABSTAIN=off` as `coverage_gate:"on"` — both
typos silently resolved in the deployment's favour. Note also that the gate's
own default base, `$HOME/AI/CNET/logs/personal.cnb`, **does not exist on this
host**, so `make own_learning_health` was passing while inspecting nothing.

### Fix

`tools/cnet_own_learning_health.c`:

* **Strict deployed mode is the default.** New dangers: `base_not_specified`,
  `base_not_loaded`, `coverage_file_missing` (an armed gate with no sidecar is
  an unenforced guard, not "nothing to check"), `invalid_boolean_mine_on_serve`,
  `invalid_boolean_coverage_abstain`, `residual_http_unreachable`,
  `residual_gguf_unreadable`.
* Booleans must be exactly `"0"` or `"1"`. While a garbage value is reported it
  is also *interpreted at its most dangerous* (mining on, gate off), so a typo
  can never be resolved in the deployment's favour even for one line of output.
* A configured HTTP teacher is probed with a plain non-blocking TCP connect and
  a 2 s timeout — no request is sent and no body read, so it cannot perturb
  whatever is on the other end. A configured GGUF teacher must be readable.
* `--config-only` inspects knob relationships alone and emits
  **`CONFIG_ONLY_PASS` / `CONFIG_ONLY_FAIL`**, never the deployment marker,
  because it has not looked at a deployment. The JSON carries `"mode"` and the
  raw knob strings so a report cannot be misread as deployment health.

`tests/mk_test_base.c` builds a real loadable CNB (and optionally a valid
coverage sidecar) through the ordinary API, so the "base exists but the state
around it is wrong" cases test the loader rather than a hand-written fixture.

### GREEN — FRESH

```
bash tests/test_own_learning_health.sh
OWN_LEARNING_HEALTH_STRICT_PASS checks=33          # exit 0
```

33 checks covering: missing base, unloadable base, unnamed base, garbage
booleans (both knobs), dead configured teacher, armed gate with no sidecar,
mined unit with no record, truncated sidecar, config-only pass, config-only
catching the mine-on/gate-off typo, and a positive control — a fully inspected
healthy deployment still passes, so the gate is not unconditionally red.

The re-analysis reproduction now:

```
{"mode":"deployed","base":"/definitely/not/a/cnet/base.cnb","base_loaded":0,...,
 "residual_teacher":"unreachable",
 "dangerous":["base_not_loaded","residual_http_unreachable"],"status":"danger"}
OWN_LEARNING_HEALTH_FAIL count=2                    # exit 1
```

### Gate status on this host — BLOCKED, reported not hidden

```
make own_learning_health                            # exit 4
OWN_LEARNING_HEALTH_STRICT_PASS checks=33
CONFIG_ONLY_PASS knobs=checked deployment=not_inspected
OWN_LEARNING_HEALTH_BLOCKED reason=no_deployed_base path=/home/marble/AI/CNET/logs/personal.cnb
Strict deployed health cannot be assessed here. This is not a PASS.
```

The configured deployed base does not exist in this environment. The recipe
therefore reports **BLOCKED with exit 4** — a deployment that is not present
cannot be certified healthy, and the previous behaviour (PASS) was the defect.
An operator on the real deployment host, or anyone setting `CNET_BASE_PATH`,
gets a genuine strict verdict. This gate is recorded as BLOCKED in the final
matrix, not as passing.

### Regression commands — FRESH

`own_learning_health` is not a prerequisite of `verify` or `ci_core`, so the
new non-zero exit does not cascade. Confirmed by re-running `make recipe_gate`
(exit 0), which enumerates it as a headline gate.

---

## B4 — Coverage sidecar load must be transactional and sealed

### Defect

`hybrid_coverage_load` broke out of its parse loop on any malformed or
truncated record and then returned **0 — success** — with whatever it had
already installed. It also:

* ignored `hybrid_coverage_record` failures;
* never required `in_dim == field_width * field_count`;
* accepted any integer as a port family, including families whose exact
  membership cannot be decided;
* accepted non-finite row values (`scanf` parses `nan` and `inf` happily);
* let a later record silently displace an earlier one on the same port shape,
  freeing the incumbent's rows and leaving that unit default-allow;
* rewrote an ownerless record (`~`) into the invented unit name `restored`;
* ignored trailing bytes after the last record.

Startup then treated a mined unit as guarded if **any** active record merely
carried its name (`hybrid_coverage_has_unit`), and `hybrid_coverage_admits_unit`
returned *allow* when a naming record had an unsupported family or a mismatched
dimension. Together: a corrupt or stale record that kept the unit name
suppressed the fail-closed arm while binding no ports and no dimension.

### RED — FRESH

```
make coverage_sidecar_seal      # exit 2 (make), test binary exit 1
COVERAGE_SIDECAR_SEAL_FAIL checks=98 failures=67
```

67 failures across 20 corruption mutations. Representative:

```
FAIL: a record for another dimension must not admit a mined unit
FAIL: an undecidable family must not admit a mined unit
FAIL: truncation at byte 106 of 123 is rejected whole
FAIL: in_dim disagreeing with the port shape: load must report failure
FAIL: in_dim disagreeing with the port shape: mined unit refuses Tier A
FAIL: an out-of-range input family: no partial state is installed
FAIL: a NaN row value: load must report failure
FAIL: two units claiming one port shape: load must report failure
FAIL: trailing garbage: load must report failure
FAIL: a record naming no unit: load must report failure
```

Each mutation asserts four things: the load reports failure, a fresh store ends
with zero records, the mined unit refuses Tier A, and — the partial-state case
the old loader got wrong — loading the corrupt file into an **already
populated** store leaves the prior records intact.

### Fix

`src/hybrid_ai.c`:

* The loader now **stages** every record (parse + validate + allocate) and only
  commits once the entire file is known good. The commit phase is
  allocation-free by construction, so it cannot fail half way. Any rejection
  returns `-4` and leaves the store byte-identical.
* Validation added: magic/version, row-count and dimension ranges with overflow
  checks, `field_width * field_count == in_dim`, family within the enum,
  input family must be one whose exact membership can be decided, non-zero and
  bounded port dimensions, an owning unit name (`~` is refused, not renamed),
  `isfinite` on every value, exact row markers, no trailing bytes, no duplicate
  or conflicting port shape within the file, no shape already owned by a
  different unit, and store capacity.
* `hybrid_coverage_admits_unit` no longer returns *allow* for a naming record on
  an undecidable family or a mismatched dimension. It keeps looking, and if a
  record named the unit but none admitted the input, it refuses.
* New `hybrid_coverage_binds_unit(h, unit, in_port, out_port, in_dim)` — the
  exact relation, as opposed to "is this name mentioned".

`src/personal_ai.c`: the startup self-check materializes each mined unit and
requires a record binding that unit to **its** ports at **its** input
dimension. A rejected sidecar also arms fail-closed on its own.

### GREEN — FRESH

```
make coverage_sidecar_seal
COVERAGE_SIDECAR_SEAL_PASS checks=98 mutations=20 partial_state=none   # exit 0
```

The control assertions matter as much as the negatives: a well-formed sidecar
still round-trips, a certified row is still admitted, an uncertified row is
still refused, and dropping only trailing whitespace still loads — so the gate
cannot pass by being vacuously strict.

### Regression commands — FRESH

```
make coverage_abstain               COVERAGE_ABSTAIN_PASS checks=55 heldout_correct=4/4 was=0/4   exit 0
make knowledge_capsule              KNOWLEDGE_CAPSULE_PASS checks=88 coverage_rows=5             exit 0
make knowledge_accumulation_bench   PASS units=32 ... ood_refused=96                             exit 0
make knowledge_composition_bench    PASS members=3 hops_guarded=every refusals=root+intermediate exit 0
```

### Sanitizers — FRESH

```
make coverage_sidecar_seal_san      # ASan + UBSan, detect_leaks=1, halt_on_error=1
COVERAGE_SIDECAR_SEAL_PASS checks=98 mutations=20 partial_state=none   # exit 0
```

A transactional loader that leaked staged rows or read freed memory on the
reject path would not really have rolled anything back, so the rollback paths
are exercised under sanitizers rather than trusted.

---

## B5 — Capsule certification scope, lineage, and least disclosure (with C8)

### Defect

Three invariants the capsule claimed but did not enforce, plus one parity bug:

1. **Scope.** Coverage was optional and `coverage 0 0 0` was written whenever a
   caller passed no `HybridAi`. The header said `cov == NULL` was correct only
   for a whole-domain unit; nothing proved whole-domain. A unit certified on a
   *sample* could be exported, imported, re-certified against that same sample,
   and then answer anywhere.
2. **Lineage.** Import verified the payload's provenance against the manifest
   and then dropped it — `cnb_add_unit` zero-initialises the new unit ref —
   while the success report went on repeating the manifest's provenance.
3. **Least disclosure.** `cnb_export_subset` copied **every** oracle descriptor
   before filtering units, so a one-unit capsule shipped the whole source
   registry's names, kinds, ports and identities.
4. **Parity (C8).** Export accepted `asset != NULL` with `asset_len == 0`;
   import rejects zero-byte assets. Export could report success for an artifact
   its own importer refuses.

### RED — FRESH

The same test file compiles against a build that predates the `scope` report
field (it probes `CNET_CAPSULE_REPORT_HAS_SCOPE`), so it can be run directly
against pristine `HEAD` sources extracted read-only into a scratch tree:

```
cd <scratch>/red-head
gcc ... src/cnet_capsule.c src/base.c ... tests/test_capsule_scope_lineage.c -o bin/red_capsule_scope
./bin/red_capsule_scope
```

Observed (exit **1**):

```
FAIL: the destination unit carries the verified provenance
FAIL: the payload does not disclose an unreferenced descriptor
FAIL: the referenced oracle descriptor travels with the unit
FAIL: a sampled unit refuses to export without coverage
FAIL: the refusal names the missing boundary
FAIL: a refused export publishes no manifest
FAIL: a zero-length asset refuses to export
FAIL: the refusal names the parity failure
CAPSULE_SCOPE_LINEAGE_FAIL checks=22 failures=8
```

### Fix

* **Scope is derived, not declared.** `cap_scope_exhaustive()` proves the sealed
  contract's exemplar inputs are exactly the input port's domain: the domain
  must be finite and enumerable (one-hot `width^count`, binary `2^(width*count)`,
  both capped at 2^20; `PORT_RAW`/`EVIDENCE`/`CONCEPT` are never enumerable), and
  every exemplar must be a legal member, all distinct, and as many as the domain
  has points. Export refuses `sampled_scope_requires_coverage`; the manifest
  carries a `scope` line inside the checksummed region.
* **Import re-derives the scope from the payload** rather than believing the
  manifest: `exhaustive_scope_claim_unproven`, `sampled_scope_without_coverage`,
  `unknown_certification_scope`, `scope_disagrees_with_payload`.
* **Lineage is restored atomically.** The referenced oracle descriptor travels
  in the payload subset; import ensures it exists in the destination, adds the
  unit, then calls `cnb_set_unit_provenance`. If the unit admit fails, the
  coverage record is forgotten *and* the appended descriptor is rolled back by
  truncating `oracle_count` (`CnbOracleDesc` owns no heap, so this is exact).
* **`cnb_export_subset` copies only descriptors a kept unit references.**
* **Export refuses a zero-length asset** (`zero_length_asset_would_not_import`).

### GREEN — FRESH

```
make capsule_scope_lineage
CAPSULE_SCOPE_LINEAGE_PASS checks=33 scope=machine_verified lineage=restored disclosure=referenced_only   # exit 0
```

33 checks including the refusal negatives *and* destination non-mutation after
each one (unit count, oracle count and coverage count all unchanged), a refused
export publishing no manifest, and positive controls so the gate is not
vacuously strict.

The tamper fixtures deliberately **recompute** `manifest_fnv` after editing.
FNV here is unkeyed and the header says so — it detects accident, not
authorship. Proving that the *semantic* checks refuse a well-formed lie is the
only thing that could have caught these defects.

### Regression commands — FRESH

```
make knowledge_capsule              exit 0
make knowledge_accumulation_bench   exit 0
make knowledge_composition_bench    exit 0
make coverage_abstain               exit 0
make vision_capsule_asset           exit 0
make port_raw_unit_seam             exit 0
```

### What this does NOT establish

`exhaustive` is only provable for finite enumerable discrete ports. For
`PORT_RAW` and any continuous domain the answer is always "sampled", so those
units simply cannot travel without coverage — which is the correct fail-closed
outcome, not a solution to continuous certification. The re-analysis verdict
that portable continuous specialists remain **BLOCKED** is unchanged.

---

## C6 — Schema-2 vision asset parser hardening

### Defect

`tools/vision_detection/vd_runner.cpp` validated the magic, the schema, and one
size expression, then used every other asset-controlled field directly:

* `pca_dim` drove a loop writing into a fixed `double x[512]` with **no bound**;
* `hog_dim * pca_dim` was multiplied with no overflow check and cast to OpenCV
  `int`;
* `extractor` and the other fixed char arrays were printed with `%s` with no
  proof of NUL termination;
* nothing required the frontend's projection width to match the head that
  arrived with it, so a checksum-valid capsule could compute a confident
  function of the wrong input.

Capsule FNV is unkeyed and detects accident, not authorship, so none of this was
caught by the checksum.

### RED — FRESH

The pre-fix parser is transcribed into the test as an executable control, so the
reproduction stays runnable rather than living in a scratch directory:

```
CNET_VD_FRONTEND_LEGACY=1 ./bin/vd_frontend_parse      # exit 1
```

Observed (24 mutations accepted by the pre-fix parser):

```
VD_FRONTEND_LEGACY_MODE parser=pre_fix_vd_runner_3edac49
FAIL: an unterminated class0 is refused
FAIL: an unterminated extractor is refused
FAIL: an unterminated protocol is refused
FAIL: a pca_dim past the fixed projection buffer is refused
FAIL: a pca_dim larger than hog_dim is refused
FAIL: an enormous hog_side is refused
FAIL: an enormous ss_width is refused
FAIL: an enormous proposal budget is refused
FAIL: an out-of-range colour flag is refused
FAIL: a NaN gate tau is refused
...
VD_FRONTEND_PARSE_FAIL checks=399 failures=24
```

`a pca_dim past the fixed projection buffer` is the stack overflow: an asset
declaring `pca_dim = 4096` writes 3584 doubles past `double x[512]`.

### Fix

* `tools/vision_detection/vd_frontend.c` — the validator, extracted into its own
  plain-C translation unit with **no OpenCV**. That is deliberate: a parser only
  reachable through a runner that needs OpenCV, VOC images and an imported
  capsule is a parser that never gets fuzzed.
* `vd_frontend_validate()` proves, before any field is used: magic (all 8 bytes,
  it is not a C string), schema, NUL termination of every fixed char array,
  every dimension in range, `pca_dim <= VD_FRONTEND_MAX_PCA_DIM` (the size of
  the projection buffer, named so the coupling is explicit), `pca_dim <=
  hog_dim`, finite non-negative `gate_tau`, and checked arithmetic for
  `hog_dim + pca_dim*hog_dim` and the total byte count, which must equal `len`
  exactly.
* `vd_frontend_check_contract()` requires `pca_dim == btn.input_count` and
  `btn.output_count == n_classes`.
* `vd_frontend_check_protocol()` requires the asset to name the protocol
  compiled into the runner — identity comes from the binary, never from the
  artifact describing itself.
* `vd_runner.cpp` now calls all three and declares
  `double x[VD_FRONTEND_MAX_PCA_DIM]`.

### GREEN — FRESH

```
make vd_frontend_parse
VD_FRONTEND_FUZZ iterations=20000 accepted=12894
VD_FRONTEND_PARSE_PASS checks=25883 mutations=31 fuzz=20000     # exit 0

make vd_frontend_parse_san      # ASan + UBSan, detect_leaks=1, halt_on_error=1
VD_FRONTEND_PARSE_PASS checks=25883 mutations=31 fuzz=20000     # exit 0

make bin/vd_runner              # exit 0, warning-free with the validator wired in
```

31 structured mutations (one per field) plus a deterministic splitmix64 byte
fuzz over the header. Every fuzz-accepted mutant is additionally asserted to be
self-consistent — `pca_dim` within the projection buffer and the declared size
equal to the buffer length — so "accepted" can never mean "unchecked".

### BLOCKED in this worktree

An end-to-end `bin/vd_runner` run against a real capsule could not be executed:
there is no `data/` tree here, and the vision capsule itself remains BLOCKED
upstream because CNU1 rejects continuous exemplars. The parser is proven by
mutation and fuzz; the full runtime path is unchanged and unproven here.

---

## C7 — CNU parser topology budgets

### Defect

`unit_load_mem` bounded each dimension individually against `UNIT_MAX_DIM`
(2^20) but never bounded their **products**, and called `btn_init` before
anything proved the sealed payload carried the arrays those products imply.
`btn_init` `calloc`s `input x max_hidden` and `max_hidden x output`, then runs
`btn_add_hidden_neuron` `hidden_count` times — and each pass *writes*
`input_count` doubles. So a correctly resealed 764-byte CNU could make the
parser touch a gigabyte of real memory and burn seconds of CPU before the
bounded reads discovered there were no weights to read. Capsule payload size
caps do not cap expansion implied by header dimensions.

### RED — FRESH

The same test compiled against pristine `HEAD` sources in a read-only scratch
extraction. Fixtures are built from a real saved unit, have their four
dimensions **and their port widths** rewritten (the parser cross-checks the two,
so a fixture that only changed the dimensions would be refused for the wrong
reason), and are re-sealed with the format's own FNV-1a.

```
cd <scratch>/red-head && ./bin/red_cnu_budget      # exit 1
```

Observed:

```
CNU_BUDGET_CONTROL bytes=764
CNU_BUDGET_BASELINE peak_kb=568 cpu=0.000
CNU_BUDGET_CASE verdict=1 peak_kb=824 cpu=0.000 a tiny CNU whose max_hidden_count alone is amplified
FAIL: a tiny CNU whose max_hidden_count alone is amplified is refused (verdict=1)
CNU_BUDGET_CASE verdict=0 peak_kb=1060408 cpu=1.406 a tiny CNU that makes the parser touch a gigabyte
FAIL: a tiny CNU that makes the parser touch a gigabyte is refused BEFORE allocating (peak 1060408 kB vs control 568 kB)
CNU_BUDGET_FAIL checks=22 failures=2
```

**1,060,408 kB and 1.406 s of CPU from a 764-byte file**, and one header
accepted outright. Measuring cost rather than only the verdict is the point: a
parser that commits a gigabyte and *then* refuses has still refused, and that is
exactly the denial of service.

### Fix

`src/contract/unit.c`, before `btn_init`, with every product checked against
`SIZE_MAX` first:

* **Read budget** — the doubles the parser is about to read
  (`output_bias + hidden_bias + input x hidden + hidden x output`) must fit in
  the bytes that actually remain in the sealed payload. This pins `hidden_count`
  to what the file can possibly contain.
* **Allocation budget** — what `max_hidden_count` implies must be neither absurd
  in absolute terms (`UNIT_MAX_CELLS`, 64M doubles ≈ 512 MB) nor more than
  `UNIT_ALLOC_SLACK` (64x) the size of the artifact describing it. The relative
  bound is what "header-amplified" actually means: a real unit's payload already
  contains its matrices, so the honest ratio is near 1.

### GREEN — FRESH

```
make cnu_budget
CNU_BUDGET_CONTROL bytes=764
CNU_BUDGET_BASELINE peak_kb=556 cpu=0.000
CNU_BUDGET_CASE verdict=0 peak_kb=556 cpu=0.000 a tiny CNU declaring 2^20 in every dimension
CNU_BUDGET_CASE verdict=0 peak_kb=556 cpu=0.000 a tiny CNU whose max_hidden_count alone is amplified
CNU_BUDGET_CASE verdict=0 peak_kb=556 cpu=0.000 a tiny CNU with an amplified input x hidden matrix
CNU_BUDGET_CASE verdict=0 peak_kb=556 cpu=0.000 a tiny CNU with an amplified hidden x output matrix
CNU_BUDGET_CASE verdict=0 peak_kb=556 cpu=0.000 a tiny CNU whose products would wrap unchecked arithmetic
CNU_BUDGET_CASE verdict=0 peak_kb=556 cpu=0.000 a tiny CNU just past the absolute cell ceiling
CNU_BUDGET_CASE verdict=0 peak_kb=556 cpu=0.000 a tiny CNU that makes the parser touch a gigabyte
CNU_BUDGET_PASS checks=22 amplified=7 address_limit=2048MB      # exit 0
```

Every amplified header is now refused at **baseline cost** — 556 kB and 0.000 s,
down from 1,060,408 kB and 1.406 s. The control assertion (an honest unit still
loads inside the same limits) is what stops the budget from passing by rejecting
everything.

### Regression commands — FRESH

```
make contract_unit                  exit 0
make base                           exit 0
make knowledge_capsule              exit 0
make coverage_abstain               exit 0
make port_raw_unit_seam             exit 0
make vision_capsule_asset           exit 0
make knowledge_accumulation_bench   exit 0
make knowledge_composition_bench    exit 0
make capsule_scope_lineage          exit 0
```

Every gate that loads real sealed units still loads them, so the budgets do not
reject honest topologies.

### Pre-existing failure, NOT caused by this slice — carried forward as FAILED

```
make certify                        exit 2
  combine      vs combine_contract      : DENIED (240/256)
  CERTIFY FAIL.
```

Verified by running `make certify` in the read-only pristine `HEAD` extraction:
**it fails identically there** (`exit 2`, `DENIED (240/256)`). This is a
demo-training convergence failure in `certify_demo`, unrelated to the parser
budgets, and `certify` is a prerequisite of `make verify` — so `verify` was
already red at the base commit. Recorded as a pre-existing FAILED gate rather
than fixed or hidden inside this patch.

### Sanitizers — FRESH

```
make cnu_budget_san      # ASan + UBSan, halt_on_error=1, CNU_BUDGET_NO_RLIMIT=1
CNU_BUDGET_PASS checks=22 amplified=7 address_limit=2048MB      # exit 0
```

`RLIMIT_AS` is disabled under the sanitizer because ASan reserves an enormous
shadow mapping that the limit would refuse; the RSS and CPU assertions still
apply, and they are the ones that detect the defect.

---

## D10 — Evidence discipline within this patch

### Defect

Headline gate logs live at stable paths under an ignored `logs/` tree and carry
no run identity, so "the marker is present" proves only that *some* process once
wrote it. The re-analysis observed fresh-looking `2026-07-30` logs that no
reviewer had created and correctly refused to count them as evidence. Marker
output cannot distinguish fresh from carried.

The broad remedy (relocating runtime state, content-addressed run directories) is
explicitly **out of scope for this patch**. What is in scope: the gates this
patch adds or changes must emit run-bound evidence and must not treat a stale
mutable log as authority.

### Fix

`scripts/gate_evidence.sh`, used by every gate added here
(`heldout_fixture_test`, `capability_fixture_causality`,
`coverage_sidecar_seal`, `capsule_scope_lineage`, `vd_frontend_parse`,
`cnu_budget`):

1. **deletes any pre-existing log first**, so a stale file can never be read as
   this run's evidence;
2. runs the producer with direct redirection, so its exit status is the status —
   no pipe to lose it;
3. writes `<log>.evidence.json` binding gate name, run UUID, start time, commit,
   working-tree digest, assume-unchanged count, exact argv, producer exit status,
   the SHA-256 of the log it just wrote, and whether the marker was found. The
   binding is a sidecar, not appended to the log, so the log's digest is the
   digest of exactly what the producer wrote;
4. fails on a non-zero producer status **or** a missing marker — status first,
   marker as corroboration, never the other way round.

`tests/run_capability_cert.py` does the same for the certificate itself, and
additionally binds the evaluator's declared source-set and binary digests.

### GREEN — FRESH

```
bash tests/test_gate_evidence.sh
GATE_EVIDENCE_PASS checks=13 stale=refused marker_then_exit=refused silent=refused  # exit 0
```

13 checks: an honest producer passes and writes a complete binding; two runs of
the same gate get different run ids; a **stale passing log left at the same path
cannot be inherited** by a producer that then fails; a producer that prints the
marker and exits 7 fails with 7; a producer that exits 0 without printing the
marker fails too.

Live example from `make heldout_fixture_test`:

```
GATE_RUN gate=heldout_fixture_test run_id=725ca340-76e4-4775-87e7-6f0f470a24de started=2026-07-30T10:13:38Z commit=68dfdfafbe1b1473d3cc192dec3b691fe63cdbf5 worktree=ed0c21fd9fdb0849 dirty=2 assume_unchanged=83
CNET_HELDOUT_TEST_PASS checks=42
GATE_PASS gate=heldout_fixture_test run_id=725ca340-... commit=68dfdfa... evidence_sha256=539ad0f0...
```

`recipe_gate` runs this test, so the discipline is enforced on every invocation.

### What this does NOT establish

This binds the gates **this patch touches**. The other ~40 recipes still write
unbound logs to stable ignored paths, and the P0 item "separate evidence from
runtime state" (content-addressed run directories, a quiesced learner) is
**not done** — it was explicitly out of scope here. Treat any log outside the
list above exactly as the re-analysis did: as untrusted until re-executed.

---

## Verification contract — outcome

Every gate in the contract was executed fresh against the final tree. Nine of
eleven pass; the two that do not are reported as they are.

| Command | Exit | Result |
|---|---|---|
| `make recipe_gate` | 0 | PASS |
| `make capability_cert` | 0 | PASS 6/6, run-bound |
| `make own_learning_health` | 2 (recipe 4) | **BLOCKED** — no deployed base on this host |
| `make coverage_abstain` | 0 | PASS checks=55 |
| `make knowledge_capsule` | 0 | PASS checks=88 |
| `make knowledge_accumulation_bench` | 0 | PASS units=32 |
| `make knowledge_composition_bench` | 0 | PASS members=3 |
| `make port_raw_unit_seam` | 0 | PASS checks=8 |
| `make vision_coverage_test` | 0 | PASS checks=22 |
| `make vision_capsule_asset` | 0 | PASS checks=39 |
| `make ci_core` | 2 then **0** | see below |

### `ci_core` — failed once on ambient state, then passed

First invocation failed at `managed_warning_gate`:

```
error NETSDK1004: Assets file '.../dotnet/Cce.Tests/obj/project.assets.json' not found.
make: *** [Makefile:3622: managed_warning_gate] Error 1
```

`dotnet/Cce.Tests` had never been NuGet-restored in this worktree. That project
is not touched by this patch. After `make dotnet_restore` (exit 0,
`DOTNET_RESTORE_PASS`), `make ci_core` exits **0** with `CNET_CI_CORE_PASS`.

This is itself a small gate-integrity defect of the same family as the ones
fixed here — `managed_warning_gate` depends on ambient restore state it does not
declare as a prerequisite — but it is outside the requested slices and adding
`dotnet_restore` as a dependency would give `ci_core` a network side effect. It
is **reported, not silently fixed**.

---

# Phase 2 — Codex review findings

Second strict RED→GREEN pass against the Codex final-review verdict
(REQUEST CHANGES). Same rules: no floor lowered, no negative weakened, no
FAILED/WITHHELD/BLOCKED renamed, no path from the CRLF baseline touched.

A method note that applies to R1, R2 and everything else in this phase that
concerns resource amplification: **a verdict alone cannot detect it.** A parser
that reserves a gigabyte and only then discovers the file is two lines long has
still "refused", and `calloc`/`malloc` of untouched pages leaves RSS flat. So
the harnesses fork a child under `RLIMIT_AS` and `RLIMIT_CPU` and read the
child's **`VmPeak`** from `/proc/self/status` just before it exits. Peak address
space is what the defect actually moves.

## R1 — CNU exemplar-count amplification

### Defect

`exemplar_count` was bounded only by `UNIT_MAX_EXEMPLARS` (2^24), and its
products with the port totals were checked only for overflow — never against
the bytes remaining in the sealed payload. Exemplars are packed one **bit** per
value with each row byte-aligned, so a declared row count implies an exact
payload length; nothing compared the two before two `malloc`s.

### RED — FRESH

```
cd <scratch>/red-head && ./bin/red_cnu_budget        # exit 1
```

```
CNU_BUDGET_BASELINE peak_kb=1092 vm_peak_kb=3836 cpu=0.000
CNU_BUDGET_CASE verdict=0 peak_kb=1348 vm_peak_kb=1052420 cpu=0.000 a tiny CNU declaring 2^24 exemplars
FAIL: a tiny CNU declaring 2^24 exemplars reserves no extra address space (VmPeak 1052420 kB vs control 3836 kB)
CNU_BUDGET_CASE verdict=0 peak_kb=1348 vm_peak_kb=1052412 cpu=0.000 a tiny CNU one under the exemplar ceiling
CNU_BUDGET_CASE verdict=0 peak_kb=1348 vm_peak_kb=69380 cpu=0.000 a tiny CNU declaring 2^20 exemplars
CNU_BUDGET_FAIL checks=54 failures=8
```

**1,052,420 kB of address space reserved by a 764-byte file.**

### Fix

`src/contract/unit.c`, before both `malloc`s, all products checked against
`SIZE_MAX` first:

* **read budget** — `exemplar_count * ceil((in_total + out_total) / 8)` packed
  bytes must fit in the payload that remains;
* **allocation budget** — the two exemplar arrays must be within
  `UNIT_MAX_CELLS` absolutely and within `UNIT_ALLOC_SLACK` (64x) of the
  artifact size.

### GREEN — FRESH

```
make cnu_budget
CNU_BUDGET_BASELINE peak_kb=1076 vm_peak_kb=3836 cpu=0.000
CNU_BUDGET_PASS checks=54 amplified=7 exemplar_amplified=6 address_limit=2048MB   # exit 0
make cnu_budget_san                                                               # exit 0
```

Every exemplar case now returns to the control's `vm_peak_kb=3836`. The six
pre-existing topology cases and the honest-unit control are unchanged.

## R2 — Coverage sidecar cell/byte amplification

### Defect

`n_rows` and `in_dim` were each bounded at 2^20 and their product checked only
for overflow, so a two-line sidecar could declare 2^40 doubles (8 TB) and reach
`calloc` before anything compared the declaration to the file's own length.

### RED — FRESH

```
make coverage_sidecar_seal        # exit 2
```

```
COVERAGE_AMPLIFIED_BASELINE vm_peak_kb=24288 cpu=0.000
COVERAGE_AMPLIFIED verdict=0 vm_peak_kb=57060 cpu=0.000 2^20 tiny rows declared by a two-line file
COVERAGE_AMPLIFIED verdict=0 vm_peak_kb=1072868 cpu=0.000 a two-line file declaring a gigabyte of coverage rows
FAIL: a two-line file declaring a gigabyte of coverage rows reserves no extra address space (VmPeak 1072868 kB vs control 24288 kB)
COVERAGE_SIDECAR_SEAL_FAIL checks=115 failures=2
```

### Fix

`src/hybrid_ai.c` measures the file's length at open and, before each record's
`calloc`:

* a row costs at least `2 + 2*in_dim` bytes (`R`, then per value a separator and
  one digit, then a newline), so `n_rows * (2 + 2*in_dim)` must fit in the bytes
  that remain — a safe lower bound that cannot reject a real encoding;
* `n_rows * in_dim` must be within `COVERAGE_MAX_CELLS` (2^24 doubles) and
  within `COVERAGE_ALLOC_SLACK` (16x) of the file size.

### GREEN — FRESH

```
make coverage_sidecar_seal
COVERAGE_SIDECAR_SEAL_PASS checks=115 mutations=20 amplified=5 partial_state=none  # exit 0
make coverage_sidecar_seal_san                                                     # exit 0
```

All five amplified fixtures return to the control's `vm_peak_kb=24288`.

## R7 — Vision asset body finiteness

### Defect

`vd_frontend_validate` checked every header field but never looked at the body.
The PCA mean and eigenbasis go straight into `cv::PCA::project` and then into
the certified head, so one NaN or Inf makes every projected feature NaN, every
score NaN, and the coverage gate's comparisons meaningless — a silently degraded
guard rather than a refusal.

### RED — FRESH

```
CNET_VD_FRONTEND_LEGACY=1 ./bin/vd_frontend_parse    # exit 1
FAIL: a NaN as the first PCA mean value is refused
FAIL: a +Inf as the last PCA mean value is refused
FAIL: a -Inf in the PCA mean is refused
FAIL: a NaN as the first eigenbasis value is refused
FAIL: a +Inf as the last eigenbasis value is refused
FAIL: a -Inf in the eigenbasis is refused
FAIL: a signalling NaN in the eigenbasis is refused
VD_FRONTEND_PARSE_FAIL checks=406 failures=31
```

### Fix

Every body float is `memcpy`'d out (nothing guarantees body alignment) and
required to be `isfinite`, with the refusal naming whether it was in the mean
(`pca_mean_not_finite`) or the matrix (`pca_matrix_not_finite`).

### GREEN — FRESH

```
make vd_frontend_parse
VD_FRONTEND_PARSE_PASS checks=25897 mutations=31 body_mutations=7 fuzz=20000   # exit 0
make vd_frontend_parse_san                                                     # exit 0
```

## R8 — Verdict and receipt cardinality

### Defect

`grep -q PREFIX_PASS` is a substring test over a whole file. A log stating both
PASS and WITHHELD answered PASS; a log stating PASS twice looked like a log
stating it once; `PREFIX_PASSED`, `NOT_PREFIX_PASS` and a marker mentioned
mid-sentence all answered PASS. On the capability side, `re.search` took the
FIRST `HELDOUT_FIXTURE` / `HELDOUT_METRIC` receipt and per-case lines collapsed
into a dict, so duplicates and undeclared cases were invisible, and a
`metric_regex` matching twice silently used the first number.

### RED — FRESH

Both suites can be pointed at the pre-fix implementation, so the reproduction
stays runnable:

```
CNET_VERDICT_SCRIPT=<pre-fix> sh tests/test_benchmark_verdict.sh    # exit 1
FAIL: a log claiming both PASS and WITHHELD is ambiguous, not PASS -- expected exit 5, got 0
FAIL: a duplicated PASS is ambiguous -- expected exit 5, got 0
FAIL: PASSED is not PASS -- expected exit 1, got 0
FAIL: a marker with a prefix in front of it is not the marker -- expected exit 1, got 0
FAIL: a marker mentioned mid-line is not a verdict -- expected exit 1, got 0
BENCHMARK_VERDICT_FAIL checks=21 failures=9

CNET_CERT_RUNNER=<pre-fix> python3 tests/test_capability_cert_runner.py   # exit 1
FAIL: test_two_fixture_receipts_are_refused
FAIL: test_two_metric_receipts_are_refused
FAIL: test_duplicate_case_line_is_refused
FAIL: test_conflicting_duplicate_case_lines_are_refused
FAIL: test_undeclared_case_line_is_refused
FAIL: test_metric_regex_matching_twice_is_refused
FAIL: test_metric_regex_matching_never_is_refused
```

### Fix

* `scripts/benchmark_verdict.sh` counts **lines** matching
  `^PREFIX_<VERDICT>([[:space:]]|$)`. Exactly one terminal verdict line is
  required; more than one is `AMBIGUOUS` with its own exit code **5**.
* `run_capability_cert.py` requires exactly one fixture receipt, exactly one
  metric receipt, exactly one line per declared case, no line for an undeclared
  case, and a `metric_regex` that matches exactly once.

### GREEN — FRESH

```
sh tests/test_benchmark_verdict.sh
BENCHMARK_VERDICT_PASS checks=30 pass=0 withheld=3 blocked=4 no_verdict=1   # exit 0
python3 tests/test_capability_cert_runner.py
CAPABILITY_CERT_RUNNER_PASS shell=disabled receipt=required checks=39       # exit 0
```

## R3 — Capability report: no secrets, and a two-sided binding

### Defect

`environment_binding` serialized the raw value of every `CNET_*`/`CCE_*`
variable into `logs/capability_cert.json` — a file people paste. And every
binding was one-sided: the source-set digest was taken before the run, the
binary digest after, the tree digest once at the start. Nothing could notice a
source, fixture, binary, dirty tracked file or untracked file that moved *after*
a valid receipt was accepted, which is exactly when it matters.

### RED — FRESH

```
CNET_CERT_RUNNER=<pre-fix> python3 tests/test_capability_cert_runner.py   # exit 1
FAIL: test_secret_env_values_never_reach_the_report
ERROR: test_binding_is_stable_when_nothing_moves
ERROR: test_source_mutation_after_the_receipt_is_detected
ERROR: test_fixture_mutation_after_the_receipt_is_detected
ERROR: test_binary_mutation_after_the_receipt_is_detected
ERROR: test_tracked_dirty_content_mutation_is_detected
ERROR: test_committed_file_content_mutation_is_detected
ERROR: test_untracked_file_appearing_is_detected
Ran 39 tests -- FAILED (failures=8, errors=9)
```

The `ERROR`s are the honest shape of this RED: the pre-fix runner has no
`capture_state` or `compare_states` at all, so there is nothing to call.

### Fix

* Values are recorded only for an explicit `ENV_VALUE_ALLOWLIST` of documented
  non-secret knobs. Every other `CNET_*`/`CCE_*` knob contributes its **name**
  and a per-value SHA-256, so a knob that changed is still visible without
  disclosing what it is. The whole-environment digest is unchanged.
* `capture_state()` takes commit, a **content** digest of every changed tracked
  path, a content digest of every untracked non-ignored path, the declared
  source set, the fixture, and (optionally) the binary. It is called **three**
  times per capability: before the run, immediately after the evaluator exits,
  and once every other digest has been taken. `compare_states()` names anything
  that moved; any drift fails the capability. The binary is excluded from the
  first comparison only, because the recipe legitimately builds it.

### GREEN — FRESH

```
python3 tests/test_capability_cert_runner.py     # 39 tests OK, exit 0
make capability_cert                             # exit 0
CAPABILITY_CERT_PASS certified=6/6 run_id=0dfc1e70-... commit=bb85301...
```

with `binding_stable = True`, `binding_drift = []`, and no raw knob value in
`logs/capability_cert.json`.

## R4 — Gate evidence integrity

### Defect

`scripts/gate_evidence.sh` was POSIX `sh`, and four defects followed from that:
it hashed `git status` **text** (so a tracked file whose bytes changed while its
status line stayed `" M path"` produced an identical binding, and untracked
content was never hashed); it had no post-state; it recorded the command as
`"$*"` (losing argv boundaries and breaking on a quote, backslash or newline);
and it emitted JSON with `printf` and matched the marker as a **substring**.

### RED — FRESH

```
CNET_GATE_EVIDENCE=<pre-fix> bash tests/test_gate_evidence.sh      # exit 1
FAIL: an honest run reports a stable binding
FAIL: one argument 'a b' binds differently from two arguments 'a' 'b'
FAIL: the binding records argv as a list, not a joined string
FAIL: an argument with a quote, backslash and newline keeps the JSON valid
FAIL: such an argument round-trips exactly
FAIL: the knob NAME is still recorded
FAIL: a marker printed twice is refused (exit 0)
FAIL: a log asserting both PASS and FAIL is refused (exit 0)
FAIL: PASSED, a prefixed marker and a mid-line mention are not the marker (exit 0)
GATE_EVIDENCE_FAIL checks=32 failures=13
```

### Fix

`scripts/gate_evidence.py` replaces it (the `.sh` is removed; the Makefile's six
call sites now use `python3`). Content-addressed tracked **and** untracked
digests, captured before and after and compared; argv serialized as a real JSON
array via `json.dumps`; the marker matched anchored at line start and required
to appear exactly once, with any other terminal verdict for the same prefix
(`_FAIL`, `_WITHHELD`, `_BLOCKED`, `_AMBIGUOUS`, `_NO_VERDICT`) refused as
conflicting; environment recorded as a digest plus knob **names** only.

### GREEN — FRESH

```
bash tests/test_gate_evidence.sh
GATE_EVIDENCE_PASS checks=29 stale=refused marker_then_exit=refused silent=refused
  duplicate=refused conflicting=refused substring=refused argv=exact secrets=redacted  # exit 0
```

One control matters as much as the negatives: **ignored build output changing
during a run must not fail the gate.** `logs/` and `bin/` move on every
invocation; a binding that failed on those would be unusable, so the digests
deliberately cover only tracked and untracked non-ignored paths.

## R5 — Capsule import atomic rollback

### Defect

`include/cnet_capsule.h` promises the destination is not mutated when an import
is refused, but `cnb_add_unit` has no removal counterpart, so any failure after
admission left the unit in the base. The only rollback was an ad-hoc
`dst->oracle_count = oracles_before`, which covers the descriptor and nothing
else — not the blob, the unit ref, the minted tags, or the mint sequence.

The window between admission and provenance restore is unreachable by
construction (the descriptor is ensured beforehand and the unit is added with
empty provenance). **Unreachable is not the same as recoverable**, so
`CNET_CAPSULE_FAIL_AFTER_ADMIT=1` makes the recovery path executable.

### RED — FRESH

Same test, same fault injected, with the `cnb_rollback` calls removed from
`src/cnet_capsule.c` — i.e. exactly the pre-slice behaviour:

```
<scratch>/r5-red/red_capsule_rollback        # exit 1
FAIL: the admitted unit is rolled back out of the base
FAIL: the destination is BYTE-identical across the refused import
FAIL: the rolled-back destination still accepts an honest import
CAPSULE_SCOPE_LINEAGE_FAIL checks=45 failures=3
```

The third failure is the compounding one: the half-imported unit makes the
destination refuse the *honest* retry as a duplicate.

### Fix

`cnb_mark()` / `cnb_rollback()` in `src/base.c`. Every base mutation is an
append, so recording the array lengths and the mint sequence is an exact
inverse; rollback frees the blob payloads past the mark (the only owned heap)
and truncates. Rolling back to a mark from a different base, or "forward", is
refused rather than guessed at. `cnet_capsule_import_asset` takes the mark
before its first destination mutation and rolls back on every later failure.

### GREEN — FRESH

```
make capsule_scope_lineage
CAPSULE_SCOPE_LINEAGE_PASS checks=45 scope=machine_verified lineage=restored
  disclosure=referenced_only rollback=byte_identical                     # exit 0
```

The assertion is byte identity: the destination is saved with `cnb_save` before
and after the refused import and the two files are compared by FNV, on a base
that already holds a unit, a coverage record and a descriptor — not an empty
one. It then still accepts an honest import, so the rollback left nothing subtly
broken.

## R6 — Health and serving must agree

### Defect

`tools/cnet_own_learning_health.c` asked `hybrid_coverage_has_unit` — "is this
name mentioned anywhere" — while `personal_ai`'s startup asks
`hybrid_coverage_binds_unit`, the exact unit + ports + dimension. A sidecar
record carrying the right unit name but a different port tag, goal tag, input
dimension or port family loads perfectly well, so the deployment gate reported
**healthy** for exactly the unit serving had armed fail-closed against.

### RED — FRESH

The pre-fix tool, built from `HEAD` against the current libraries and driven by
the same fixtures:

```
bash tests/test_own_learning_health.sh <pre-fix-binary> ./bin/mk_test_base   # exit 1
FAIL: a record with a different input port tag must not count as guarded -- expected exit 1, got 0
FAIL: ... -- forbidden marker OWN_LEARNING_HEALTH_PASS was printed
FAIL: a record with a different goal port tag must not count as guarded -- expected exit 1, got 0
FAIL: a record with a different input dimension must not count as guarded -- expected exit 1, got 0
FAIL: a record with a different port family must not count as guarded -- expected exit 1, got 0
OWN_LEARNING_HEALTH_STRICT_FAIL checks=45 failures=13
```

All four mismatched-interface records produced `OWN_LEARNING_HEALTH_PASS`.

### Fix

The health tool materializes each mined unit with `cnb_get_unit` and calls the
**same predicate serving calls**. A unit it cannot materialize is its own danger
(`mined_units_unreadable`), and the existing danger is renamed to
`mined_units_without_bound_coverage` because "without coverage" was never what
was being measured. `tests/mk_test_base.c` grows
`--cov-in-tag/--cov-out-tag/--cov-in-width/--cov-in-family` so a fixture can be
a record that **loads** and does not **bind**.

### GREEN — FRESH

```
bash tests/test_own_learning_health.sh
OWN_LEARNING_HEALTH_STRICT_PASS checks=45      # exit 0
```

45 checks, up from 33. `make own_learning_health` still ends
`OWN_LEARNING_HEALTH_BLOCKED reason=no_deployed_base` on this host — unchanged
and still not a PASS.

## R9 — Adapter baselines must be causal

### Defect

The declared `adapter_on_baseline` / `adapter_off_baseline` were only required
to satisfy `on > off` — a condition any pair of numbers in the right order
meets, so both could be edited freely and the certificate stayed green. Three
more fields (`description`, `note`, and — as it turned out — one `query`) were
read by nobody at all.

The general problem: **one mutation per capability proves the fixture is
consumed; it does not prove every declared field is causal.** A field nobody
reads sits in the fixture looking like a commitment.

### RED — FRESH

`tests/test_capability_fixture_causality.py` now enumerates a mutation for every
declared field name and every declared case, and refuses to pass if any is
uncovered. Running it first reported exactly which fields had no mutation:

```
FAIL: calibrated_abstention: every declared field has a mutation (uncovered: [(0, 'reliability'), (0, 'samples'), (1, 'expected'), (1, 'margin'), (1, 'reliability'), (1, 'samples'), (2, 'evidence_refs')])
FAIL: cce_classification: ... (uncovered: [(0, 'minimum_lift_over_majority')])
FAIL: honest_memory_retrieval: ... (uncovered: [(0, 'query')])
FAIL: hybrid_skill_serve: ... (uncovered: [(0, 'query'), (1, 'backend'), (1, 'query')])
CAPABILITY_FIXTURE_CAUSALITY_FAIL failures=4
```

Adding the mutations then exposed the fields that were genuinely decorative:

```
FAIL: hybrid_skill_serve: mutating case[0].query -> 'totally different words entirely' must fail the evaluator (rc=0)
```

### Fix

* **`json_toolcall_adapter`**: the measured arms are compared to the declared
  baselines within a declared `baseline_tolerance` (`HELDOUT_BASELINE_DRIFT` on
  failure). `skill` and `held_out_pairs_from` are compared to what the binary
  actually is and where its pairs actually come from. `description` and `note`
  are removed — prose cannot be made causal, and the manifest's
  `failure_envelope` is where that narrative belongs.
* **`hybrid_skill_serve`**: binding a query to a proposal *derived from that
  query* proves nothing, because both move together. The fixture now declares
  the `expected_proposals` the run must produce, so mutating either the query or
  the expectation breaks the match. The residual case's `query` is **removed**:
  that path publishes `residual-token:<id>` chosen by the injected top-k
  callback, so nothing observable is a function of the query text, and a
  declared field there would be a claim the fixture cannot keep.
* Coverage is checked at **field-name** level plus **per-case** level, not per
  (case, field) slot. That is deliberate and stated in the gate: an input can
  only flip the verdict in some cases — `reliability` cannot flip a case whose
  margin already forces abstention — and demanding a verdict-flipping mutation
  in every slot would mean deleting real inputs to satisfy the gate.

### GREEN — FRESH

```
python3 tests/test_capability_fixture_causality.py json_toolcall_adapter
CAPABILITY_FIXTURE_CAUSALITY_PASS capabilities=1 mutations_rejected=8 scope=selected   # exit 0
```

All eight retained `json_toolcall_adapter` fields are individually causal,
including both baselines.

---

## R10 — `make certify`: root-caused, and BLOCKED on a CRLF-baseline file

**Status: STOPPED AND REPORTED, as instructed. Not fixed, not worked around.**

### The failure, reproduced fresh on this branch

```
make certify          # exit 2
binary transform learned nibble-pair join (combine):
combine hidden neurons selected: 64
combine final loss: 0.008750
FAIL: combine did not certify against combine_contract.
  hex_value    vs hex_value_contract      : CERTIFIED (16/16)
  increment    vs increment_contract      : CERTIFIED (16/16)
  combine      vs combine_contract        : DENIED (240/256)
  split        vs split_contract          : CERTIFIED (256/256)
CERTIFY FAIL.
```

### Root cause — established, not guessed

A read-only probe replayed `btn_certify`'s exact per-exemplar logic
(`port_validate` → `port_canonicalize` → compare) against the saved weights and
contract. All 16 failures are one defect, not sixteen:

```
FAILEX s= 24 in=0x18 validate=1 canon_match=0 raw= 0.1000 0.0000 0.0000 0.0000 0.9000 0.1000 0.1000 0.1000 want= 0.0 0.0 0.0 1.0 1.0 0.0 0.0 0.0
FAILEX s= 28 in=0x1C validate=1 canon_match=0 raw= 0.1000 0.0000 0.0000 0.0000 0.8998 1.0000 0.1000 0.1000 want= 0.0 0.0 0.0 1.0 1.0 1.0 0.0 0.0
... 14 more, identical shape
failing=16
```

* Every failure passes `port_validate` — nothing is in the ambiguous band. The
  net is **confidently wrong**.
* Every failure is the **same output bit**, index 3, emitting `0.0000` where
  `1.0` is required.
* The failing inputs are exactly the 16 values with `(v & 0x1B) == 0x18` — bit 3
  and bit 4 both set, low two bits clear. `combine` is the identity per bit, so
  the net simply never learned bit 3 in that neighbourhood.

Two controlled experiments in a read-only scratch extraction:

| Change | Neurons | Final loss | Verdict |
|---|---|---|---|
| baseline (`max_hidden=64`, seed `91u`) | 64 | 0.008750 | DENIED (240/256) |
| `max_hidden` 64 → **256** | 113 | **0.008750** (bit-identical) | DENIED (240/256) |
| seed `91u` → `7u` | 64 | 0.027500 | DENIED (192/256) |
| seed `91u` → `123u` | 64 | 0.002188 | **CERTIFIED (256/256)** |
| seed `91u` → `20260730u` | 64 | 0.001875 | **CERTIFIED (256/256)** |

**The root cause is `btn_train_dynamic` converging to a seed-dependent local
minimum it cannot escape.** The capacity experiment is the decisive one: raising
the ceiling let the trainer add 49 more neurons and the loss did not move by a
single bit. Adding capacity to a plateau changes nothing, which is a trainer
defect, not a hyperparameter that wants tuning.

### Why this stops here

The fix has to make the trainer escape that plateau — for example, re-seeding a
neuron whose addition produced no loss improvement, or detecting the plateau and
restarting from a different initialization. That code is
`btn_train_dynamic` / `btn_add_hidden_neuron` in **`src/nn.c`**. The only other
place a legitimate fix could live is the demo's training setup in
**`src/legacy/main.c`**, and the certifier itself is **`tests/certify_demo.c`**.

```
PROTECTED  src/legacy/main.c
PROTECTED  src/nn.c
PROTECTED  tests/certify_demo.c
PROTECTED  include/nn.h
editable   src/contract/contract.c
editable   Makefile
```

**All four candidate files are on `/tmp/cnet-integrity-lineending-baseline.json`.**
The instruction is explicit — stop before editing a listed path and report the
exact need — so this slice stops here.

The two things I could have done instead are both forbidden and both wrong:

* changing the seed to `123u` would make `make certify` green in one line. It is
  tuning around the defect: the trainer would still have a plateau it cannot
  escape, and the next primitive to land on a bad initialization would fail the
  same way.
* relaxing `port_validate`, `port_canonicalize` or the certification comparison
  would be increasing permissiveness globally to hide a confidently wrong bit.

### Exact need, to unblock

1. Permission to edit **`src/nn.c`** (and possibly `include/nn.h` if a new
   trainer entry point is wanted), with the CRLF/index containment handled
   deliberately — a targeted edit, **not** a whole-file line-ending
   normalization, which is what the containment exists to prevent.
2. The intended change: in `btn_train_dynamic`, when adding a hidden neuron does
   not reduce loss by more than epsilon over its patience window, re-initialize
   that neuron from a fresh draw rather than keeping a dead one; and surface a
   non-zero return when training ends on a plateau above the target loss, so a
   demo cannot silently persist an uncertifiable net.
3. Acceptance, unchanged: `make certify` exits 0 with all four primitives
   certified and `combine` at 256/256, with the exemplars, the contract, the
   port semantics and the certification floors exactly as they are today.

`make certify` therefore remains **FAILED** in this phase's matrix, with the
same fresh, deterministic `DENIED (240/256)` it had at `3edac49`.

---

## R11 — Coverage identity is owner + interface, not interface alone

### Defect

`HybridCoverage` was keyed by port pair. A second specialist behind the same
typed interface therefore freed the incumbent's rows and took its slot, and
`forget_unit` could not put them back. "32 accumulated units" only worked
because `knowledge_accumulation_bench` mints a unique tag per unit — turning a
type into a namespace, which is the opposite of a reusable interface.

### RED — FRESH

```
make coverage_sidecar_seal        # exit 2
FAIL: both owners survive the round trip
FAIL: the reloaded interface still reports two owners
```

(The in-memory half failed first and was fixed with `coverage_find_owned`; the
round-trip half then exposed that `coverage_install` was still shape-keyed.)

### Fix

* `coverage_find_owned()` keys by **unit + exact interface**;
  `hybrid_coverage_record` and the transactional loader's `coverage_install`
  both use it, so a different owner appends instead of evicting.
* `hybrid_coverage_owner_count()` is new. The one lookup that *cannot* name an
  owner — `hybrid_coverage_admits`, which sees a request's ports and not which
  unit will answer — **fails closed** when more than one owner matches, because
  guessing would hand one specialist another's certified domain.
* `hybrid_coverage_forget_unit` removes **all** of a unit's records; returning
  after the first made "independently forgettable" false.
* `hybrid_coverage_rows` sums across owners.
* Sidecar **v2**: same records, plus the rule that owners may share a shape. v1
  files still load and **keep their stricter one-owner-per-shape refusal** —
  migration is explicit, not a reinterpretation of old files.
* Capsule import no longer refuses a different owner on an occupied shape; the
  displacement that refusal existed to prevent is now impossible.

### GREEN — FRESH

```
make coverage_sidecar_seal
COVERAGE_SIDECAR_SEAL_PASS checks=140 mutations=20 amplified=5 owners=multi partial_state=none  # exit 0
make knowledge_capsule
KNOWLEDGE_CAPSULE_PASS checks=94 coverage_rows=5        # exit 0 (was 88 checks)
make coverage_abstain
COVERAGE_ABSTAIN_PASS checks=55 heldout_correct=4/4 was=0/4   # exit 0
```

### Two expectations changed, and why neither is a weakened negative

1. `knowledge_capsule`'s **"conflict: import REFUSED rather than displacing"**
   now asserts the import **succeeds**. The guarantee that case has always
   protected — *the incumbent's gate is not lost* — is unchanged and still
   asserted byte-for-byte; what changed is that keeping it no longer costs a
   refusal. Four **new** assertions were added around it: two owners coexist,
   the incumbent still admits its own row, the newcomer holds the capsule's own
   rows and not the incumbent's, and the shape-only lookup fails closed while
   ambiguous. The case is strictly stronger than before.
2. `knowledge_capsule`'s **"slots: rejected imports consume no registry slots"**
   occupied the shape with a *foreign* owner to force rejection. A foreign owner
   no longer causes one, so the case would have stopped testing what it names.
   It now occupies with the **capsule's own unit**, which the duplicate-import
   preflight still rejects — same property, still exercised.
3. `coverage_abstain`'s header assertion moves from `v1` to `v2`, checked
   exactly rather than by prefix, so a silent format change is still caught.

## R12 — The per-hop guard belongs to ordinary serving

### Defect

`knowledge_composition_bench` proves the DAG API *can* enforce coverage at every
hop — because the benchmark injects a guard. `personal_ai_serve` checked the
**original request** against each mined unit by name and then called
`route_execute`, which takes no guard at all. For a chain that is the wrong
question twice over: hop 2 does not receive the request, it receives hop 1's
output, and the request's ports are not hop 2's ports.

### RED — FRESH

The same test against `HEAD`'s `src/personal_ai.c`:

```
<scratch>/red_hop_guard         # exit 1
FAIL: an uncovered INTERMEDIATE must not be served as Tier-A certified
FAIL: the refusal is recorded as a coverage abstention
FAIL: no certified authority is claimed for the refused chain
FAIL: the refused chain abstains rather than serving a partial result
HOPGUARD_REFUSAL source=0 trust=0 coverage_abstains=0 rc=0
PERSONAL_AI_HOP_GUARD_FAIL checks=15 failures=4
```

`source=0` is `PERSONAL_AI_LOCAL` and `trust=0` is `HYBRID_TRUST_CERTIFIED`: the
product path served an answer built from a hop operating outside its certified
domain, and called it certified.

### Fix

* `route_execute_guarded()` in `src/router/route.c` takes the **same**
  `DagNodeGuard` the DAG path uses, so the two paths cannot drift into different
  policies. It consults the guard with the **canonical input each primitive is
  about to consume** — an intermediate for every hop after the first — and a
  refusal returns `ROUTE_EXEC_REFUSED_GUARD` before that primitive runs, so
  nothing downstream executes and no reliability evidence is recorded for a hop
  that never happened. `route_execute_ex` is now a NULL-guard call to it, so the
  legacy path is byte-for-byte unchanged.
* `personal_ai_serve` installs a guard that checks **owner + the executing
  primitive's exact typed interface + the actual value** via
  `hybrid_coverage_admits_exact` — the same predicate the benchmark injects.
  Missing or unsupported metadata (no unit, no BTN, not exactly one input and
  one output port) fails closed. A refusal is an **abstention**, handled exactly
  like the existing plan-level refusal, never a partial chain.

### GREEN — FRESH

```
make personal_ai_hop_guard
HOPGUARD_REFUSAL source=2 trust=2 coverage_abstains=1 rc=-1
PERSONAL_AI_HOP_GUARD_PASS checks=15 hops_guarded=every refusals=root+intermediate  # exit 0
```

`source=2` is `PERSONAL_AI_ABSTAIN`. The controls matter as much: a fully
covered chain is still served Tier-A certified, and still is *after* a refusal,
so the guard is not simply refusing everything.

---

## R10 (resumed under a narrow authorized exception) — the trainer plateau

The stop-and-report above was answered with an explicit, narrow authorization:
edit **`src/nn.c` only**, byte-preserving, leaving `include/nn.h`,
`src/legacy/main.c`, `tests/certify_demo.c` and every other baseline path
untouched. What follows is that work.

### RED — FRESH, in a new LF test that targets the TRAINER

The defect is in the trainer, so the reproduction is too — no protected test was
touched and `certify_demo` was not involved. `tests/test_btn_train_plateau.c`
uses the exact shape, data, hyperparameters and seed the failing primitive uses,
and applies the same certification predicate (`port_validate` →
`port_canonicalize` → exact compare) to all 256 exemplars.

```
make btn_train_plateau        # exit 2
BTN_PLATEAU_RUN seed=91 max_hidden=64 hidden_used=64 loss=0.008750 certifiable=240/256
BTN_PLATEAU_FIRST_MISS input=24 bit=3
FAIL: every exemplar certifies after dynamic growth
FAIL: a run that misses its target loss does not report success silently
BTN_PLATEAU_CONTROL seed=123 hidden_used=64 loss=0.002188 certifiable=256/256
BTN_TRAIN_PLATEAU_FAIL checks=5 failures=2
```

`input=24 bit=3` is exactly the first failure `certify_demo` reported, now
reproduced with no dependency on the demo or the certifier.

### What the fix is, and the two things it is not

Dynamic growth had exactly one lever — add a neuron — and a net that has settled
into a local minimum does not leave it because capacity appeared. `src/nn.c` now:

* **snapshots the best net ever seen**, scored on the **whole** sample set
  rather than the held-out slice. That detail was not optional: with 16
  exemplars the slice is three rows, and selecting by three rows measurably
  returned a net that was worse on the other thirteen — `hex_value` dropped to
  `DENIED (14/16)` until the score was corrected;
* when growth is exhausted and a window buys too little to be worth a neuron,
  **escalates**: perturb every weight, and every fourth stuck window **restart
  from the best snapshot** and re-draw half the hidden layer. Perturbing the
  current point repeatedly only explores one basin; restarting from the best
  makes each attempt an independent draw around the best net found;
* **engages only for a run that has not met the target it was given.** A run
  already at its target has nothing to escape from, and perturbing it can only
  cost exemplars — lower MSE is not the same as more exemplars certifying. Those
  runs take the unchanged path and return exactly what they always did;
* **reports a run that never escaped** as `BTN_TRAIN_PLATEAU` (-2.0) instead of a
  plausible loss, so a caller cannot persist an uncertifiable net with no signal
  that anything went wrong — which is exactly how `combine` shipped weights that
  failed certification while the demo printed a healthy-looking number.

Two things this deliberately is **not**: the seed was not changed (that is tuning
around the defect — the trainer would still have a plateau it cannot escape),
and nothing in `port_validate`, `port_canonicalize` or the certification
comparison was relaxed (that is hiding a confidently wrong bit).

### GREEN — FRESH

```
make btn_train_plateau
BTN_PLATEAU_RUN seed=91 max_hidden=64 hidden_used=64 loss=0.001718 certifiable=256/256
BTN_PLATEAU_CONTROL seed=123 hidden_used=64 loss=0.001254 certifiable=256/256
BTN_PLATEAU_IMPOSSIBLE loss=0.155344
BTN_TRAIN_PLATEAU_PASS checks=5 certifiable=256/256      # exit 0

make certify                                              # exit 0
  hex_value    vs hex_value_contract      : CERTIFIED (16/16)
  increment    vs increment_contract      : CERTIFIED (16/16)
  combine      vs combine_contract        : CERTIFIED (256/256)
  split        vs split_contract          : CERTIFIED (256/256)
CERTIFY PASS: tags are earned, imposters are refused, and plans can be certified end-to-end.
```

`combine` went from `DENIED (240/256)` at loss 0.008750 to `CERTIFIED (256/256)`
at loss 0.001718, with the exemplars, the contract, the port semantics and the
certification floors exactly as they were.

### The CRLF containment, handled explicitly

`src/nn.c` is 89510 bytes of pure CRLF (2756 line endings, zero lone LF, zero
lone CR) and was flagged assume-unchanged. The edit was applied by a script that
**refuses to write** unless: the file matches the expected size, CRLF count and
SHA-256 before the edit; every inserted line carries an explicit `\r\n`; no lone
LF or CR exists afterwards; the CRLF count equals the original plus exactly the
intended line delta; and **every old line the diff removes is quoted verbatim by
one of the anchors**. It printed the changed regions and the removed lines for
inspection on every run.

```
removed old lines: 5, all inside anchors
patched: 89510 -> 97764 bytes, CRLF 2756 -> 2918 (+162 lines)
```

Five lines removed — the old growth decision — and the rest pure insertion. The
blob was staged with `git hash-object -w --no-filters` so the raw CRLF bytes are
what lands in the index, not a clean-filtered LF copy.

---

## R13 — CI declares what it proves; the fault paths are executable

### Defect

`make ci_core` ran two scientific gates (capsule and accumulation) and nothing
recorded what CI was supposed to cover, so a gate could stop being enforced with
no signal. `tests/test_ci_workflow.py` printed
`CI_WORKFLOW_PASS status=gha_disabled` when there was **no hosted workflow at
all** — a local structural check labelled as a workflow pass. Separately, the
accumulation benchmark's failure paths read `names[k]` entries it had never
written and leaked the BTN they had just allocated; a green run never takes
those paths, so both were invisible.

### Fix

* `config/ci_contract.json` enumerates every gate with a **status**: `required`
  (must be a `ci_core` prerequisite and must pass), `blocked` (cannot run here,
  with the reason), `withheld` (a claim CI does not establish). It also records
  four withheld/failed *claims* — useful accumulation, portable continuous
  specialist, continuous coverage floors, evidence separated from runtime state
  — so the things CI does **not** prove are written down next to the things it
  does.
* `tests/test_ci_contract.py` enforces the contract against the Makefile: every
  `required` gate exists and is a prerequisite; every `blocked` gate is **not**
  a prerequisite (a gate that cannot run here cannot be part of a passing CI);
  `ci_core` declares no scientific gate the contract omits; and an absent hosted
  workflow may never be `pass`.
* `ci_core` grew from 2 scientific gates to **16**: recipe_gate, certify,
  btn_train_plateau, knowledge_capsule, capsule_scope_lineage, both benches,
  personal_ai_hop_guard, coverage_abstain, coverage_sidecar_seal, cnu_budget,
  vd_frontend_parse, port_raw_unit_seam, vision_coverage_test,
  vision_capsule_asset, capability_cert.
* `own_learning_health` and `vision_detection_bench_v2` are recorded as
  **blocked** with their reasons, and are deliberately *not* prerequisites.
  Making CI green by including a gate that cannot run here would be the same
  category of error this whole remediation is about.
* `tests/test_ci_workflow.py` now prints
  `CI_WORKFLOW_LOCAL_PASS status=local_gates_ok hosted_workflow=WITHHELD`.
* `tests/knowledge_accumulation_bench.c`: one cleanup path in `add_unit`,
  `name_out` written **only** on success, and the build loop tracks `built` so
  nothing indexes a name it did not build. `CNET_ACC_FAIL_AT` makes the fault
  path executable.

### RED → GREEN — FRESH

The forced-failure gate found a leak on the **success** path that no green run
had ever shown:

```
make knowledge_accumulation_faults        # exit 2
FAIL: no sanitizer finding on the k=3 fault path (exit 99)
  Direct leak of 7560 byte(s) in 7 object(s) allocated from:
    #1 add_unit tests/knowledge_accumulation_bench.c:186
  SUMMARY: AddressSanitizer: 72968 byte(s) leaked in 49 allocation(s).
```

`cnb_add_unit` serialises the unit into the base as a sealed byte image and
keeps no pointer, so the caller owns the BTN on **every** path — the success
path never freed it either. After the fix:

```
make knowledge_accumulation_faults
ACCUMULATION_FAULTS_PASS checks=9 forced_failures=3 sanitizers=asan+ubsan+leak  # exit 0
make knowledge_accumulation_bench
KNOWLEDGE_ACCUMULATION_BENCH_PASS units=32 ... ood_refused=96                   # exit 0
python3 tests/test_ci_contract.py
CI_CONTRACT_PASS required=16 blocked=2 withheld_claims=4 hosted_workflow=withheld  # exit 0
```

### One generated vision status, history intact

`plans/cnet_vision_portable_specialist_20260728.md` contradicted itself about
whether slices exist, whether a runner is committed, and whether anything had
been touched. A single **CURRENT STATUS — generated 2026-07-30** section is
appended, stating what was verified fresh and what is FAILED/BLOCKED. Nothing
above it was rewritten: the earlier snapshots are the historical record and are
left exactly as they were.

## R14 — Continuous vision boundary: prerequisites do not exist here

### What was inspected

```
ls -d data                          -> No such file or directory
find . -name 'vision_cache*' -o -name 'VOC*' -o -name '*.pack'   -> nothing
```

The only VOC cache is under the original checkout `/home/marble/AI/CNET/data`,
which this remediation may not touch and which a live learner writes to.

### Decision, and why it is the only honest one

A valid continuous-coverage rerun needs three things this worktree does not
have: **the data**; **a preregistered gate with immutable split hashes** (fitting
before the gate exists is how a floor gets chosen to match a result); and **an
unspent slice that stays unspent**. The ledger records `shuf[2800,3800)` and
`shuf[3800…4951]` as unspent, and this pass touched **neither**.

Nothing was trained, fitted, extracted, calibrated or scored. Therefore:

* **continuous coverage — FAILED**, against its own unchanged floors: natural
  OOD refusal 0.0906 vs ≥0.30 and ≥3× in-domain, synthetic 0.0245 vs ≥0.99;
* **portable continuous CNU — BLOCKED**, CNU1 rejects continuous exemplars.

Neither floor was relaxed, restated or moved. The executable schema, parser and
`PORT_RAW` safety gates all remain green and were re-run this pass.

## R15 — The sanitized seal gate could not run its own resource assertion

Found by running the full verification matrix rather than the narrow tests:
`make coverage_sidecar_seal_san` exited **2** while `make coverage_sidecar_seal`
exited 0.

### RED — the actual failure

```
$ make coverage_sidecar_seal_san
==67222==ERROR: Failed to mmap
##EXIT=2
```

ASan reserves a ~20 TB shadow mapping before `main()`, so the child's
`RLIMIT_AS` of 2 GB kills it on startup. Every amplification case died before
reaching the loader, so the target proved nothing at all — and the gate was
correctly non-zero about it.

The wrong fix is to drop `RLIMIT_AS` and let the target report a bare
`COVERAGE_SIDECAR_SEAL_PASS`: the log would then be indistinguishable from a run
that measured the bound and found it held.

### GREEN — withhold one assertion, name it, and prevent it from spreading

1. Only `RLIMIT_AS` is skipped under `COVSEAL_NO_RLIMIT`. **`RLIMIT_CPU` stays
   armed unconditionally** — ASan has no conflict with it, so the runaway guard
   is not given up.
2. Each skipped case prints
   `COVERAGE_AMPLIFIED_VMPEAK_WITHHELD reason=sanitizer_shadow_map <case>`,
   and the verdict line carries `vmpeak_withheld=<n>`. A reader can tell
   "bounded" from "not measured".
3. The sanitized recipe **requires** the withheld line to be present; the
   non-sanitized recipe **requires `vmpeak_withheld=0`**, so if the exemption
   ever leaks into that lane the amplification bound stops being measured
   anywhere and CI fails.

```
$ make coverage_sidecar_seal_san
COVERAGE_AMPLIFIED_VMPEAK_WITHHELD reason=sanitizer_shadow_map 2^20 rows x 2^20 dimensions declared by a two-line file
COVERAGE_AMPLIFIED_VMPEAK_WITHHELD reason=sanitizer_shadow_map 2^20 rows of 1024 dimensions declared by a two-line file
COVERAGE_AMPLIFIED_VMPEAK_WITHHELD reason=sanitizer_shadow_map 4096 rows of 2^16 dimensions declared by a two-line file
COVERAGE_AMPLIFIED_VMPEAK_WITHHELD reason=sanitizer_shadow_map 2^20 tiny rows declared by a two-line file
COVERAGE_AMPLIFIED_VMPEAK_WITHHELD reason=sanitizer_shadow_map a two-line file declaring a gigabyte of coverage rows
COVERAGE_SIDECAR_SEAL_PASS checks=134 mutations=20 amplified=5 owners=multi partial_state=none vmpeak_withheld=5
##EXIT=0

$ make coverage_sidecar_seal
COVERAGE_AMPLIFIED verdict=0 vm_peak_kb=24296 cpu=0.000 2^20 rows x 2^20 dimensions declared by a two-line file
COVERAGE_AMPLIFIED verdict=0 vm_peak_kb=24296 cpu=0.000 2^20 rows of 1024 dimensions declared by a two-line file
COVERAGE_AMPLIFIED verdict=0 vm_peak_kb=24296 cpu=0.000 4096 rows of 2^16 dimensions declared by a two-line file
COVERAGE_AMPLIFIED verdict=0 vm_peak_kb=24296 cpu=0.000 2^20 tiny rows declared by a two-line file
COVERAGE_AMPLIFIED verdict=0 vm_peak_kb=24296 cpu=0.000 a two-line file declaring a gigabyte of coverage rows
COVERAGE_SIDECAR_SEAL_PASS checks=140 mutations=20 amplified=5 owners=multi partial_state=none vmpeak_withheld=0
##EXIT=0
```

All five bounds are still measured, in the target that can measure them: VmPeak
flat at the 24296 kB control for a file declaring a gigabyte of rows. The 6-check
difference (140 vs 134) is exactly the 5 skipped assertions plus the baseline
`the child reports its peak address space`.

### RED for the anti-spread guard

Simulating the exemption leaking into the non-sanitized lane — the binary alone
is happy, the gate is not:

```
$ COVSEAL_NO_RLIMIT=1 ./bin/test_coverage_sidecar_seal ; echo binary=$?
COVERAGE_SIDECAR_SEAL_PASS checks=134 ... vmpeak_withheld=5
binary=0
$ grep -q 'vmpeak_withheld=0' <log> ; echo guard=$?
guard=1
```

# Phase 3 — Codex re-review (2 High, 4 Medium)

## F1 — assume-unchanged paths escaped evidence binding (HIGH)

Both canonical evidence runners bound the working tree by walking
`git status --porcelain`, which deliberately does not report paths marked
assume-unchanged or skip-worktree. Both noticed and recorded a **count** of
such paths, which names a blind spot without closing it.

In this repository the gap is not hypothetical:

```
$ git ls-files -v | awk '{print substr($0,1,1)}' | sort | uniq -c
   1937 H
     83 h
```

83 of 2020 tracked paths, including `src/nn.c`, `include/nn.h` and
`src/legacy/main.c` — the trainer, its public header and the legacy persistence
path.

### RED (re-runnable: `CNET_EVIDENCE_LEGACY=<rev>` installs the pre-fix runners)

```
$ CNET_EVIDENCE_LEGACY=HEAD python3 tests/test_evidence_special_index.py
GATE_EVIDENCE_MUTATED exit=0
FAIL: gate_evidence must REFUSE a run that rewrote an assume-unchanged tracked file (exit was 0)
FAIL: gate_evidence must name the drift it refused on
GATE_EVIDENCE_PRISTINE exit=0
CAPABILITY_CERT_MUTATED exit=0
FAIL: run_capability_cert must REFUSE to certify a run that rewrote an assume-unchanged tracked file (exit was 0)
FAIL: run_capability_cert must name the drift it refused on
CAPABILITY_CERT_PRISTINE exit=0
GATE_EVIDENCE_SYMLINK exit=0
FAIL: the evidence JSON must record a special-index digest; a runner that does not emit one cannot have bound those paths
GATE_EVIDENCE_SYMLINK_REPOINTED exit=0
FAIL: a bound symlink must be identified by WHERE IT POINTS: /dev/zero and /dev/null both read as empty, so an equal digest proves the binding followed the link instead of recording it
GATE_EVIDENCE_SYMLINK_MOVED exit=0
FAIL: repointing a bound symlink mid-run must be refused as drift (exit was 0)
EVIDENCE_SPECIAL_INDEX_FAIL checks=15 failures=7
##EXIT=1
```

Both PRISTINE controls pass in the RED lane, so the suite is not merely
refusing everything, and the two premise checks confirm `git status` really is
blind to the mutation — otherwise the RED would prove nothing.

### GREEN

```
$ python3 tests/test_evidence_special_index.py
GATE_EVIDENCE_MUTATED exit=1
GATE_EVIDENCE_PRISTINE exit=0
CAPABILITY_CERT_MUTATED exit=1
CAPABILITY_CERT_PRISTINE exit=0
GATE_EVIDENCE_SYMLINK exit=0
GATE_EVIDENCE_SYMLINK_REPOINTED exit=0
GATE_EVIDENCE_SYMLINK_MOVED exit=1
EVIDENCE_SPECIAL_INDEX_PASS checks=15 runners=2 special_index=bound symlinks=identity_only
##EXIT=0

$ make evidence_special_index
GATE_RUN gate=evidence_special_index ... special_index=f9344f9ffe6909c4 dirty=4 special_index_files=83
EVIDENCE_SPECIAL_INDEX_PASS checks=15 runners=2 special_index=bound symlinks=identity_only
GATE_PASS gate=evidence_special_index ...
EVIDENCE_SPECIAL_INDEX_RED_CONFIRMED rev=dc3b2a2
##EXIT=0
```

### What was implemented

* `special_index_binding()` in **both** runners digests flag + path + content
  for every non-`H` index entry (assume-unchanged and skip-worktree). Ordinary
  cached paths are skipped on purpose — a change to one appears in
  `git status` and is already bound by the dirty walk, so hashing all 2020
  twice per gate would buy nothing. The flag letter is part of the digest, so
  setting or clearing a bit is drift too.
* `path_identity()` replaces every `Path.is_file()` content read in both
  runners. `is_file()` FOLLOWS symlinks: a link to `/dev/zero` in a bound set
  reads until the gate dies, and a link outside the repository binds bytes this
  tree does not own. A link is now bound by where it points; only regular files
  are opened; fifos/sockets/devices bind their mode.
* A 512 MiB ceiling on special-index content. CNET's 83 paths hold 1.27 MB, so
  the ceiling is ~400x the real cost. Crossing it REFUSES rather than sampling.
* `make evidence_special_index` runs the suite AND re-runs it against the
  pre-fix runners, failing if those pass.

`tests/test_gate_evidence.sh` previously asserted "the binding discloses the
assume-unchanged count". That assertion was replaced by two stricter ones — a
64-char `special_index_sha256` and disclosure of file count and byte size —
because the count was the inferior disclosure, not because it was inconvenient.

## F5 — capability terminal marker was a substring test (MEDIUM)

`run_capability_cert.py` asked `marker not in completed.stdout`. A raw substring
test certifies on `CAP_X_PASSED`, on `NOT_CAP_X_PASS`, on a mid-sentence
mention, and on a log printing `CAP_X_PASS` and `CAP_X_FAIL` one line apart; it
also counts no duplicates, so two concatenated evaluator runs read as one.

### RED

```
$ python3 tests/test_capability_cert_runner.py
AttributeError: module 'capability_cert_runner' has no attribute 'terminal_marker_ok'
   (10 errors)
##EXIT=1
```

with a standing witness for the defect itself, which survives as a regression
test rather than only proving the API was absent:

```python
output = "NOT_CAP_X_PASSED_YET waiting for the real gate\n"
self.assertIn("CAP_X_PASS", output)          # the predicate that used to certify
found, _ = RUNNER.terminal_marker_ok(output, "CAP_X_PASS")
self.assertFalse(found)                      # the current one refuses
```

### GREEN

```
$ python3 tests/test_capability_cert_runner.py
Ran 52 tests in 0.078s
OK
CAPABILITY_CERT_RUNNER_PASS shell=disabled receipt=required checks=52
##EXIT=0
```

39 -> 52 checks.

### The split, and why it is not a loophole

Auditing the six real manifests showed `required_marker` is always a whole-line
verdict, but a fixture's `expected_markers` are deliberately FIELD PROBES —
`acc_on=`, `semantic=2`, `classes_used=4`, `authority=cnet` — meant to match
inside a line. Anchoring those would have broken six honest capabilities for no
integrity gain. So:

* `required_marker` — the verdict — gets the anchored, exactly-once,
  no-rival-terminal-verdict rule;
* `expected_markers` stay substring probes, **except** any that is SHAPED like a
  verdict (`looks_terminal`), which would otherwise be a way to smuggle a
  terminal claim through the weaker path. Two tests pin both halves.

### One manifest was genuinely wrong, and got stronger

`honest_memory_retrieval` declared its `required_marker` as a dotnet test METHOD
NAME:

```
  Passed CNET.Cce.Llm.Tests.MemoryLookupLoopTests.AutoRecall_GenuineEmpty_TellsModelNotToInvent [25 ms]
```

That substring is present whether the line says `Passed` or `Failed` — the
marker proved the test was *mentioned*, never that it passed, and only
`returncode == 0` was actually catching a failure. Fixed by asserting the run
verdict and demoting the method name to what it always was:

```
required_marker : "AutoRecall_GenuineEmpty_TellsModelNotToInvent"
               -> "Test Run Successful."
expected_markers: ["Passed"]
               -> ["CNET.Cce.Llm.Tests.MemoryLookupLoopTests.AutoRecall_GenuineEmpty_TellsModelNotToInvent",
                   "Passed: 1"]
```

No floor moved: `absolute_floor` stays 1.0. The assertion is strictly stronger —
a failed run now fails the marker itself, not merely the exit code.

All six manifests re-verified against their committed evidence logs under the
new rule: `BAD: 0`.

## F6 — the fault harness accepted any abnormal exit (MEDIUM)

`tests/test_accumulation_faults.sh` asserted the forced-failure line appeared
and that the exit status was **not 99** (the ASan exitcode). Nothing else was
checked, so a bench that printed the right words and then exited 0, exited 7,
segfaulted, or was killed by `timeout` all counted as a clean fault path. A
harness whose whole job is to prove the failure path is clean cannot treat
"crashed" and "refused correctly" as the same outcome.

The documented contract for an injected failure, read off the bench itself:
print `build FAILED at k=<at>`, print `KNOWLEDGE_ACCUMULATION_BENCH_FAIL`, exit
exactly **1**.

### RED — false-green fakes, run against the real harness

`tests/test_accumulation_faults_harness.sh` writes six fake benches into a
mkdtemp root. All six print exactly what an honest injected failure prints; only
what they do next differs.

```
$ sh tests/test_accumulation_faults_harness.sh
ACCFAULT_CASE bench=honest        harness_exit=0
FAIL: a bench that prints the failure marker and then exits 0 must be REFUSED (harness exit 0, expected nonzero)
ACCFAULT_CASE bench=exit_zero     harness_exit=0
FAIL: a bench that exits 7 instead of the documented 1 must be REFUSED (harness exit 0, expected nonzero)
ACCFAULT_CASE bench=exit_wrong    harness_exit=0
FAIL: a bench that prints the marker and then segfaults must be REFUSED (harness exit 0, expected nonzero)
ACCFAULT_CASE bench=crash         harness_exit=0
ACCFAULT_CASE bench=hang          harness_exit=124
ACCFAULT_CASE bench=asan_finding  harness_exit=1
FAIL: a crash must be reported as a signal, not as a wrong exit code
FAIL: a timeout must be reported as a timeout
ACCUMULATION_FAULTS_HARNESS_FAIL checks=8 failures=5
##EXIT=1
```

The `hang` case returned 124 only because the *outer* `timeout` in the test
killed it — the harness had no budget of its own that it honoured.

### GREEN

```
$ sh tests/test_accumulation_faults_harness.sh
ACCFAULT_CASE bench=honest        harness_exit=0
ACCFAULT_CASE bench=exit_zero     harness_exit=1
ACCFAULT_CASE bench=exit_wrong    harness_exit=1
ACCFAULT_CASE bench=crash         harness_exit=1
ACCFAULT_CASE bench=hang          harness_exit=1
ACCFAULT_CASE bench=asan_finding  harness_exit=1
ACCUMULATION_FAULTS_HARNESS_PASS checks=8 exit_code=exact signals=refused timeouts=refused
##EXIT=0

$ make knowledge_accumulation_faults
ACCUMULATION_FAULTS_HARNESS_PASS checks=8 exit_code=exact signals=refused timeouts=refused
ACCUMULATION_FAULTS_PASS checks=12 forced_failures=3 sanitizers=asan+ubsan+leak
##EXIT=0
```

The real bench still passes, at 9 -> 12 checks. What changed:

* the exit status must be exactly 1;
* 124 is reported as a timeout, `>= 128` as `signal N`, 99 as a sanitizer
  finding — three distinct refusals rather than one generic failure, because a
  harness that cannot say which of those happened is not much better than one
  that ignores all three;
* `KNOWLEDGE_ACCUMULATION_BENCH_FAIL` must be present, so the bench's own
  verdict is asserted and not just its progress line;
* `ACC_FAULT_TIMEOUT` makes the budget injectable so the hang fixture is
  bounded at 3s instead of 900s. The default is unchanged at 900.

ASan/UBSan/leak detection is untouched and still asserted.
