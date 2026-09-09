# Herdr language continuation — retained evidence

Source-only experiment on `experiment/herdr-language-20260909`, based on
`5041f00`. The user subsequently authorized publication to `origin/master`.
No deployment, live learning, source approval, GPU fitting or soak change is
part of this checkpoint. ASI means Artificial Specialized Intelligence.

## Implemented candidates and verification

| Candidate | Change | Verification before freeze |
| --- | --- | --- |
| `29a4091` | Bounded constituent parsing with shared scalar/coverage boundaries | 2,210 regressions; 34 evaluator tests; three bounded independent review cycles; all recorded findings fixed, final fix locally verified |
| `61d92cf` | Shared case vocabulary, explicit named-input constraints, unresolved-role handling, separate operation/noun automata | 2,263 regressions; 34 evaluator tests; second independent review cycle clean, including 44 read-only probes |
| `8cefc80` | Explicit trailing input slots, unresolved choice/missing-direction handling, preserved scalar/byte representation roles | 2,314 regressions; 34 evaluator tests; final third review cycle clean, including 40 read-only probes |
| `59b8923` | Verb inflections, bound-request ownership, clarification forms and refusal of additional transformations | Fresh locked push-time build: 2,386 regressions; 72 verb tests also checked independently; no new holdout score or latency benchmark |

Production changes followed actual assertion RED. The vocabulary slice's first
37 tests had 30 failures. A regex automaton-size failure was resolved by separating
operation and noun recognition, without increasing the engine's limit. A legacy
bare-A/a regression and an independently found second-input substitution were
reproduced and repaired. All raw regression logs are under `verification/`.
An initial unavailable `XPlat Code Coverage` collector was not counted as coverage;
the final run used the installed `Code Coverage;Format=cobertura` collector.
The main constituent class has 98.80% line / 93.11% branch coverage; the main
legacy parser class 99.62% / 93.03%. These exclude separately reported generated
record/lambda classes and do not establish semantic completeness.

Local locked transitive NuGet auditing returned no advisory warnings. This can
use cached advisory data and is not an exhaustive vulnerability assessment.
The boundary review exercised original input preservation, malformed/ambiguous
requests, decoded controls, out-of-domain operands and extra-action refusal.
Native policy, source evidence, demand and job guards remained tested. Existing
native build warnings are preserved, not silently declared fixed.

Graft's isolated non-LLM wiring graph was refreshed for the slot candidate and
its freshness check passed with 24,283 nodes. This predates the verb fix; its
semantic/deep layer was not built. The MCP graph did not
contain this worktree, so source discovery used the documented Graft/source
fallback. No primary worktree or global agent configuration was changed. Estimated
tool token savings are not treated as measured performance.

## Language acceptance history

The first admitted Hermes corpus, hash
`f47c14e717f414984fcbb1f472ddbf39460b5ef301a805f902af91bb74b59a57`, was scored
once against `29a4091` and **failed**: 100/128 exact, 61/80 ready (33/40 upper,
28/40 lower), 16/24 clarify, 23/24 abstain and zero wrong-ready proposals.
See `confirmation-score.jsonl`, `candidate-freeze.json` and
`corpus-continuation/admission.json`. Its structural draft repairs and blind
label-review dispositions were completed before that score. No scored row was
removed or relabeled. This is the twelfth first-blind quality failure; provider
timeouts and pre-admission draft rejections are not additional quality scores.

That corpus is now exposed development data, retained in
`exposed-corpora/hermes-first.json`. The vocabulary development run repaired it
to 128/128; this is not fresh acceptance. Earlier round11 remains a known
coverage gap (94/128 in that development run, zero wrong ready). All other
12 exposed collections passed exactly. No pooled accuracy or cross-population
comparison is claimed as independent improvement evidence.

The second Hermes confirmation was independently authored and blindly labeled
without parser, test, score or graph access. Its final review received all 128
opaque IDs/texts with author family/status/operation/key withheld. All labels
matched and every critique was reconciled before admission. Candidate source,
binary, unchanged scorer and probe were frozen before its texts were revealed.

That one confirmation also **failed**, solely on clarification: 116/128 exact,
73/80 ready (36/40 upper, 37/40 lower), 19/24 clarify, 24/24 abstain, zero wrong
ready. The original clarification floor is 22/24; it was not lowered. This is
the thirteenth first-blind quality failure. See `vocabulary-freeze.json`,
`vocabulary-confirmation-score.jsonl` and `corpus-continuation2/admission.json`.
Hash: `102f76da2de5ed7a93b6c97730736e95d254bf9fffd76a894483cf7c440b669b`.
It is now exposed and retained as `exposed-corpora/hermes-second.json`.

The slot candidate `8cefc80` was scored once against the third admitted corpus,
hash `7b71796aa4d40e61439668af3ad5a100c0aa2cdb2667e5d3cc239810cb063e0c`.
It **failed**: 112/128 exact, 68/80 ready (35/40 upper, 33/40 lower), 20/24
clarify, 24/24 abstain and zero wrong ready. This is the fourteenth first-blind
quality failure. See `clarification-freeze.json`, `slot-confirmation-score.jsonl`
and `corpus-continuation3/admission.json`. All rows are now exposed development
data in `exposed-corpora/hermes-third.json`; the original failure is unchanged.

The fourth draft stopped at its predeclared retry bound: only 96/128 rows were
available, and batch 4 timed out on both permitted attempts. Seven author calls
produced three successful batches and four timeouts. No partial corpus was
admitted, reviewed or scored. See `corpus-continuation4/blocker.json`. Further
authoring requires new direction; fresh acceptance remains WITHHELD.

## Subsequent verb-composition repair

The 71-test development suite first reproduced 55 failures. The user then
implemented shared verb inflections, two-clause ownership, missing complement
forms and clarification constructions. Review reproduced a further defect:
`Raise 'a' after converting it to lowercase.` silently proposed lower/97.
The user restricted sequential conversion to output requests, added the 72nd
regression, and reported 2,386 full-suite passes. The coordinator independently
confirmed 72/72 and the corrected refusal before the publication request.
The fresh locked push-time build independently passed all 2,386 tests; see
[publication verification](verification/push-verification.md) and its raw log.

The fix changes typed proposals, not capsule certification, source authority or
training eligibility. No fresh confirmation or new performance benchmark has
been run for this verb candidate. Existing benchmark and coverage numbers below
belong to their earlier named candidates, not this later source. These repairs
do not establish a learned intent model or unrestricted language coverage.

Every population is synthetic and training-ineligible. Repeated symbols and
templates, author/custodian reuse and procedural blinding do not establish IID
sampling, third-party human validation or real-user performance. Even a passing
finite gate cannot establish unrestricted language access or learned semantics.

## Benchmarks and remaining scope

[First candidate measurements](benchmarks-first/README.md) retain three alternating
warm-parser trials and all four native capsule gates. Parser allocation rose
from 2,653.60 to 6,373.70 bytes/request; median throughput fell 2.24%. Additional
grammar coverage is not a speedup. [Final vocabulary measurements](benchmarks-vocabulary/README.md)
retain three new alternating trials: p50/p95/p99 medians 11.621/37.561/48.471 µs,
64,595.53 calls/s and 6,406.96 allocated bytes/call. Baseline p50 varied from
11.411 to 21.300 µs; no clean speedup or statistical significance is claimed.
All four unchanged native executables passed again (12/74/94/55 checks). These
runtime-only timings are not comparable to the first build-plus-gate timings.

[Frozen slot-candidate measurements](benchmarks-slots/README.md) retain another
three alternating trials: p50/p95/p99 medians 11.782/37.300/46.277 µs,
64,927.62 calls/s and 6,417.06 allocated bytes/call. Allocation is 141.82% above
baseline; the timing variability still prevents a clean speedup claim. All four
unchanged native gates passed again. These are warm managed parser measurements,
not deployed end-to-end latency or GPU utilization.

The native gates test small certified capsule fixtures, per-hop coverage and
portability/refusal. They do not measure language quality, end-to-end deployed
latency, GPU training, real demand or autonomous learning improvement. Candidate
source/binary identities are separate: binaries were built and regression-tested
before their source commits, then hash-frozen afterward. Embedded revision metadata
may name the parent; the recorded source bytes match the committed files. Do not rebuild a frozen
artifact and call it the same binary.

Broader semantic coverage, known round11 misses, genuine independently verified
experience, richer workflows, dependency-aware freshness and AMD shadow-training
experiments remain separate roadmap work. Original certification and language
floors are unchanged. No finite proposal benchmark authorizes rollout.
