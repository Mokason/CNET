# Partial-intent candidate qualification

The previous confirmation-v1 remains a failed fresh result: 80/80 ready,
19/24 clarify, 24/24 abstain and zero wrong ready. Its exact uncommitted source
was copied to a hash-verified candidate-source snapshot BEFORE this new repair;
its frozen DLL, corpus, admission, raw predictions and score remain unchanged.

## Implemented change

The existing operation/input field parser now recognizes bounded incomplete
operation slots and explicitly uncertain input fields, in either order. Exact
whole-field matches precede unary operand ownership. Ordinary certain `input is`
and `input:` fields stay with their existing ready path. The new partial path
cannot emit ready: validated scalars and alternatives become clarification;
domain and extra-action refusals remain terminal. Newly claimed `for/on`
directionless relations reject arbitrary unknown operand prose.

This reuses operation lexemes/nouns, operand validation and alternative-list
validation. It changes no learned-model weights/specification, certification
floor, capsule format, source authority, GPU worker or live runtime.

## Executed evidence

- Initial focused RED: 8 failed, 11 passed (five exposed misses plus three
  structural cross-product checks), with `PARTIAL_INTENT_RED` assertions.
- First focused GREEN: 19/19.
- Broader regression caught one ownership defect: a certain `input is` field
  incorrectly clarified instead of remaining ready. Narrowed uncertainty roles;
  added two ready-ownership tests. Next focused suite: 1504/1504.
- Independent boundary review reproduced two arbitrary-action operand defects
  in newly added `for/on` relations. Added executed RED tests, constrained their
  partial-operand ownership, and added reverse-order field tests (also RED first).
- Final focused parser suite: **1506/1506**, including **23** dedicated partial
  intent xUnit cases and their cross-product loops.
- Fresh isolated full suite with locked restore: **2426/2426**, zero failed or
  skipped. [Retained TRX](qualification/full-suite.trx). Existing test-only CA1416
  warnings remain; no warning suppression was added.
- Setup-only failed attempt: the new isolated artifact path initially used
  `--no-restore`, so NETSDK1004 correctly reported missing project.assets.json.
  The successful full build used `-p:RestoreLockedMode=true`. This was not a
  production-behavior RED or a failed test assertion.
- Independent follow-up boundary review: **21/21 diagnostic probes** matched
  expectations, including reversed fields, quoted separators, extra actions,
  negation, full strings, controls, OOD scalars and certain-input ownership.
- [Exposed development check](development.json): **128/128**, zero wrong ready,
  on the OLD, already-exposed corpus with the NEW DLL. Not fresh acceptance.
- The reviewed single-use runner was reused unchanged in this new directory;
  its five guard tests passed against this copy. The original exact evaluator
  and probe are unchanged, with their previously recorded 34 integrity tests.

Full build command:

```sh
dotnet test dotnet/CnetControlPlane.Tests/CnetControlPlane.Tests.csproj --artifacts-path .artifacts/partial-intent-v2 -p:RestoreLockedMode=true --logger 'console;verbosity=minimal' --logger 'trx;LogFileName=full-suite.trx' --results-directory /tmp/cnet-partial-intent-z1XhzH8i/results
```

## Freeze and limits

[Candidate freeze](candidate-freeze.json) preceded fresh corpus authoring and
contains eight pins plus a durable source snapshot and regression/development
evidence hashes. Candidate DLL SHA-256:
`98c8a4026f2ab50dfab02c4b18ccf4ea3cae184d747ade0b0562fe2968ff261e`.

Debugging, incremental implementation and independent review guided the
RED/GREEN and ownership fixes. Documentation records preserve the earlier failed
candidate; git workflow preserved user changes without staging, commit or push.
The skill's shared definition-of-done reference is not installed, so completion
uses repository gates and the predeclared frozen evaluation protocol.

No claim of incremental learned-model gain follows from grammar-on performance.
At freeze, fresh acceptance awaited the NEW one-shot confirmation. That sequence
subsequently [passed every original floor](result.md). Grammar-off, deployment,
training eligibility and broader product acceptance remain WITHHELD.
