# Offline learned intent proposer — 2026-09-10

## Goal

Take unclaimed English-to-typed-request frames off hand-written sentence
tables. A learned proposer maps unfamiliar wording to a typed case
operation. Existing operand decoding, coverage and policy remain the
authority. Propose ≠ admit.

## What this is not

- Not fresh confirmation. Exposed corpora stay development data.
- Not live deployment or soak mutation.
- Not removal of the grammar. Grammar still owns claimed ready, clarify
  and domain refusals.
- Not training on CNET capsule answers.

## Architecture

```
request → grammar (claimed constructions)
       → else learned frame class {upper, lower, clarify, abstain}
       → independent scalar bind (quotes, U+/0x, bare letters)
       → ProposeOperand + policy/coverage
```

A confident class cannot skip domain, extra-scalar or negation checks.

## Training

Answer-free external spec in `LearningIntentSpec`. Labels are request
roles (ready/clarify/abstain + direction), never casing results.
Character n-gram linear model, deterministic seed, calibrated so
in-corpus wrong-ready is 0.

## Promotion rule

Learned output is used only when grammar returns unclaimed
`unsupported_intent`. Promote further (grammar-off inference) only if a
new blind confirmation meets original floors with zero wrong ready.
