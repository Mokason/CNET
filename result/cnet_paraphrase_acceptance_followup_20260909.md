# Task-language acceptance follow-up — September 9, 2026

Status: eleven first-blind confirmations failed. Acceptance remains WITHHELD;
the latest candidate is regression-green, not acceptance-complete. This is source-only work
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
| [Round 8](task_paraphrases_round8_20260909/confirmation-1.json) | 50/80 | 23/40, 27/40 | 14/24 | 24/24 | 0 | FAIL |
| [Round 9](task_paraphrases_round9_20260909/confirmation-1.json) | 39/80 | 20/40, 19/40 | 16/24 | 24/24 | 0 | FAIL |
| [Round 10](task_paraphrases_round10_20260909/confirmation-1.json) | 48/80 | 24/40, 24/40 | 16/24 | 24/24 | 0 | FAIL |
| [Round 11](task_paraphrases_round11_20260909/confirmation-1.json) | 52/80 | 26/40, 26/40 | 16/24 | 24/24 | 0 | FAIL |

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
round7 was then frozen with zero-overlap checks against 896 prior texts.

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
was blind at candidate freeze, with zero-overlap checks against 1,024 exposed texts and the same floors.

Round8 shares relationship phrases, literal descriptions and declaration labels.
Its quote-domain check rejects explicit multi-character literals without
depending on the surrounding frame. The first review caught source-case
descriptions being interpreted as operations; five new tests ran RED (three
failed). An explicit operation/conversion noun is now required in that frame.
All 551 focused tests pass, with native refusal/positive replay added. Final
candidate regression passed 1,473 tests with zero failures/skips, 99.73% parser
line and 93.26% branch coverage. All 12 independent correction-review probes
passed. Source `0945dc4` was freshly built and pinned in `fbc57dd` before the
first blind score. Round8 failed ready and clarification floors; all rows
remain. Round9 is separately authored with the original floors and zero exact
overlaps against 1,152 exposed texts, retaining the reused-context limitation.

Round9 passed 1,527 managed/native tests, zero failures/skips, 99.75% parser
line and 93.49% branch coverage. All 34 independent implementation probes
passed. Source `d1d0e83` was freshly built and pinned in `3f04d74` before the
first blind score; all nine exposed populations scored 128/128. Fresh round9
nevertheless failed ready and clarification floors. Round10 was separately
authored and blind at freeze against 1,280 exposed texts under the same original floors.

Round10 passed 1,595 managed/native tests, zero failures/skips, 99.77% parser
line and 93.71% branch coverage. Source `d343de3` was freshly built and pinned
in `6bbacaa` before its first blind score. The recipient correction passed 12
independent probes. Its perfect exposed scores again failed to predict fresh
acceptance; ready and clarification floors failed. The next structural slice
shares input bindings and request parsing across two-clause compositions.
No reliable time-to-acceptance follows from the ten failed confirmations.

The contextual structural slice passes 118 independently composed tests (96
component combinations and 22 boundary mutations); 93 initially failed. Its
compatibility run passed 793 task-filter tests, excluding the explicitly
unfinished next-repair test class. It preserves all ten earlier development
populations at 128/128 but moves exposed round10 only to 90/128. This is
component-composition evidence, not fresh acceptance. The next confirmation
remains blind pending completed repair, review and candidate freeze.

After boundary corrections and shared component vocabulary, all 941 task-filter
tests pass (939 parser cases plus two existing task tests). The component
matrix contains 180 independent combinations and 31 boundary cases; another
55 cases cover exposed request-role misses and adversarial boundaries. All
11 exposed populations now score 128/128, totaling 1,408 unique development
texts. Full structural and subsequent development reports are retained under
`task_paraphrases_round11_20260909/`; neither is fresh acceptance. The structural
correction review passed 24 probes; the separate component-inventory review
passed 34 probes with no further issue. At that checkpoint full committed-candidate
regression and the untouched round11 confirmation were still pending.

The freshly built round11 source `dd860d9` passed 1,861 managed/native tests,
zero failures/skips, with 99.80% parser line and 93.09% branch coverage. All
11 exposed collections again score 128/128 on that committed build. The private
native fixture now also replays bound input/produce requests and agreeing alias
questions after capsule activation; controls, literal references and result
formats still refuse before daemon startup. Candidate identities are frozen in
`task_paraphrases_round11_20260909/candidate.json` before the first blind score.

Round11 was first scored after pins commit `9a7a9dd` and failed: 52/80 ready,
26/40 per operation, 16/24 clarify, 24/24 abstain and zero wrong-ready proposals.
The 36 misses span input descriptions, request/declaration relationships and
missing/alternative input or direction. The new shared binding is genuinely
tested component reuse, but did not close fresh-language acceptance. All 12
collections (1,536 unique texts) are now exposed; none is a new holdout. No
reliable completion ETA follows from these results, and another perfect score
on repaired exposed cases would not change that. The remaining language-routing
work must demonstrate generalization under unchanged floors before claiming
completion. No new source, fitting, deployment or live mutation was performed.

The evaluator's 34 integrity tests pass; tracing covers 97% of 213 executable
lines. The unchanged capture/bridge surface passed 148 tests earlier this turn.
Existing test-project CA1416 platform warnings remain; none were suppressed.
Native tests use private installations and pinned external Unicode tables, not
live services or CNET self-answers. Representative µ/A paraphrases succeed
offline after explicit approval and capsule activation, while uncovered `ß`
still abstains and both 256-key table verifications remain intact.

ECC's TDD and verification workflows required executed RED cases and separate
regression/coverage evidence. Graft supplied worktree source discovery when the
MCP graph had no index for this worktree. The current-checkpoint Graft refresh
parsed 2,011 files (12 reparsed, 1,999 cached) into 24,233 nodes, 24,517 edges
and 1,983 cards. Its check reports graph OK, zero changed and zero stale files.
Deep context remains missing; no provider, token-savings or model-quality claim
is made. This freshness check does not turn the failed acceptance gate green.

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
now exposed development data too. Rounds6 and7 used the same separation and
failed their first blind runs. Round8 retains this authorship limitation; its
custodian checked zero exact overlaps against all 1,024 exposed texts.

No claim of learned semantics, broader real-user coverage, allocator gain or
AMD task-selection improvement follows from these grammar tests. Genuine origin
review, measured learning costs, richer workflows, dependency-aware source
replacement, evidence-qualified shadow training and separately approved rollout
remain product work. Existing live ingress, capture policy and frozen 72-hour
soak were not changed or restarted.
