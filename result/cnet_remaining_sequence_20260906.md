# Remaining product sequence — September 6, 2026

## Verified implementation

The private capsule core now supports the existing 256-directory ceiling with
lazy 64-record coverage banks. Public HybridAi layout, trace capacity, capsule
format, certification floors, search limits, and historical replay budgets
remain unchanged. An immutable input-Port index avoids scanning unrelated
edges on every historical obligation. Every selected edge still executes its
canonical coverage and certification checks.

The original binaries failed at the 64→66 test. Banks alone reached 85
published capsules before exhausting historical replay; the diagnostic showed
zero remaining work at 8,074 obligations. The indexed implementation passed
the complete 2/16/64/256 sweep. No floor or work limit was relaxed.

At 256 capsules, three repetitions checked 12,288 distinct covered requests
(36,864 total), plus 1,155 expected refusals: zero wrong certified answers,
unexpected abstentions or invalid results. There were 128 composed input/output
interface pairs without direct composed training labels. This proves the
synthetic typed-composition workload, not unseen primitive competence.

Acquisition used 8,192 verified-tool calls and supplied primitive rows; cumulative
publication time was 22.080039 seconds. Serving teacher calls were zero.
Monetary and energy cost are unmeasured (`null`). The 257th capsule refused
with `capsule_count_limit`.

## Socket measurement

At 256 capsules, each concurrency level issued 128 real socket requests:
112 covered and 16 OOD. Every level had zero incorrect results and zero
transport failures.

| Clients | p95 milliseconds |
|---|---:|
| 1 | 36.156821 |
| 4 | 132.620764 |
| 16 | 534.106149 |

These are private-daemon, serial-server round trips including client startup
and queueing, not production SLAs. The default authority gate also runs a
small 1/4-client benchmark contract with explicit counts and invalid-config
refusal.

## Intent and recovery

New finite grammar forms cover polite conversion, `into`, numeric `what is`,
numeric bare conversion, and `how many … are in …`. Exact tags and unsigned
integer bounds remain mandatory; ambiguous continuations and fractions do not
fall through to unverified answers. Unit and real-socket regressions passed.

Startup now verifies guard rows against sealed contract rows, preserves moved
records during stale cleanup, and removes invalid guards before arming
fail-closed. Sorted membership supports reordered 1,024-row reservoirs without
quadratic replay exhaustion. Byte equality also refuses signed-zero widening.

Private temporary sidecars and manifests prevent predictable-temp symlink
truncation. Successful publication requires file and directory sync. Identical
retries repair prior post-rename sync failures; failure retains the certified
artifact. Tests inject sidecar file/directory, manifest file/directory, and
post-publication sync failures, including trailing-slash roots and retries.
This is Linux fault injection, not physical power-loss or arbitrary concurrent
multi-file checkpoint testing. Existing legacy writer locks remain required.

Fresh-context adversarial reviews directly influenced immutable index keys,
exact row identity, retry sync, trailing-slash handling and temporary descriptor
inheritance. Focused address/undefined-behavior sanitizer testing covers the
startup binding helper; the linked shared library was not sanitizer-instrumented.
The deployment-health command shares the same predicate. Its signed-zero
fixture first reproduced a false health PASS, then passed the strict refusal
test; the complete strict-health suite now checks 48 assertions.

## Evidence and limits

Isolated dirty-source build: `/tmp/cnet-remaining-build-tfUcSB`; this is not a
clean-HEAD release. Full `make verify` passed all 28 fresh-log suites and the
expanded authority gate. Capability certification passed 6/6 on final source,
run `eb3fb3ee-9f23-44a8-b181-a6ba6b3f5dee`; the graded classification and
JSON-adapter metrics were 0.861 and 0.775, respectively. The other four gates
reported 1.000 on their named held-out fixtures, not broader competence.
Knowledge accumulation passed 32-unit isolation, 8/8 replay and 96 OOD refusals;
composition passed three members with coverage enforced at every hop.

Two build-order issues were encountered, not treated as passes: parallel .NET
test builds collided on their shared output, and a lane-runner target left
OpenMP CCE objects in the standard native cache. Running capability tests
serially and rebuilding the standard library resolved these local build states;
the final full regression and capability runs then passed. No certification
floor changed. General mixed-flavor/concurrent build-system remediation was
not part of this product patch.

## Deployment and live acquisition

Verified binaries were atomically installed after preserving current binaries,
configuration, all active base sidecars, and original capsules under
`artifacts/deployments/remaining-sequence-20260906-gNxQLX`. The existing
maintenance/writer locks coordinated the learner; the acquisition timer was
temporarily paused and restored. No base snapshot was restored or overwritten.

cnetd restarted as PID 2895419 and the learner resumed as PID 2895929. Shared
MCP remained PID 2119296; GPU model services were not restarted. All 160 prior
live cases, four new grammar forms and two refusal/clarification cases passed.

One additional live miss, bytes → bits at input 32, queued under the existing
approved tool policy. The real acquisition worker certified one new 32-row
capsule using 32 tool calls, improving its evaluation from 0/32 to 32/32.
All 32 newly covered byte inputs (32–63) then passed on the actual socket:
192 covered cases total, zero wrong certified answers. A fresh CLI process
also imported the published artifact and returned 63 → 504. All five prior
capsules remained byte-identical to backup. No external model supplied these
labels; the evidence came from the approved integer tool.

Final deployed health: one mined unit, one bound coverage record, zero unguarded
or unreadable mined units, coverage gate on, teacher reachable. cnetd, learner,
shared MCP and the acquisition timer were active. This turn exercised the
real acquisition worker directly; an unattended future timer firing was not
observed. The existing scheduled entrypoint was verified in the prior live
acquisition report.

Binary rollback: pause the acquisition timer, wait for any active tick, use
the existing lane-maintenance wrapper/writer lock, stop cnetd, copy the eight
backup binaries through temporary siblings and rename them into `bin`, then
restart cnetd, resume the learner/timer and rerun live/health checks. Preserve
newly learned capsules and the current base; rolling binaries back does not
require rolling knowledge back. Backup `before.sha256`/`after.sha256` record
the executable generations. The backup can also restore unchanged configuration
if needed, but do not restore old base/sidecars over current learning state.

Raw full sweep: `/tmp/cnet-scale-bench-w2FDbp/results.jsonl`.
Raw socket run: `/tmp/cnet-socket-bench-ugHqlv/results.jsonl`.
Recovery logs: `/tmp/cnet-recovery-green.log`,
`/tmp/cnet-remaining-verify.log`, `/tmp/cnet-checkpoint-asan.log`.
Copies of the measured results and final gate/deployment logs are retained in
[`cnet_remaining_sequence_evidence_20260906`](cnet_remaining_sequence_evidence_20260906/),
with `SHA256SUMS`. Temporary working directories remain diagnostic evidence,
not required deployment inputs.

Unknown-domain teacher discovery still requires operator selection of the
domain and trusted evidence sources. No new external service or model CLI was
invoked. Finite approved-tool coverage is not arbitrary autonomous learning;
unmeasured broader capability remains WITHHELD.
