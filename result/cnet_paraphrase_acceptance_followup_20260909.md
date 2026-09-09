# Task-language acceptance follow-up — September 9, 2026

Status: candidate evaluation in progress. Acceptance remains WITHHELD until a
new blind confirmation passes every original gate. This is source-only work
on `feature/verified-task-core-20260908`; no push, migration or live rollout.

## Original floors and all scored confirmations

Every collection contains 128 synthetic cases: 80 ready (40 upper/40 lower),
24 clarify, 24 abstain. Exact ready means the original input byte and requested
operation, not a casing answer. Required: zero wrong-ready proposals, at least
72/80 ready, 34/40 per operation, 22/24 clarify and 23/24 abstain. Nonready
proposals may not contain a dataset/key. No floor, label or denominator changed.

| First scored confirmation | Ready | Upper / lower | Clarify | Abstain | Wrong ready | Result |
|---|---:|---:|---:|---:|---:|---|
| [Original](task_paraphrases_20260909/confirmation-1.json) | 68/80 | 34/40, 34/40 | 21/24 | 23/24 | 0 | FAIL |
| [Follow-up](task_paraphrases_followup_20260909/confirmation-1.json) | 54/80 | 27/40, 27/40 | 23/24 | 22/24 | 0 | FAIL |
| [Round 3](task_paraphrases_round3_20260909/confirmation-1.json) | 39/80 | 20/40, 19/40 | 12/24 | 21/24 | 0 | FAIL |
| [Round 4](task_paraphrases_round4_20260909/confirmation-1.json) | 36/80 | 17/40, 19/40 | 8/24 | 24/24 | 0 | FAIL |
| [Round 5](task_paraphrases_round5_20260909/confirmation-1.json) | 13/80 | 8/40, 5/40 | 10/24 | 21/24 | 0 | FAIL |
| [Round 6](task_paraphrases_round6_20260909/confirmation-1.json) | 33/80 | 15/40, 18/40 | 16/24 | 22/24 | 0 | FAIL |
| [Round 7](task_paraphrases_round7_20260909/confirmation-1.json) | 32/80 | 14/40, 18/40 | 15/24 | 21/24 | 0 | FAIL |

All rows and failed gates remain in the linked reports. Round3 also has a
separate [infrastructure record](task_paraphrases_round3_20260909/infrastructure-attempt-1.json):
the first invocation failed on manifest layout before decoding cases or calling
the parser. Its corrected runner was pinned before the first actual score;
no parser/corpus/binary change or quality result occurred in that failed attempt.

Before round5, the original qualification and four failed confirmations formed
five exposed development populations (640 unique texts). Round5's pre-freeze source scored
128/128 on each, with complete `development-*.json` reports retained under
`task_paraphrases_round5_20260909/`. These are regression checks, not fresh
validation. Prior perfect development scores did not predict new-set success.
Round6's candidate now passes all six exposed collections (768 texts), with
both development and committed-candidate reports retained under
`task_paraphrases_round6_20260909/`. These remain development-only checks.

## Implementation and verification

Test-first changes separate direction lexemes from whole-request frames, then
reuse one scalar/domain boundary. Shared output verbs, preference phrases and
input declarations compose only in explicit grammatical roles. Numeric inputs
retain their codepoint type; decoded controls refuse. Missing inputs, candidate
lists and competing directions never become executable proposals. Unsupported
modifiers and extra actions refuse without native observations.

An adversarial design review rejected a keyword-only fallback: words alone
cannot distinguish applying a function from asking for its input or a different
output format. Those counterexamples became executable refusal tests. Oversized
regex automata were decomposed; the existing nonbacktracking 1000-node guard was
never raised or disabled. Existing capsule coverage, source approval, learning
and certification authority remain downstream and unchanged.

Initial round5 development verification: 310 focused parser tests; 1,232
managed/native tests, zero failures/skips. Initial parser coverage was 99.47%
lines and 94.05% branches. Subsequent boundary review identified case predicates
being misread as conversions. Eight tests reproduced four failures; explicit
verb/adjective and transformation evidence now distinguishes these roles. The
correction review found the same guard missing from a declaration frame; three
more tests reproduced two failures, then a one-line role capture applied the
shared guard there. All 321 focused parser cases pass after these corrections.
Final round5 full-suite evidence: 1,243 tests passed, zero failures/skips, with
parser coverage 99.49% lines and 94.44% branches. An intermediate full run overlapped
a test rebuild and is not used as frozen-candidate evidence. The actual candidate
was freshly built from `2d06468`, with source/binary pins committed in `e6ecc4c`
before the first scored round5 invocation. The final bounded review verified
eight independent predicate/conversion probes. Its passing regressions did not
prevent failure of the new confirmation; all rows are retained unchanged.

Round6 composes complete typed input and operation fields in either order;
separator candidates cannot discard a partial field or an extra action. Its
design review required explicit radix preservation and refusal to reinterpret
quoted codepoint strings. The 90 new tests executed RED (86 failed); all 411
focused tests pass after implementation. A separate bounded implementation
review passed 32 independently invented probes across field consumption,
duplicates, actions, radix, quotes, controls and ambiguity. The native fixture
now includes both field orders and refusal checks before daemon startup.
The committed round6 candidate passed 1,333 managed/native tests with 99.63%
parser line and 93.36% branch coverage. Source `70fc455` was freshly built and
its identities committed in `d34e7f7` before the first blind score. That score
failed; all 128 rows remain. Round6 is now exposed development evidence, and
round7 remains blind with zero-overlap checks against 896 prior texts.

Round7 adds request vocabulary, relative input/operation declarations and explicit
numeric descriptions. Its 63 initial tests ran RED (57 failed); separate review
found decimal-qualifier loss and input/result radix confusion despite passing
focused regressions. Six more tests reproduced four failures. The correction
preserves the declared qualifier and confines hexadecimal suffix interpretation
to explicit numeric input declarations. All 480 focused tests and 18 independent
correction-review probes pass; the private native fixture includes both refusals
and a valid hexadecimal-declaration replay. Final round7 evidence: 1,402 tests
passed, zero failures/skips, 99.69% parser line and 94.26% branch coverage. Source
`c76351d` was freshly built and pinned in `9b0558d` before first blind scoring.
That confirmation failed; retain all rows. Round8 is separately authored and
blind, with zero-overlap checks against 1,024 exposed texts and the same floors.

The evaluator's 30 integrity tests pass; tracing covers 97% of 201 executable
lines. The unchanged capture/bridge surface passed 148 tests earlier this turn.
Existing test-project CA1416 platform warnings remain; none were suppressed.
Native tests use private installations and pinned external Unicode tables, not
live services or CNET self-answers. Representative µ/A paraphrases succeed
offline after explicit approval and capsule activation, while uncovered `ß`
still abstains and both 256-key table verifications remain intact.

ECC's TDD and verification workflows required executed RED cases and separate
regression/coverage evidence. Graft supplied worktree source discovery when the
MCP graph had no index for this worktree. Final Graft freshness check is pending.

## Evidence limits and unchanged product scope

These are procedurally separated same-Astra synthetic evaluations, not human,
external-model or IID population tests. Iterative repair and stopping after a
passing confirmation do not establish an unbiased general-language success
rate. Exposed sets are never relabeled as fresh evidence or pooled to erase
failures. All examples remain training-ineligible.

Fresh-agent capacity was exhausted before round5. Its isolated author and
custodian contexts were reused, with knowledge of their own prior authored sets
but no CNET code, tests, graph, results or execution access. They independently
checked all 128 statuses and 80 original-byte operation labels, with zero exact
overlaps against 640 prior texts. This limitation is recorded in the immutable
manifest. Root remained blind until candidate source/binary freeze. Round5 is
now exposed development data too. Round6 remains separately authored and blind;
its custodian checks zero exact overlaps against all 768 exposed texts.

No claim of learned semantics, broader real-user coverage, allocator gain or
AMD task-selection improvement follows from these grammar tests. Genuine origin
review, measured learning costs, richer workflows, dependency-aware source
replacement, evidence-qualified shadow training and separately approved rollout
remain product work. Existing live ingress, capture policy and frozen 72-hour
soak were not changed or restarted.
