# Herdr cross-model language experiment — September 9, 2026

## Latest checkpoint

The user explicitly requested publication to `origin/master` after the verb
repair and follow-up refusal fix. This authorizes publishing the reviewed
checkpoint, not live deployment, new model calls, source approvals or training.
Historical no-push statements below describe the authority at those checkpoints.

Third confirmation against `8cefc80` failed once: 112/128 exact, 68/80 ready
(35 upper, 33 lower), 20/24 clarify, 24/24 abstain, zero wrong ready. All rows
are now exposed. Fourth-draft authoring stopped at its retry bound with 96/128
unadmitted rows and no score. See the dated result bundle for all receipts.

The user implemented the pending verb-composition repair and added refusal for
`Raise 'a' after converting it to lowercase.` after coordinator review exposed
the dropped leading operation. The coordinator independently confirmed 72/72
verb tests and the corrected refusal. Push-time full-suite evidence is retained
with source/binary hashes. Fresh acceptance and learned intent remain WITHHELD;
prior benchmark results do not describe the final verb candidate.

## Authority and baseline

The user approved Hermes with its existing Grok 4.6 backend as the third seat,
alongside Claude Code and GPT-6-Astra. This is a bounded source-only experiment,
not permission to change live services, the frozen soak, source approvals,
training eligibility, GPU fitting, or to push/merge automatically.

Base: `5041f00e39bbaf903eca83f86c296b40cf084f79`, on origin/master at start.
Working branch: `experiment/herdr-language-20260909` in its own worktree.
All twelve old corpus collections are exposed development data. Eleven first
confirmations failed. The last scored ready/clarify/abstain counts were 52/80,
16/24, 24/24, with zero wrong ready; 1,861 managed/native regressions passed.
The original acceptance floors and all historical failures remain unchanged.

## Roles and first-pass limits

| Seat | First artifact | Access | Limit |
|---|---|---|---|
| GPT-6-Astra | One falsifiable structural design and small implementation slice | Read-only source worktree; no new holdout | 900 seconds |
| Claude Code, Opus alias | Adversarial architecture/safety review | Supplied parser and contract only; no tools or MCP | 600 seconds; CLI API-budget ceiling USD 3 |
| Hermes / Grok 4.6 / xai-oauth | New contract-only confirmation draft | No code, graph, old scores, memory, or execution/file tools; clarification tool only | 540-second run budget, 600-second outer limit, two tool iterations |

Provider login does not prove sufficient quota or a successful response. Model
identity, process completion and actual artifacts must be checked. No API key
is copied into a packet or terminal command. Existing account authentication is
used. A spending ceiling is not a statement of actual cost or subscription terms.

The first Astra turn is design-only. Implementation starts after reconciling
independent findings, with an executed RED test and a <=5-file slice. Do not
call three agreeing model opinions proof, repeat blind evaluations until lucky,
or use current synthetic acceptance rows as training truth. A fresh corpus draft
is not admitted as confirmation until schema/quota/overlap validation and a
separate contract-only label review are complete. Implementer and design reviewer
must not read its texts before candidate freeze. Semantic proposal schema checks
alone cannot prove that an otherwise valid operation matches user intent.

## Concrete sequence and checkpoints

1. Launch all three bounded runs under a dedicated Herdr session. Verify actual
   provider execution, complete output and exit status; retain failures.
2. Reconcile the Astra design and Claude findings into one concrete slice with
   RED tests, refusal boundaries and explicit disproof criteria. Do not silently
   authorize new weights, sources or fitting if the recommendation needs them.
3. Validate and independently review the Hermes draft without exposing it to
   implementation. Freeze bytes, labels, origin, quotas and hash identities.
4. Execute the small source-only implementation and all relevant regressions;
   review its changed boundary independently. Stop/escalate after three unresolved
   substantive review cycles for the same artifact.
5. Freeze candidate source and binaries, score confirmation once under all
   original floors, retain every row, and report success or failure honestly.

## Session and local artifacts

Attach: `herdr --session cnet-three-heads-20260909`.
Workspaces: `CNET-Astra` (`w1:p1`), `CNET-Claude` (`w2:p1`), and
`CNET-Hermes-Grok` (`w3:p1`). IDs are those returned by this session, not portable
identifiers. The default Herdr session and existing CNET worktrees are untouched.

Packets, bounded shell launchers and responses are initially under
`/tmp/cnet-herdr-three-heads-PK7L3c/`. These are local temporary evidence, not
remote archival guarantees. Only reviewed nonsecret artifacts should later be
copied into dated repository results. Never expose the evaluator response while
collecting implementation logs. Runs use redirected outputs; Herdr screen-based
idle detection is not completion evidence. Verify the process and output status.

Installed tools at preflight: Herdr 0.7.5, Hermes 0.21.1, Claude Code 2.1.261,
Codex CLI 0.153.4. Exact Astra model selected: `gpt-6-astra`, xhigh; Hermes
provider/model explicitly selected: `xai-oauth` / `grok-4.6`. Claude uses the
installed supported `opus` alias; record the resolved model from its response.

Planning and git-workflow skills require ordered ownership and an isolated
branch. The adversarial-review skill requires issues-first independent review
and bounded correction cycles. Their checks do not themselves establish CNET
language acceptance. Local source/CLI inspection confirmed launch options;
Herdr automation documentation supplies the pane/process distinction:
https://herdr.dev/docs/agent-automation/ .

## First infrastructure result

Hermes attempt 1 failed before producing any corpus: its shared Responses
stream watchdog closed the connection after 12 seconds without SSE events,
then exhausted three retries. Session metadata records model `grok-4.6`, zero
tool calls and no returned corpus. This is not a failed CNET quality score.
The error's `Codex stream` wording comes from Hermes' shared transport path,
not evidence that the explicitly requested Grok model was replaced.

Retain `evaluator/response.txt` and `stderr.log`. A single bounded retry keeps
the same prompt, backend and tool restrictions, with invocation-local
`HERMES_CODEX_EVENT_STALE_TIMEOUT_SECONDS=90` and
`HERMES_API_CALL_STALE_TIMEOUT=300`. The 540-second run budget and 600-second
outer ceiling remain. No global Hermes config or credentials were changed.
Its output uses separate `response-retry2.txt` and `stderr-retry2.log` files.

Retry 2 failed at the 90-second event-idle threshold, again without a corpus.
Astra and Claude completed their independent designs. Root reproduced 15
specific diagnostic cases and reconciled actionable findings versus contract
misreads before starting Astra's 1,200-second, five-file implementation slice.
See [the retained design checkpoint](../result/cnet_herdr_three_heads_20260909/reconciliation.md).
No fresh acceptance set is admitted; no backend was silently substituted.

## Execution boundary and recovery

A minimal same-backend health request subsequently returned the exact
`HERMES_GROK_HEALTH_OK` marker. This rules out a complete connectivity failure
at that time; it does not prove long-request reliability. The next bounded
authoring attempt requests four independent 32-case batches, still totaling
80 ready (40 each operation), 24 clarify and 24 abstain. Each invocation uses
the same `xai-oauth` / `grok-4.6`, no execution/file tools, a 150-second run
budget, a 180-second outer limit and a 120-second stream-idle guard. No partial
batch is scored or substituted for the full acceptance denominator. Original
raw attempts and batch outputs remain separate, and no global settings changed.

Astra's implementation attempt finished **blocked before implementation**.
Nested CLI file-write attempts failed with `bwrap: loopback: Failed RTM_NEWADDR:
Operation not permitted`; the attempted no-restore test also reported
`NETSDK1004` for missing private build assets. No production or test source was
changed, no assertion executed, and `TASK_CONSTITUENT_RED` was not obtained.
Infrastructure failure is not RED. See the retained implementation handoff.

The next source step is coordinator-brokered patch application and test execution
from the outer workspace, starting with dependency restore and actual failing
tests. Do not disable the nested agent sandbox or call this a parser improvement
already delivered. All three model outputs can inform that step; model agreement
does not substitute for tests or a separately reviewed confirmation population.

## Completed first-pass checkpoint

All four Grok batches returned parseable JSON with 32 cases each. The concatenated
draft has 128 rows, 80 ready (40 upper, 40 lower), 24 clarify and 24 abstain. Its
SHA-256 is `720e1d8bdd6c6e0e7f5175cbd689c67fdb565f06aacc7f6d2a3baaabcc3b358d`.
Admission failed: `h4_11` duplicates `h2_17`; four other rows exactly overlap
previously exposed corpora. No rows were removed or relabeled. The complete
draft, four raw parts and failed large-response attempts remain private under
the temporary run directory. Repository results retain metadata, not holdout
texts. [The metadata receipt](../result/cnet_herdr_three_heads_20260909/hermes-draft-metadata.json)
lists affected IDs without exposing their texts.

The draft was never run against CNET and has not had independent label review.
Its structural rejection is not a twelfth failed language-quality confirmation.
Existing eleven scored failures and all floors remain unchanged. The next draft
must pass full schema, quotas, uniqueness, prior-overlap and contract-only label
review before candidate freeze and a single confirmation run.

All bounded CLI jobs have exited; the dedicated Herdr server and three panes
remain available for inspection. There is no autonomous coordinator continuing
source edits or model requests in the background. Astra's failed private build
outputs were moved to `astra-failed-build/` in the temporary run directory, not
deleted or staged. CNET production/test source remains unchanged from `5041f00`.

Fresh checkpoint checks: `make asi_framing`,
`bash tests/test_execution_tiers_doc.sh`, and `git diff --check` passed. These are
documentation checks, not new managed/native regression runs. The 1,861 passing
tests belong to the unchanged prior candidate; no new implementation has been
qualified. No commit, push, merge, training, deployment or live service mutation
was performed in this first pass.

## Implemented continuation: first constituent candidate

The outer workspace subsequently executed real assertion RED, implemented the
bounded constituent parser and committed its four source/test files as
`29a4091b9a2d52bd06f109f7a588adf11a470a2c`. Shared operand validation and native
coverage remain authoritative. Three bounded independent review cycles exposed
small/letter ambiguity and compatibility-role bugs; all recorded examples have
passing regressions. The third-cycle fixes received local regression verification,
not a fourth independent clean review. One newly authored development expectation
(`Please write the uppercase of France.`) was corrected from abstain to clarify
after contract review; no frozen corpus label was changed.

First candidate qualification: 2,210 managed/native integration tests passed,
34 evaluator integrity tests passed, all four native capsule gates passed, and
locked transitive NuGet audit reported no advisory warnings. An initial evaluator
run failed because its expected private artifact paths were absent; building
those same-worktree fixtures resolved it. No old-checkout binaries were substituted.
The first candidate's measured warm-parser benchmark and native gate receipts are
retained separately under `result/cnet_herdr_three_heads_20260909/benchmarks-first/`.
These are scoped checks, not a comprehensive vulnerability audit or GPU result.

The original Hermes draft was repaired and independently labeled before scoring.
Frozen population SHA-256:
`f47c14e717f414984fcbb1f472ddbf39460b5ef301a805f902af91bb74b59a57`.
Its one confirmation execution failed: 61/80 ready (33/40 upper, 28/40 lower),
16/24 clarify, 23/24 abstain; 100/128 exact and zero wrong ready. This is the
twelfth scored first-blind failure, distinct from pre-admission draft rejections.
All 128 rows are now exposed development evidence. Of the prior 12 development
collections, 11 passed exactly; round11 stayed 92/128. Different populations must
not be presented as a paired accuracy improvement.

## Second component extension, in progress

The next distinct slice targets shared operation vocabulary, descriptions attached
to explicit operands, and unresolved input/direction roles. Its first 37 assertions
executed with `TASK_VOCABULARY_RED`: 30 failed, seven passed. The first implementation
hit the regex engine's 1,000-state ceiling; operation and noun automata were split
instead of increasing the limit. All 37 then passed. Further executed boundary
tests require an unresolved selection to clarify even with one listed candidate.
Named descriptors constrain an explicit byte and never generate a byte from a name.

A separate custodian is authoring another 128-row population through the same
Grok backend, with blind Claude label review, no source/score/failure access and
the original admission and scoring floors. Provider timeouts and retries remain
evidence, not CNET scores. No candidate will see that population before source and
binary freeze. This is not authorization to repeat an exposed confirmation,
change live deployment, train, push or merge. Acceptance remains WITHHELD until
a newly admitted, single-run confirmation passes every original floor.

Second candidate source was committed as
`61d92cf078ff89443883d39fa13eb6427c51fd87` and frozen with its tested binary at
12:56:11 UTC. Qualification passed 2,263 regressions, including actual private
native policy/evidence guard checks for the new verbs, and 34 evaluator tests.
Independent cycle 1 found a bare-A/a compatibility regression and an ambiguous
second-input substitution. Both received actual RED/green repair. Cycle 2 found
no remaining required issue in its scoped source review and 44 read-only probes.
No production source may change between this freeze and confirmation.

The final development replay retained all 1,664 exposed cases: 12 collections
passed exactly and round11 remained 94/128, with zero wrong ready throughout.
This is development evidence only. The original qualification gate still passes.
Three final alternating warm-parser trials measured vocabulary p50/p95/p99
medians of 11.621/37.561/48.471 microseconds and 6,406.955 allocated bytes/call.
Baseline timing was noisy; no speedup is established. The four unchanged native
executables passed again (12/74/94/55 checks). Their direct execution timing is
not comparable to the first run's build-plus-gate timing.

The second admitted confirmation (`102f76da…b669b`) was scored once against
that frozen candidate and failed only clarification: 73/80 ready (36/40 upper,
37/40 lower), 19/24 clarify, 24/24 abstain, zero wrong ready; 116/128 exact.
The final blind label review withheld family/status/operation/key for all 128
and matched every label. Both provider-review cycles and all critiques are retained.
This is the thirteenth scored first-blind failure; its 128 rows are now exposed.

The next distinct slot extension targets explicit trailing operands, incomplete
quoted slots, unresolved choices and missing direction. Initial 32 assertions
executed 20 failures before production edits. Directional-constraint and scalar-
description role checks also executed RED before their repairs. An independently
authored third population is prepared without old examples or scores; the overlap
set now contains 14 collections/1,792 texts. No floors, label authority, provider
permissions or live scope change. Passing ready/abstention alone is not completion.

Slot source `8cefc80793116426df0d6bb780dfa36d43830194` was committed and frozen
at 13:32:23 UTC after 2,314 passing regressions and 34 evaluator tests. Review
cycles identified byte-spelling ambiguity and quote-character compatibility;
both executed RED before repair. The final third scoped review was clean, with
40 final read-only probes. Graft's refreshed non-LLM wiring graph passed freshness
with 24,283 nodes; this is not semantic-layer completion. The original two frozen
Hermes populations now pass as development data; earlier round11 remains 94/128.
Third-population blind label review and final benchmark receipt remain separate
from this completed source qualification.

Final slot benchmark trials completed against unchanged frozen binaries: median
p50/p95/p99 11.782/37.300/46.277 µs, throughput 64,927.62 calls/s and allocation
6,417.06 bytes/call. No clean speedup is established against the variable baseline;
allocation is 141.82% higher. The four native gates passed again with unchanged
12/74/94/55 checks. First and second benchmark bundles remain intact. The final
development replay retained all 1,792 rows with zero wrong ready; 13 collections
passed exactly and round11 remained 94/128.
