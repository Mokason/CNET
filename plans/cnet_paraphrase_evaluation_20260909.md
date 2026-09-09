# Frozen task-proposal evaluation — September 9, 2026

## Contract and independence

Evaluate the bounded Latin-1 Unicode case proposal route, not answer generation.
Proposals remain untrusted typed inputs; policy, coverage, external evidence and
native certification retain final authority. No new source, live traffic,
deployment, GPU fitting or frozen-soak change is authorized by this experiment.

Freeze two separately authored synthetic collections before parser edits. Authors
receive only the task contract, not parser code, regression strings or results.
The first collection assesses the existing grammar. Once exposed for repair it
becomes development evidence. The second remains unread by the implementer
until a candidate source checkpoint is frozen. Evaluate confirmation once; any
subsequent adaptation requires a new independent confirmation collection.
Hash both files and retain all scores and exclusions. This procedural separation
is not a third-party human benchmark, IID sampling or proof of real-user quality.

## Label contract for independent authors

The only executable task is one explicit uppercase or lowercase request applied
to one Latin-1 input scalar, 0..255. `ready` labels carry operation `upper` or
`lower` and the original input scalar, never an answer. Coverage may still
exclude a well-formed input (for example no explicit simple case mapping).

Use direct commands and natural English paraphrases; common case-operation
synonyms are in the intended evaluation scope. Single letters may be bare;
literal digits, spaces and punctuation must be quoted with matching quotes.
`U+` hexadecimal and explicit decimal `codepoint` operands are unambiguous.
Do not silently interpret bare digit strings as codepoints or guess a character
from a character name. Missing direction/input, multiple candidate characters,
malformed quoting and bare numeric/punctuation operands require `clarify`.

Unrelated tasks, negated or merely quoted instructions, compound/embedded
requests, locale-specific/full-string casing and inputs outside Latin-1 require
`abstain`. Controls, surrogates and >256 UTF-16-unit requests also abstain.
Avoid linguistically debatable labels; document uncertainty before freezing.
Never call a model's generated answers or CNET's own replies source truth.

Each collection has exactly 128 distinct text cases: 80 ready (40 per operation),
24 clarify and 24 abstain, with at least eight named syntactic/input families.
File JSON: `{"schema":1,"name":"qualification|confirmation","cases":[...]}`.
Each case has exactly `id`, `family`, `text`, `status`, `operation`, `key`;
non-ready cases use null operation/key. IDs and families are short ASCII atoms.
No corpus text is executed as shell, code or a file path.

## Predeclared acceptance floors

- Zero wrong typed ready proposals, including ready proposals on any non-ready
  case. This is proposal safety, not a claim that a native action was executed.
- >=90% exact ready operation/key accuracy overall; >=85% per operation.
- >=90% exact clarification status and >=95% exact OOD abstention status.
- No non-ready output may contain an executable dataset/key.
- Existing parser, approval, capture, capsule and native-coverage gates stay intact.

Report category/family counts and all misses. Do not hide conservative refusals,
drop difficult rows, change labels after scoring, lower floors or count repeat
runs as fresh evidence. Selection and confirmation are not pooled.

## Implementation sequence

1. Test-first bounded corpus validation, hash enforcement and exact scoring.
   Verify missing/changed/malformed/duplicate cases and misleading scores refuse.
2. Freeze authored corpora and source identity; run the existing grammar on the
   first collection only. Retain the complete baseline before parser repair.
3. If needed, add minimal bounded parser improvements with executed RED/GREEN,
   preserving full-request matching, ambiguity and native authority boundaries.
4. Freeze candidate code before opening the confirmation collection. Run exact
   scoring and a private native replay/coverage check; report every gate honestly.
5. Run regressions/coverage, refresh Graft, record ECC/review evidence and update
   documentation/current queue. Commit locally without push or deployment.

## Recorded outcome (protocol and floors above unchanged)

The sequence was executed. See [the measured handoff](../result/cnet_paraphrase_evaluation_20260909.md):
the repaired candidate passed exposed development, but its first confirmation
failed ready coverage and clarification (68/80 and 21/24). OOD abstention was
23/24 and wrong ready proposals were zero. Acceptance is WITHHELD. The parser
was not adapted after opening confirmation; future repairs require a new
independently authored confirmation population. No live rollout occurred.
