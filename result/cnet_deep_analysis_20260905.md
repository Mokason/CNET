# CNET deep analysis — 2026-09-05

## Conclusion and solution

CNET has a working foundation for Artificial Specialized Intelligence: typed
specialists, contract admission, portable capsules, guarded composition, and
external-teacher acquisition. Fresh tests support those mechanisms. The main
obstacle is that the serving and learning surfaces do not consistently inherit
the foundation's guarantees.

The solution is to make the existing certified runtime the authority for every
certified response and every promotion. Keep semantic proposals, memory,
teacher drafts, and fast execution backends, but bind a certified answer to the
actual executed unit, input coverage, artifact generation, and evidence. Extend
the existing capsule/CNB path; do not create another packaging system.

First repair the concrete authority failures below. Then demonstrate useful
growth through one real front-door workflow: unsupported request → externally
verified examples → certified capsule → fresh-runtime import → correct local
answer → preserved abstention outside coverage. Expand coverage only when that
workflow passes on independently held-out requests.

## Scope and evidence

Analyzed committed HEAD `2d298cade27ff7196e551ccefdc51af43e87638c` plus the
current working tree, which contains substantial additional work. At the final
inventory there were 56 changed tracked entries and 177 untracked entries.
This is a targeted architectural and behavioral analysis, not an exhaustive
audit of all 1,805 tracked files under src/include/tests/tools/dotnet.

Used the codebase graph for discovery and source snippets. Its outbound trace
for `cd_ask` returned no callees despite direct calls visible in source; direct
file reads and text searches supplemented that incomplete relationship data.

No production source, service configuration, certification floor, or deployment
was changed. Tests rebuilt test artifacts and wrote logs. Adversarial probes ran
in `/tmp/cnet-analysis-probe-Pzv2bJ`. No chat requests were sent to the active
daemon, because its request path can trigger learning and other side effects.

### Fresh verification

| Check | Observed result | Scope |
|---|---|---|
| `knowledge_accumulation_bench` | PASS: 32/32 isolated units, 8/8 transfers, 8 exact coverage transfers, 96 OOD refusals, zero drift on 8 probes of unit 0 | Distinct functions over 8 symbols; not broad task acquisition |
| `knowledge_capsule` | PASS: 94 checks | Capsule transfer and refusal fixtures |
| `coverage_abstain` | PASS: 55 checks; reported held-out correctness 4/4 versus 0/4 | Small targeted coverage fixture |
| `knowledge_composition_bench` | PASS: 3 members, every hop guarded, root and intermediate refusals | Small typed composition mechanism |
| `cce_train_bench` | PASS: classification 0.861, majority baseline 0.274, lift 0.587, 4/4 classes | 1,000 held-out synthetic classification draws |
| `core_bus_untagged`, `core_bus_tensor_refusal` | PASS | Addressing and named-tensor refusal |
| `cnetd_protocol_boundary`, `cnet_mcp_transport`, `roe_process_security` | PASS | Framing/deadlines, partial writes, argv-based document tools |
| `distrust_loop`, `autonomy_spine` | PASS | Trust-policy and autonomy fixtures; do not catch the serving defect below |
| `own_learning_health` | 45 strict negative checks PASS; default deployed check BLOCKED | Make defaults to absent `logs/personal.cnb` |
| Health check with the base named by the active service | FAIL: missing coverage file, one unguarded mined unit | Repository health profile plus `soul_gemma4v2_final.cnb`; not a complete inspection of process environment |
| `capability_cert` | BLOCKED at evaluator prerequisites | Selected .NET installation has runtime 10.0.7, while control-plane tests require 8.0 |
| `build_integrity` | FAIL | Makefile 8,540 lines exceeds configured 8,519 ceiling |

These runs used the current incremental build. The header-dependency defect
below prevents treating them as a clean rebuild attestation. No full T1/T2,
GPU parity, open-ended chat evaluation, or new large-scale benchmark was run.

The generated claims ledger remains dated August 19 and limited to four
autonomy claims. It is not fresh evidence for the present working tree.

## Findings with concrete remedies

### P0 — A fallback can borrow trust without executing a fallback

**Reproduced end to end.** `personal_ai_serve` executes its initial specialist or
plan, then checks domain trust. `distrust_on_tier_a_refuse` can replace the domain
name with `CNET_DISTRUST_REROUTE_DOMAIN` and return success. The caller returns
the output it already computed. No alternative unit is resolved or executed.

Probe: admit exactly one local rot2 specialist; disable exploration; leave its
domain cold. With no alternative the request refuses. Add a mature trust-ledger
entry named `ledger_only_no_specialist`, without adding any specialist, and
select it as the fallback:

```text
without_alternative rc=-1 local=0 trust=2 registry_units=1
with_ledger_only_alternative rc=0 local=1 certified=1 answer=3 registry_units=1
```

Sources: `src/personal_ai.c:202`, `:644`, `:733`;
`src/cnet_distrust.c:38`. Existing distrust tests exercise ledger decisions,
not whether the serving path executes a different unit.

**Remedy:** when the original domain refuses, invalidate its candidate output.
Resolve an actual alternative compatible with the original requested contract;
check that alternative's coverage and trust; execute it into a separate output;
publish only that result. A ledger-only name must refuse or reach the explicitly
uncertified residual path. Record the executed unit and generation with the
trust decision.

**Acceptance:** source and target produce distinguishable outputs; assert target
execution and target output. Also test nonexistent target, incompatible ports,
target OOD, and target execution failure. A policy decision alone cannot count
as a successful reroute.

### P0 — Plain LUT files can become certified answers without validation

**Reproduced.** `load_one_lut` accepts `tag=audit` and an empty `lut=` line. It
does not require 16 successful numeric conversions, a seal, a contract, or a
coverage record. `cnet_serve_result` sets `proved=1` and `claimed_cert=1`.
`cnetd` converts that result to `LOCAL` and `verified=1`.

```text
load_rc=0 bricks=1
query=audit 3 rc=0 proved=1 cert=1 input=3 answer=0
query=audit -3 rc=0 proved=1 cert=1 input=3 answer=0
query=audit 3.5 rc=0 proved=1 cert=1 input=3 answer=0
query=audit 3 trailing rc=0 proved=1 cert=1 input=3 answer=0
```

The final three cases also expose partial request parsing: signs, fractional
suffixes, and trailing text are not validated as a complete typed input.

Sources: `src/serve/cnet_core_serve.c:29`, `:130`, `:196`;
`include/cnet_core_serve.h:4`; `tools/cnetd.c:680`, `:1216`.

**Remedy:** immediately refuse malformed tables and malformed typed requests.
An unverified table may remain an explicitly uncertified artifact. For certified
execution, use a LUT only as an acceleration representation of an admitted
capsule, bound to its identity and coverage. Check equivalence against the
independent contract examples before exposing it. Parse the entire request;
reject unconsumed semantic input.

**Acceptance:** empty/short/nonfinite/out-of-range tables cannot load as certified;
content mutation invalidates authority; negatives and fractions cannot silently
become positive integers; a valid capsule survives export/import and still
answers correctly. Preserve the fast path after those checks.

### P0 — Learning provenance and promotion are not an enforced boundary

**Provenance bypass reproduced.** `miss_row_self_authored` tests compact JSON
substrings instead of parsed fields:

```text
excluded=1 row={"source":"CORE","self_authored":true}
excluded=0 row={"source": "CORE", "self_authored": true}
excluded=0 row={"source":"LOCAL","claimed_cert":1}
```

`load_misses` uses this filter before counting answers as votes. Equivalent JSON
can therefore count CNET's own answer as learning evidence. This proves evidence
intake bypass; it does not by itself prove every such row reaches deployment.

**Additional source-confirmed promotion bypass:** the tick records the result of
`promote_via_front_door`, then calls `write_skill` regardless of that result.
`write_skill` writes `certified 1` and updates the catalog. Repeated-answer
promotion can also bypass reviewer execution when reviewer mode is disabled.

Sources: `tools/roe_evolve_tick.c:694`, `:705`, `:996`, `:1023`, `:1539`, `:1581`.

**Remedy:** parse a typed provenance record and require positive evidence of an
allowed source: external teacher, explicit user correction, or verified tool
result. Reject unknown or conflicting provenance for training. Retain rejected
answers only as evidence of demand. Keep proposals separate from admitted units;
publish atomically only after successful verification through the canonical
admission path. Repetition is a scheduling signal, not proof of correctness.

**Acceptance:** whitespace/key-order variants behave identically; LOCAL/Tier-A
answers never vote; unknown provenance never trains; failed reviewer/admission
leaves the serving catalog unchanged; successful external evidence promotes
exactly once with an auditable receipt.

### P1 — A failed capability test prints a PASS marker

**Observed during `make capability_cert`.** The runner's test host aborted due
to missing .NET 8, but `logs/capability_cert_runner.log` still ends with
`CAPABILITY_CERT_RUNNER_PASS`.

The recipe uses `$?` and `$status` where Make requires `$$?` and `$$status`.
The dry run expands them to a prerequisite list and `tatus`. It then continues
to emit PASS because the commands are separated with semicolons. The later
evaluator-prerequisite gate correctly fails, so the aggregate did not pass.

Source: `Makefile:6410`. This is a reproducible evidence-integrity defect,
separate from the missing runtime prerequisite.

**Remedy:** use the existing `gate_evidence` wrapper for this target. Preserve
producer exit status and append PASS only after success. Select/install a
matching .NET 8 runtime alongside .NET 10 when executing that test lane.

**Acceptance:** a producer that prints a plausible marker and exits nonzero,
an aborted testhost, and zero tests executed all fail without a success receipt.

### P1 — Build and verification do not cover the same product boundary

**Reproduced:** `make -n -W include/cce/cce_tensor.h libcce` reports nothing to
do. The object pattern rule lists only its `.c` file. Header changes do not
invalidate the archive. Compiler/flag changes also lack a build fingerprint in
that rule.

Source: `mk/cce_lib.mk:29`. Add compiler-generated dependencies (`-MMD -MP`,
included `.d` files) and a compiler/flag identity prerequisite or build-directory
key. Verify by changing a public header in an isolated build and observing the
affected object and archive rebuild.

The default T1/T2 lists also omit the newly verified socket/core-bus/ROE security
boundaries. Tier synchronization compares its existing log rows to dependencies;
it cannot detect a critical behavior absent from both lists.

**Remedy:** add the inexpensive serving authority/provenance regressions to T1
and its log inventory. Keep expensive scale and GPU tests in explicit lanes.
Move the extra Makefile rules into appropriate `mk/*.mk` fragments to satisfy
the existing size ceiling; do not raise it to obtain a green result.

### P1 — The deployed learning profile has missing coverage evidence

Read-only service inspection found `cnetd` and `cnet-personal-ai-lane` active,
and the evolve timer waiting. The learning service names
`/home/marble/AI/CNET/soul_gemma4v2_final.cnb`, not the Make health target's
default `logs/personal.cnb`.

Checking that named base against `config/personal-ai.env` produced:

```text
base_loaded=1 mined_units=1 coverage_records=0 unguarded_mined=1
coverage_file=missing coverage_gate=on residual_teacher=reachable
OWN_LEARNING_HEALTH_FAIL count=2
```

This is a failed artifact/profile health check, not proof that the running
process serves that unguarded unit. Runtime refusal may be working as intended.

**Remedy:** have the health command consume the same deployment profile and base
identity as the service. Recover the matching verified coverage evidence or
reacquire and recertify the affected unit from allowed external evidence. Keep
it unavailable for certified serving until that succeeds. Never fabricate an
empty coverage file or turn off the guard to pass health.

## What to build after the authority repairs

The repository already contains a stronger request-admission model in
`src/compete/cnet_compete_runtime.c:2145`: learned intent proposal plus an
independent typed semantic check. The August 14 bounded competition report
records 432/448 correct decisions and 304/304 correct answered covered requests.
That is historical evidence for a narrow suite, not a fresh result or a broad
chat-quality claim. Its closed vocabulary also limits coverage.

Reuse the principle and existing capsule execution machinery in the product
path. A semantic model can propose a typed request, but certified execution
requires exact argument consumption, compatible ports, covered inputs, current
unit identity, and a passing result check. This allows richer phrasing without
granting prose generation certification authority.

Recommended delivery sequence:

1. **Make evidence dependable:** fix the false PASS, build dependency tracking,
   and gate membership. Preserve every current certification floor.
2. **Close serving authority:** reproduce the two P0 serving failures as RED
   tests, fix fallback execution and LUT admission, and run those tests through
   the actual front-door formatting path as well as native APIs.
3. **Close learning authority:** parsed provenance, successful admission before
   catalog publication, atomic publication, and idempotent retry.
4. **Repair the named deployment artifact:** recover/reacquire bound coverage,
   verify on a fresh runtime, and report health for that exact generation.
5. **Prove one useful growth loop:** select a real repeated typed task from
   organic demand, freeze independent evaluation requests, teach from allowed
   evidence, admit/export/import via CNB/capsules, and replay through `cnetd`.
6. **Expand by measured benefit:** prioritize tasks by independently verified
   successful local answers and teacher calls avoided per acquisition cost.

The growth evaluation must compare teacher-only, CNET-before, and CNET-after
on the same frozen requests. Report correct local coverage, wrong certified
answers, OOD refusal, residual calls, acquisition cost, and latency separately.
Distinguish ground-truth correctness from fidelity to a teacher. Include
paraphrases, near-domain negatives, restart/import, and coexistence with old
units. A larger registry is useful only if these measurements improve.

This keeps the viable core and gives CNET a measurable route to broader
specialized competence. Broad semantic coverage, open-ended task quality, and
large-scale composition remain WITHHELD pending their own evaluations.

## Evidence locations

- Fresh aggregate logs: `/tmp/cnet-analysis-{core,boundaries,capability,build,mechanisms,deployed-health}-20260905.log`.
- Named gate logs and JSON: `logs/knowledge_accumulation_bench.*`,
  `logs/knowledge_capsule.log`, `logs/coverage_abstain.log`,
  `logs/knowledge_composition_bench.log`, `logs/cce_train_bench.log`,
  `logs/capability_cert_runner.log`, `logs/capability_evaluator_prereq.log`.
- Standalone probes and binaries: `/tmp/cnet-analysis-probe-Pzv2bJ/`:
  `lut_probe.c`, `evolve_probe.c`, `reroute_probe.c`. They compile the current
  implementation; the reroute probe reuses the existing personal-AI fixture's
  training helper and changes only its own process-local trust settings.
- Historical bounded-suite result:
  `plans/cnet_7b_competition_v5_results_20260814.md`.

Review method: the code-review skill focused the analysis on correctness,
architecture, security, build evidence, and concrete structural remedies. The
debugging skill drove isolated reproduction before accepting the key findings.
