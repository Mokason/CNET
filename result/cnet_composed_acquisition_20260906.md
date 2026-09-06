# Approved-tool composition acquisition — September 6, 2026

## Outcome

Implemented automatic multi-step acquisition through operator-approved tool
policies. Requests no longer need a direct policy pair: a bounded native planner
discovers typed, value-valid paths; the existing worker acquires ordinary
per-edge curriculum jobs; existing certification and historical replay control
publication. Serving still requires coverage at every hop.

This advances the previous remaining transfer scope to **held-out composed task
pairs over covered primitive inputs**. It does not establish arbitrary-domain
teacher discovery, broad language understanding or unseen-input accuracy.

## Concrete behavior

- Native `cnet_capsule_tool plan POLICY INPUT OUTPUT VALUE` validates the whole
  private policy and emits a complete path, or refuses without a consumable
  partial path. Duplicate pairs and inconsistent interface widths refuse.
- Value-state search handles shared suffixes and positive cycles; it supports
  eight hops, 1,024 states and 65,536 charged edge/visited comparisons.
- Evidence bounds intersect policy with representable output values. Input 25
  under 8-bit multiplication by 10 now acquires rows 0–25; neighboring overflow
  at 26 cannot trap the valid request in endless retries.
- The worker retains a demand until all required jobs are durable. At most two
  new jobs are published per tick; exact existing jobs do not consume that
  allowance, so longer paths progress across ticks.
- A total 2,048 evidence invocation budget counts retries and failed calls.
  Planner oracle work is separately bounded and reported. Work/state failures
  preserve demand; no approved path within the supported eight hops is terminal.
- No new capsule packaging, network integration, certification bypass or
  training on CNET predictions was introduced.

## Evidence

The actual socket test starts from an unverified `alpha → delta` demand with
only these three approved primitive policies:

| Primitive | Independent tool rule |
|---|---|
| alpha → beta | `beta = alpha XOR 15`, 5-bit ports |
| beta → gamma | `gamma = beta * 2`, 5-bit to 6-bit |
| gamma → delta | `delta = gamma XOR 63`, 6-bit ports |

The first tick acquires two jobs and preserves demand. The next acquires the
third and resolves 16 composed inputs. One additional suffix block unlocks the
other half, while keeping the prefix jobs unchanged. The test then checks all
32 inputs for each of `alpha → gamma`, `beta → delta` and `alpha → delta`:
**96/96 correct verified socket answers**. None of those three task pairs appears
as a direct primitive training job. This is not an unseen-primitive-input test.
Restart, portable-root query and OOD refusal also pass.

Focused gates passed:

- `capsule_tool_plan`: paths, shared suffix, positive cycles, signatures,
  invalid/truncated policy, representable bounds, `/dev/full` output failure,
  and work/state exhaustion.
- `capsule_composed_acquire`: the 96-case task-pair transfer proof above.
- `capsule_composed_refusal`: planner failure retains demand without publishing;
  over-depth requests terminate without acquisition; fault-injected evidence
  failures count against work and retain demand.
- Prior direct demand growth and acquisition-security suites remain green.
- Native planner AddressSanitizer/UndefinedBehaviorSanitizer tests with leak
  detection passed. No sanitizer errors were found in that run.

RED logs: `/tmp/cnet-tool-plan-red.log`, `/tmp/cnet-composed-red.log`,
`/tmp/cnet-plan-output-red.log`, `/tmp/cnet-composed-call-count-red.log`.
Focused combined log: `/tmp/cnet-composed-final-focused.log`.
Sanitizer log: `/tmp/cnet-composed-sanitizer.log`.

Full isolated verification completed with exit **0**:

| Gate | Result |
|---|---|
| `make verify` | 28/28 suites, fresh-log binding passed |
| `make capability_cert` | 6/6 certified; graded scores unchanged at 0.861 and 0.775 |
| `knowledge_capsule` | 94 checks passed |
| `coverage_abstain` | 55 checks passed |
| `knowledge_accumulation_bench` | 32/32 isolation, 8/8 replay, 96 OOD refusals |
| `knowledge_composition_bench` | Three members; every-hop coverage enforced |
| `own_learning_health` | Passed |
| Source/diff checks | Ten implementation/test/guide files match the audit copy; `git diff --check` passed |

Capability run: `33eb6b5a-b256-42ae-a4ec-9c2044e321db`, bound to audit worktree
SHA-256 `f1cef48f325378383e781e35ad3b5f810783d49632c14c07b16ff69cf22b702e`.
Report: `/tmp/cnet-composed-audit-UoXqkA/logs/capability_cert.json`.
Logs: `/tmp/cnet-composed-verify.log`, `/tmp/cnet-composed-capability.log`,
`/tmp/cnet-composed-knowledge.log`. The final report/checklist were written after
the copied-source audit; the dirty workspace is not represented as clean HEAD.

## Review and provenance

The interface, security and incremental-implementation skills kept policy
authority separate from serving authority and required small RED-to-green
steps. Independent design/code reviews required representable evidence bounds,
complete-output status handling, positive-cycle semantics, total work limits
and counting failed invocations. Those findings were resolved and rechecked.
No external cross-model CLI was invoked without user approval.

Audit copy: `/tmp/cnet-composed-audit-UoXqkA`, including the dirty workspace's Git
metadata. Key source SHA-256 identities:

| File | SHA-256 |
|---|---|
| `tools/cnet_capsule_tool.c` | `04275b477ea1f36e193c3fe0978a546414ef20bc4be89d7766a2add3eaf3cf19` |
| `tools/cnet_capsule_tool_plan.c` | `40a0869934fbe38b8694d23da726db765790ccff9a1389c3153ba7f33719f11f` |
| `tools/cnet_capsule_tool_internal.h` | `19fc61614077acc95d57f7cb0494c8f09fe272310af6af81ceef18f1a0a34cd5` |
| `scripts/cnet_capsule_acquire_tick.sh` | `bac8f7ce279c58c5a30359a89df0e9c66a81f52815a1c8cdd6b144b5e8703754` |

No service restart, live policy change or commit was made. Unrelated existing
worktree changes were preserved. Usage is in
[the capsule guide](../docs/CAPSULE_CORE.md#discovering-approved-multi-step-paths).

## Still remaining

- Discovering trustworthy teachers outside the operator-approved tool graph.
- Broader language intent fidelity and held-out primitive/domain transfer.
- Larger-inventory latency/cost and optimization based on measured bottlenecks.
- Full power-loss recovery across the legacy writer set.
- Operator-selected live activation of automatic acquisition.

These are separate gates and remain WITHHELD, not implicitly completed by the
96-case composition result.
