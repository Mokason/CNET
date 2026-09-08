# Verified task core — first milestone, September 8, 2026

Implemented on `feature/verified-task-core-20260908` in
`/home/marble/AI/CNET-worktrees/verified-task-core-20260908`, based on `1d45b93`.
The dirty primary checkout, existing live owner-DM deployment, and frozen
72-hour installation were not changed. No push or live rollout was performed.

## What now works

`learning task` proposes a typed Unicode case operation from bounded English;
`learning observe` is its numeric equivalent. Each accepted request ID first
receives a durable pending observation, then at most one native attempt.
Replay never reissues pending/unknown calls. Observation creates no learning
demand. `learning inbox` reads retained chronological observations without
writing the database or renewing the original heartbeat.

`learning approve` checks independently imported source bytes against the
explicit owner hash, records the external expected value separately from the
native observation, pins the source for later scheduling, and admits demand
exactly once in the same transaction. Native answers are never training truth.
Actual source drift/corruption or contradictory native values pause/refuse;
wrong operator hash alone refuses without pausing. Historical review conflicts
remain non-approvable after resume. Old schema versions refuse without reset.

The [operator contract](../docs/VERIFIED_TASK_CORE.md) documents commands,
status/exit semantics, fresh-only schema 3, boundaries and recovery. The
[continuation plan](../plans/cnet_verified_task_core_20260908.md) separates this
milestone from the remaining five-part roadmap.

## Native everyday-workflow proof

The private test used freshly built native artifacts, the pinned Unicode 17
source, actual capsule acquisition and probation, and teacher/self-answer paths
disabled. All requests were explicitly synthetic, not Discord or allocator
usage evidence. Actual retained test receipts show:

| Step | Observed result |
| --- | --- |
| Numeric ambiguity and four malformed punctuation requests | Clarification, no experience or native attempt |
| Unrelated weather request | Proposal abstention, no experience |
| “What's the uppercase of µ?” before source approval | `awaiting_evidence`, no native value, idle learner |
| Lowercase correction approved first | External expected 97; native capsule accepted; 56 answers + 200 abstentions verified |
| Uppercase correction approved next | External expected 924; native capsule accepted; 58 answers + 198 abstentions verified |
| “Please convert 'µ' to uppercase.” after learning | Independently verified native 924 / U+039C |
| “What is the lowercase of A?” afterward | Independently verified native 97; existing capability preserved |
| “uppercase ß” | Native abstention, no substituted external/default answer |
| Final complete sweeps | Both 256-key domains pass: 114 correct answers and 398 correct abstentions total |

Exactly two native jobs were created and accepted. The E2E test took 11.68 s
in the retained acceptance run; that includes build/activation/probation waits
and is not a request-latency or model-throughput benchmark.

Independent source identities:

- Raw 256-row Unicode excerpt: `75dfecc13fe9b1202e3f7c787e4e7f2c848c97c8b092dd75b4f6a2b99990cdc4`.
- Upper table: `d5a314a48db2e2e86712bf012c20ac40fd1ef68b6120a5cdbf6c5a6aa93916e3`.
- Lower table: `54d8c8aa294fa803fb49d0f5ee2cb46357d09950b664163d70f43c2dfaf8d3b7`.

The existing Unicode workload tests independently extract the explicit fields
from the pinned raw source and compare the canonical tables, rather than using
the runtime's Unicode casing implementation as an oracle.

## Verification and limitations

| Check | Measured result |
| --- | --- |
| Fresh six-artifact native build | Passed |
| Final full control-plane suite | 956 passed, 0 failed/skipped; 1 minute 24 seconds |
| Full capture/bridge suite with private native evidence runtime | 127 passed, 0 failed/skipped |
| Independently compiled native mapping fixture | 512 key/rule pairs and four negative cases passed |
| New task production files, measured line coverage | 232/232 executable source lines covered across five files |
| Production NuGet direct/transitive advisory check | No vulnerable packages reported by the configured official NuGet source |
| ECC repository IOC scan | No findings, but only one matching file examined; not a C/.NET security audit |
| Diff whitespace check | Passed |

Coverage used the already locked Microsoft collector and its documented
[Cobertura invocation](https://github.com/microsoft/codecoverage#get-started).
The measured files are `LearningLedger.Experience.cs` (86/86),
`LearningLedger.ExperienceApproval.cs` (72/72), `LearningObservation.cs` (17/17),
`LearningTaskCommand.cs` (11/11) and `LearningTaskProposal.cs` (46/46).
This is source-line coverage, not branch completeness or correctness proof.
The existing CLI wrapper executes in isolated child processes and is not
represented by this five-file coverage number; its real commands are exercised
by the integration suite and retained receipts. No test was excluded to improve
the reported percentage. Existing CA1416 platform warnings remain.

Tests include concurrent duplicate admission, cross-boot uncertainty, original
run expiry/stale heartbeat/stop refusal, read-only inbox byte identity, wrong
hash, absent/excluding evidence, source corruption/replacement/removal,
approval idempotence and 4,096-record capacity without pending-ID eviction.
The capacity fixture is bulk synthetic setup, not 4,096 native executions.
Admission's 4 MiB + 64 KiB watermark is not allocated/protected storage or a
proof of worst-case completion growth under concurrent workroot writes.

The advisory and IOC results are scoped checks, not absence-of-vulnerability
claims. No new production dependency, cloud provider, shell action, remote
approval endpoint or GPU training run was introduced. Trusted OS/framework,
malicious same-UID modification, temporal source truth and new learned-policy
quality are outside this result.

## ECC and Graft trial

ECC 2.2.1 consultation and install-plan preview ran read-only. Consultation
found useful workflow/security categories but also irrelevant framework hits.
Selecting three skills expanded into broad home-level modules; that plan was
not applied. Existing Codex configuration was preserved. The installed ECC
TDD and verification-loop instructions were applied directly to CNET's native,
.NET and Python runners, not their unrelated npm example commands.

Executed RED checkpoints include missing observation/schema support (2 fails),
missing task command (1 fail), source/run/storage review cases, and four
ambiguous punctuation inputs. The first two feature REDs are retained in
`325a214` and `317051b`; observation implementation is `38d1e33` and the completed
task route is `062337c`. Each genuine failure was followed by a passing scoped
test run. Fresh Astra adversarial
reviews found transaction rollback, post-refusal demand mutation, missing
source-drift pause, symbolic-input misclassification and punctuation ambiguity;
those concrete findings received regression tests and fixes. No other model
provider was called.

Graft 0.16.0 built the worktree graph, supplied exact source spans and callers,
and was used for change-impact discovery. The final source graph contains
1,983 indexed files, 24,000 nodes, 24,018 edges and 1,955 map cards. CLI discovery
was used because this active session did not expose Graft's registered MCP
tools. Cached queries refreshed changed files; graph freshness was checked
again at handoff. No deep/model pass was requested. Graph counts and Graft's
estimated token savings are not correctness or measured performance evidence.

## Evidence locations and initial setup failures

The acceptance TRX, including synthetic native JSON receipts, and Cobertura
report are under `.artifacts/task-verified/acceptance/` in this worktree.
Logs are `/tmp/cnet-task-acceptance-20260908.log`,
`/tmp/cnet-task-discord-final-20260908.log`,
`/tmp/cnet-task-native-mapping-20260908.log`,
`/tmp/cnet-task-dependency-audit-20260908.log` and
`/tmp/cnet-task-ecc-ioc-20260908.json`. These are local evidence locations, not
durable remote archival guarantees.

The first storage fixture failed because its filler file was not private; that
setup error was corrected before executing the real admission-limit RED.
The first full `/tmp`-artifact run could not locate the repository in three
older tests; repository-local isolated artifacts fixed the test layout.
An initial Python run used the shell's unrelated Hermes environment, which
lacked websocket-client, and skipped the explicit native fixture. A disposable
Python 3.12 environment with the already documented websocket-client 1.9.0 plus
freshly compiled private native evidence artifacts produced the final 127/127
run. No live Python environment was changed. A first parser implementation
misparsed the optional “to”; the failing paraphrase tests drove its correction.

## Remaining scope — not marked complete

This demonstrates a finite source-approved correction and local capsule reuse
under a different grammar form. It does not establish independently held-out
semantic-model generalization or close the zero-eligible-experience blocker.
Still needed: authentic reviewed capture linkage, grouped inbox gaps/costs,
useful reusable compositions, dependency-aware freshness/source arbitration,
and AMD-trained task policies that improve against frozen controls in shadow
evaluation before promotion. Schema migration and owner-DM deployment of this
new route are separate, unperformed steps.

The existing frozen 72-hour terminal result, genuine whole-window demand,
native allocator trajectories, >=.05 useful gain, positive paired 95% lower
bound and nonnegative family means remain required. No origin attestation or
genuine episode was invented; allocator remains disabled and broader product
acceptance remains WITHHELD.
