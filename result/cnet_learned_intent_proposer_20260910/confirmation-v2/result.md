# Partial-intent repair — fresh confirmation passed

The NEW frozen grammar-on hybrid passed every original acceptance floor on its
one independently authored, admitted 128-case confirmation. All rows remain in
the denominator. **121/128 exact is not perfect coverage.**

| Measure | Result | Unchanged floor |
|---|---:|---:|
| Correct ready | 75/80 | 72/80 |
| Upper | 38/40 | 34/40 |
| Lower | 37/40 | 34/40 |
| Clarify | 22/24 | 22/24 |
| Abstain | 24/24 | 23/24 |
| Wrong ready | 0 | 0 |

No non-ready output carried an executable dataset/key. The single-use journal
completed normally; no second invocation or post-score repair was performed.
The runner rechecked all 89 evaluation pins and all eight candidate pins after
scoring. Regression qualification before freeze passed **2426/2426**, with
**1506/1506** focused parser tests. See [qualification](qualification.md).
An independent saved-output-only audit reproduced every count, checked all
80 original scalar labels, confirmed zero non-ready executable-field leaks,
and verified the journal, hashes and pins without invoking the candidate/scorer.

## What changed and what was tested

Bounded incomplete operation slots and explicitly uncertain input fields now
use the existing whole-field/operand validators in either order. The new path
can clarify or abstain, never emit an executable proposal. Independent boundary
review found and helped close certain-input ownership and arbitrary-action
operand defects, with executed RED/GREEN tests. Learned weights and training
specification did not change.

Before fresh authoring, the qualified source and DLL were frozen. A fresh-context
author received only the behavioral contract. A separate fresh-context reviewer
independently labeled neutral ID/text rows. The custodian admitted all 128 rows
across 27 families after one pre-score repair of 13 overlapping texts; all labels,
operations, keys and families stayed fixed, and the other 115 rows stayed
byte-for-byte equivalent at the declared row-hash boundary. Full first review
and changed-row second review agreed without ambiguity. Original scalar keys
were independently checked for all 80 ready rows.

Exact and casefold/punctuation/whitespace-normalized overlap was zero against
2,048 prior texts, training/calibration hashes, and the declared development
denylist. This is a defined overlap screen, not proof of semantic novelty or
IID sampling. Role separation used the same model/tools, not OS isolation or a
human/third-party benchmark. A custodian discovery scan exposed evaluator
function signatures and schema/floor lines outside the loader; this exception
is recorded in the admission. Candidate source, tests, scores and old failure
texts were not exposed to author/reviewer.

## Seven retained misses

| ID | Expected | Actual | Request feature |
|---|---|---|---|
| u13 | ready upper/102 | clarify | capital-letter case for a scalar |
| u21 | ready upper/32 | clarify | explicitly described quoted space |
| l01 | ready lower/65 | abstain | input-first possessive lowercase form |
| l13 | ready lower/88 | clarify | small-letter case for a scalar |
| l21 | ready lower/32 | clarify | explicitly described quoted space |
| c04 | clarify | abstain/input_domain | decimal operand, unspecified desired case |
| c12 | clarify | abstain/unsupported_intent | explicit choice between two operands |

These are observed failure categories, not a completed root-cause diagnosis.
In particular, quoted-space coverage was **0/2** in this cohort. The candidate
is not being patched against these now-exposed rows. They may become development
regressions for a separately qualified successor, never fresh evidence again.

## Evidence and identities

- [Candidate freeze and source snapshot](candidate-freeze.json)
- [Independent admission](metadata/admission.json) and [protocol](metadata/corpus-protocol.json)
- [Pinned scoring binding](score-binding.json), [one-shot journal](score-once.jsonl),
  [raw predictions](probe-response.json), and [raw stderr](probe-stderr.txt)
- [Pre-score evidence manifest](metadata/archive-manifest.json)
- [Verified archive mapping: all 31 artifacts](metadata/archive-mapping.json)
- [Final verification](final-verification.json)
- [Exposed development only: old corpus, new DLL](development.json)

Candidate DLL SHA-256:
`98c8a4026f2ab50dfab02c4b18ccf4ea3cae184d747ade0b0562fe2968ff261e`.
Fresh corpus SHA-256:
`10cdd9e0c4a5422fe08a105b6d0cabc72d092a76df1b8b70ab34a13812276e32`.
Score journal SHA-256:
`e61a4e73d03d6eefb98f2484e477e2071e2c147fb423ce8cc27f901797d63f4f`.

The [previous confirmation](../confirmation-v1/result.md) remains failed at
19/24 clarify. Its frozen binary, original-source snapshot, corpus and outputs
are retained. Its current-source paths are superseded by this new candidate;
historical pin verification occurred before those authorized edits.
The complete new corpus, both drafts, blind reviews, dispositions and validation
reports are retained under [evidence/private](evidence/private/confirmation.json);
all 31 copies (981,626 bytes) match their pre-score manifest hashes.

## Scope of the pass

This closes the predeclared **synthetic, grammar-on typed-proposal acceptance**
gate for this candidate and cohort. It does not measure incremental learned
model gain (no grammar ablation), broad language coverage, native capsule
execution, latency/throughput, real-user acceptance or autonomous improvement.
The learned proposer still runs only on unclaimed grammar requests.

All cases are synthetic and training-ineligible. Native actions: zero.
Grammar-off promotion, deployment and broader product acceptance remain
WITHHELD. No service, soak, GPU worker, live checkout or origin/master changed.
The isolated experiment branch remains uncommitted above f78fc27.
