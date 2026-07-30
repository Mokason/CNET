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
