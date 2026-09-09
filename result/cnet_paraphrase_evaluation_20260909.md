# Bounded task language — September 9, 2026

## Outcome

Source improvements and the independent evaluation mechanism are implemented.
**Fresh confirmation acceptance remains WITHHELD.** The candidate produced no
wrong typed ready proposals in the 128-case confirmation population, but failed
the declared ready-coverage and clarification floors. No floor, label or
population was changed. No rollout, live migration, source approval, genuine
traffic injection, GPU training or frozen-soak change occurred.

| Population / stage | Exact ready | Exact clarify | Exact OOD abstain | Wrong ready | Result |
| --- | --- | --- | --- | --- | --- |
| Frozen qualification, original parser | 27/80 | 13/24 | 15/24 | 3 | FAIL |
| Exposed qualification, repaired parser | 80/80 | 24/24 | 24/24 | 0 | Development PASS only |
| Separate first confirmation | 68/80 | 21/24 | 23/24 | 0 | FAIL |

Confirmation reached 34/40 for each operation, meeting the per-operation 85%
floors. Overall ready was 85%, below 90%; clarification was 87.5%, below 90%.
OOD abstention was 23/24, meeting 95%. All 128 rows were scored; total exact was
112/128. These are proposal classifications and input identities, not native
answers, statistical guarantees or real-user success rates.

## Protocol and retained identities

Two fresh-context Astra authors received only the behavior contract, without
parser source, regression strings or CNET outputs. They cross-reviewed labels
before freezing, resolved wording concerns and removed two exact overlaps.
The final populations share zero exact request strings. This is procedural
same-model independence, not a third-party human benchmark, IID sampling or
independent model training. Both are entirely synthetic and training-ineligible.

The implementer exposed qualification for repair, then froze candidate source
and binary pins before opening confirmation. Confirmation was run **once**;
the candidate parser has not been changed in response to its results.

- Original parser source: `0585835`; source SHA256
  `baddb1aecb944c313c3dfd9a598a1e1c9c42b4c426d03662b8e0fce140127e85`.
- Corpus freeze commit: `9e30968`; [frozen identities](../benchmarks/task_paraphrases_20260909/freeze.json).
- Candidate source/build checkpoint: `1730a67096368f68e9cb05c1499dcde46dcaeeb3`.
- Candidate pin commit: `7f81286`; [source and binary pins](../benchmarks/task_paraphrases_20260909/candidate.json).
- Candidate parser SHA256:
  `d1914eca8eb15250ec639431a8388626ddb8863e4b00c6b9bfd4f278c7bb71e0`.
- Candidate managed DLL SHA256:
  `3498c57d303321a471ffaed736d63af20f2799da1a74b52b4ff3e402c658d6a9`.
- Candidate probe DLL SHA256:
  `860e7196574877ccdb62f829833af665dc8022da22e6b56f92fa50962e9f3270`.

Full reports retain every ID, expected operation/key/status, actual proposal,
family denominator and gate, joined to the immutable corpus by ID:
[baseline](task_paraphrases_20260909/baseline.json),
[development before transport repair](task_paraphrases_20260909/development-1.json),
[development with final transport](task_paraphrases_20260909/development-2.json),
[first confirmation](task_paraphrases_20260909/confirmation-1.json).
Development was evaluated twice for separate grammar and transport changes;
neither repetition is fresh confirmation. Checked-in JSON is compacted from
the complete reports without removing or changing fields.

The `confirmation_opened: false` fields in freeze records describe the
historical freeze moment, not the current state. Confirmation is now exposed.
Subsequent repairs require a new independently authored confirmation set;
rerunning the existing set can only establish development regression behavior.

## Concrete source changes

1. Reject controls at the shared decoded-byte proposal boundary. Baseline
   `U+0000`, decimal codepoint 10 and `U+0085` incorrectly became ready inputs.
   Exhaustive tests now cover every Latin-1 control through canonical, hex and
   decimal forms. Literal controls and surrogates retain their original refusal.
2. Extend whole-request bounded grammar with case synonyms, polite frames,
   explicit input descriptions, decimal codepoint forms and missing-direction
   clarification. No input scalar normalization, answer generation or substring
   instruction execution is introduced.
3. Refuse recognized locale/full-string/compound requests, clarify alternative
   codepoints, and preserve every existing parser regression. Some unsupported
   requests still clarify instead of abstaining; both remain non-executable.
4. Add an exact scorer, frozen corpus validation and actual managed-assembly
   probe. The probe does not start the application or native runtime. The runner
   checks hashes, bounded shapes, duplicate JSON properties, every output and
   no-clobber report publication; it never substitutes missing results.
5. Preserve original UTF-16 units in the probe transport, including lone
   surrogates. The actual parser, rather than a JSON decoder, now produces the
   measured abstention for those inputs.

## Remaining confirmation misses

All 16 are nonready outcomes, not wrong executable proposals:

- Ready misses `c008,c013,c016,c019,c024,c032,c048,c053,c056,c063,c064,c072`:
  `set the case`, trailing `for me`, `may I have`, `single letter`,
  `capital-letter`, `return ... in ...`, and `write ... using ...` frames.
- Clarification misses `c082,c084,c087`: `apply a case operation`,
  `adjust the letter case`, and `convert to upper case` without an operand.
- OOD miss `c116`: `Lowercase 'G' and add twelve to four.` clarified instead
  of abstaining. No dataset, key or action was produced.

The frozen corpus and row reports retain the complete texts and exact outputs.
Do not silently add these phrases and relabel a repeat run as independent.

## Verification and review

- Executed RED/GREEN checkpoints: missing scorer, missing actual probe,
  missing CLI pins, nonscalar JSON fields, encoded controls, language/OOD gaps,
  and lone-surrogate transport. Red test commits and subsequent fixes are in
  branch history, not simulated expected failures.
- Full managed suite: **988 passed, zero failed/skipped**, 1m24s reported test
  duration. Production parser class: **100% instrumented line coverage and
  96.59% branch coverage**. This is not whole-program or generated-record
  coverage. Existing test-project CA1416 platform warnings remain; the final
  probe/production build had zero warnings and errors.
- Private actual-native language/approval replay: passed with teacher and
  self-answer disabled. New µ/A phrasing returned independently verified 924/97
  after explicit source approval and normal capsule learning. Lowercase stayed
  intact; `ß` remained uncovered. Both 256-key table checks passed; that does
  not mean every key answers. Nonready requests created no observations.
- Capture/bridge suite: **148 passed**, zero skipped; no running bridge changed.
- Evaluator suite: **21 passed**, including actual managed probe calls.
  Final stdlib tracing: **97% of 172 executable evaluator lines**. An initial
  direct-script trace discovered zero tests and was discarded; module-based
  unittest discovery executed the reported 21 tests. Child-process .NET
  execution is not included in Python coverage.
- Fresh-context Astra boundary review: 42 independent ordinary/adversarial
  probes found no wrong ready result. Review found the surrogate transport
  defect; it was reproduced, fixed and re-reviewed before confirmation. No
  frozen corpus was available to that reviewer. Some nonready compound/negation
  classification misses remain acknowledged, not described as unsafe execution.

Review covered this change's input, encoding, typed-authority, hash and report
boundaries; it is not a complete repository penetration test. Hash checks do
not sandbox hostile DLLs or a compromised host. Only trusted local builds are
supported. Native certification and approval authority were not relaxed.

ECC TDD and verification-loop skills drove executed failure checkpoints,
regression/coverage checks and the explicit withheld acceptance decision.
Planning, incremental implementation, interface-design, security and
documentation skills kept the work source-only and made the private transport
contract explicit. The doubt-driven skill supplied separate Astra authors and
boundary review; no Claude/Grok or other provider was used.

MCP graph discovery returned no index for this worktree. Graft was used for
parser/fixture discovery as the fallback and refreshed after source edits;
its graph is navigation evidence, not a correctness or token-savings metric.
Final Graft wiring: 2,001 source files, 24,163 nodes, 24,410 edges and 1,973
cards; `graph.ok=true`, zero added/changed/stale files. Model-generated deep
context remains unbuilt (`context.missing=true`); no external model was called
to populate it. Documentation framing/interface gates and `git diff --check`
passed.

## Reproduction and next scope

Follow [the runbook](../docs/TASK_PARAPHRASE_EVALUATION.md), not the development
DLL against a live ledger. Local logs are retained under
`/tmp/cnet-paraphrase-*20260909.log`; full coverage artifacts are under
`/tmp/cnet-paraphrase-managed-coverage-20260909/` and
`/tmp/cnet-paraphrase-python-coverage3-20260909/`. The durable reports above do
not depend on those temporary logs.

Next: repair the exposed ready/clarification gaps with test-first changes and
obtain a **new** blind confirmation population with the same floors. Richer
composition inputs, dependency-aware freshness/source replacement, reviewed
captured-task rollout and measured AMD selection candidates remain separate
product work. Real origin review and eligible chronological experience are
still required; no synthetic score supplies those missing prerequisites.
