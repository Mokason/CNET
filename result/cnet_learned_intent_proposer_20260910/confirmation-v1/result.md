# Frozen learned-proposer confirmation v1

## Outcome: acceptance FAILED on clarification

The single allowed invocation completed against the unchanged frozen grammar-on
hybrid. Exact typed proposals/statuses: **123/128**. The clarification floor
failed; every other original numerical floor passed. This is a failed fresh
confirmation, not a successful acceptance result rounded from aggregate accuracy.

| Gate | Measured | Unchanged requirement | Result |
|---|---:|---:|---|
| Wrong ready | 0 | 0 | Pass |
| Ready operation and original input key | 80/80 | >=72/80 | Pass |
| Upper | 40/40 | >=34/40 | Pass |
| Lower | 40/40 | >=34/40 | Pass |
| Clarify | 19/24 | >=22/24 | **Fail** |
| Abstain | 24/24 | >=23/24 | Pass |
| Executable fields on non-ready outputs | 0 | 0 | Pass |

All 128 cases remain in the denominators. No labels changed after scoring.
Every ready prediction preserved the operation and ORIGINAL input scalar.
This evaluates typed proposals, not converted answers or native execution.

## All five misses

Every miss expected `clarify` but returned `abstain / unsupported_intent`, with
null Dataset, Key and Prompt. None produced an executable proposal.

| ID | Family | Original request |
|---|---|---|
| c085 | missing_input | `Apply uppercase conversion to:` |
| c090 | missing_direction | `Adjust the letter case for 'Z'.` |
| c091 | multiple_candidates | `Please uppercase one character; the input could be either "a" or "b".` |
| c093 | multiple_candidates | `Convert to uppercase; candidate input: 'é' or 'ø'.` |
| c094 | multiple_candidates | `Convert to lowercase; input possibilities: U+004D or U+004E.` |

The observed remaining gap is recognition of incomplete/ambiguous case requests:
one missing operand, one missing direction and three alternative-input requests.
This classification is based on saved requests and predictions, not a second
candidate invocation or runtime branch tracing.

## Independence and admission

The candidate was frozen before authoring. A fresh-context author received only
the behavioral contract. A separate fresh-context reviewer independently labeled
128 opaque ID/text records, without author labels, operation, key or family.
The custodian requested one bounded pre-score overlap repair affecting 26 texts;
labels, keys and families stayed fixed, and 102 unchanged rows were hash-checked.
The changed 26 texts received a second blind review. All final labels agreed,
with zero unresolved uncertainty, across 27 families.

Final exact/normalized overlap was zero against 15 exposed corpora (1,920 texts),
359 training examples, 52 calibration examples and 11 exposed proposer-test
request literals. Normalization was Unicode casefold with punctuation and
whitespace removed. This is not an exhaustive regression-template or semantic
overlap test. No earlier corpus or candidate outputs were supplied to the author
or reviewer. All data remains synthetic and training-ineligible.

Fresh-context isolation was procedural on the same model/tools, not OS-enforced
isolation, independent organizations, different-model confirmation, a human
benchmark, IID sampling or proof of real-user quality. Earlier failed and
exhausted attempts remain intact; this result is not pooled with them.

## Frozen evidence

- [Candidate freeze](../candidate-freeze.json): unchanged seven pins.
- [Protocol](protocol.md), [admission](admission.json), and
  [harness freeze](harness-freeze.json) preceded candidate execution.
- [Score binding](score-binding.json): 48 pinned inputs/artifacts, including
  the absolute .NET host, probe, evaluator, candidate, corpus and review evidence.
- [One-shot journal](score-once.jsonl): exactly `started`, then `complete`;
  the result is a quality failure, not a harness failure.
- [Raw proposals](probe-response.json), with empty [stderr](probe-stderr.txt).
- [Complete frozen corpus](evidence/private/confirmation.json), now exposed.
- [Archive mapping](evidence/archive-mapping.json): all 28 corpus, draft, review,
  validation, provenance and denylist artifacts copied byte-for-byte and verified.
- [Harness verification](harness-verification.md): existing 34/34 integrity tests
  and five one-shot guard tests; independent review and executed RED/GREEN fixes.

Corpus SHA-256: `806c0c2d38eeda7422f0cc03e36e50ff5698687513df4b50a90d4abe8c5b5f53`.
Frozen candidate DLL: `92db20a37772db1a2e6ae6b35c6e0b2c31f151246caa2c9e960fee6ad2653d96`.
Admission SHA-256: `07eb02dfa7588f7b437e4e203963e0be3c89cadd3037fb9052219caac80fa811`.

An independent post-score audit used saved JSON only. It verified every row,
aggregate and family count, every gate, null non-ready executable fields,
binding/raw-output hashes, all 48 scoring pins and all seven candidate pins.
It made no evaluator or candidate invocation and found no discrepancy.

## What stays withheld; concrete next repair

Fresh acceptance, grammar-off qualification, incremental benefit attributable
to the learned fallback, live deployment and training eligibility remain
**WITHHELD**. No grammar-only or grammar-off ablation was run. The 80/80 figure
does not establish that the learned fallback was responsible for those successes.
No performance benchmark or GPU utilization claim is made by this evaluation.

The next implementation slice should recognize partial case intent as a typed,
non-executable clarification state: missing operation, missing operand, or
multiple operand candidates. Input-description continuations must be separated
from additional actions; a semicolon alone must not decide their meaning.
Keep unrelated actions, unsupported qualifiers and forbidden-domain inputs as
terminal abstentions. Test structural variations and safety counterexamples,
not only these five now-exposed strings. Then qualify and freeze a NEW candidate
and obtain a NEW independently authored confirmation before claiming acceptance.
Do not adapt or rescore this frozen candidate/corpus as fresh evidence.

The candidate remains uncommitted above `f78fc27` on
`experiment/herdr-language-20260909`. This sequence changed only evaluation
orchestration, evidence and task documentation. No candidate source, frozen DLL,
primary worktree, live service, soak, GPU campaign, commit, push or deployment
was changed.
