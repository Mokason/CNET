I recommend one bounded slice: **parse single-clause case requests from reusable token constituents, preserving the original operand and its grammatical role**. This could improve unseen combinations of familiar language without adding more complete sentence templates. Generalization remains WITHHELD until independent confirmation passes.

HEAD matched `5041f00`. I inspected source, tests and interfaces; no implementation, tests, providers or services were run, and no new confirmation was inspected.

The likely failure mechanism is incomplete structural coverage. [LearningTaskProposal.cs](/home/marble/AI/CNET-worktrees/herdr-language-20260909/dotnet/CnetControlPlane/Learning/LearningTaskProposal.cs:18) already separates operation vocabulary from syntax, and its field/binding helpers already compose some requests. However, `Forms`, `InputFields` and ambiguity handling still enumerate complete constructions. Recognition depends on ordered matches, while special checks restore distinctions lost by replacing operation text with a marker: conversion versus property, input versus recipient, and matching versus conflicting aliases. See the [request dispatch](/home/marble/AI/CNET-worktrees/herdr-language-20260909/dotnet/CnetControlPlane/Learning/LearningTaskProposal.cs:344).

That architecture can perfect exposed examples while missing new combinations of otherwise supported constituents. Clarification has a similar problem: an unfamiliar incomplete request often reaches `unsupported_intent` rather than a recognized request with a missing slot. Existing [bound-request combination tests](/home/marble/AI/CNET-worktrees/herdr-language-20260909/dotnet/CnetControlPlane.Tests/LearningTaskBoundRequestTests.cs:9) are useful development checks, but their combinations come from a finite authored inventory.

The supplied latest figures mean 28 ready misses and eight clarification misses. Zero wrong ready proposals and 24/24 abstention support preserving conservative boundaries; they do not establish safety beyond those cases. The eleven failed confirmations support this diagnosis but cannot prove it. Per-operation and family breakdowns were not supplied; I claim none.

The alternatives differ materially:

| Approach | Feasibility and limitation |
|---|---|
| Bounded token composition | Feasible in managed source without weights, training or new dependencies. Reuses lexical and operand knowledge across constructions, but still has an explicit linguistic boundary. |
| Existing learned semantic proposer plus independent validation | The architecture is pertinent, but the current model is unsuitable for this task: its [intent labels](/home/marble/AI/CNET-worktrees/herdr-language-20260909/include/cnet_compete_intent.h:22) omit case conversion; its [tokenizer](/home/marble/AI/CNET-worktrees/herdr-language-20260909/src/compete/cnet_compete_intent.c:473) rejects non-ASCII and discards word order and numeric identity. |

The learned runtime does demonstrate the right separation: [independent contract interpretation must agree with the proposal](/home/marble/AI/CNET-worktrees/herdr-language-20260909/src/compete/cnet_compete_runtime.c:2481). Its current envelope also rejects character/literal requests. The [semantic cortex](/home/marble/AI/CNET-worktrees/herdr-language-20260909/src/memory/cnet_semantic_cortex.c:103) supplies hermetic token candidates, not a case-task semantic parser; its capsule-intent adapter handles numeric unit conversions.

Extending the classifier requires independently verified intent data, a changed label/representation contract, training and calibration under separate authorization. A pretrained alternative requires approved weights/tokenizer and independently measured task suitability. Neither current synthetic acceptance examples nor CNET replies may become training truth. An online model is outside this proposal.

The implementation slice would own only single-clause commands and requests: unary case verbs, conversion verbs with a target case, and rendering requests with a case complement. Existing canonical syntax and declaration/reference constructions remain compatibility paths.

Instead of storing complete sentences, compose:

- Request wrappers and politeness in defined positions.
- An action with explicit argument roles.
- An operand phrase containing a literal or explicit codepoint.
- A target-case phrase, including reviewed common lexical aliases such as `caps`.
- Optional terminal punctuation outside quoted data.

A quote-aware lexer retains source offsets and folds only grammatical words. Fixed-depth productions must consume the entire request; unknown words cannot be deleted. Operation phrases and operand descriptions combine independently. Missing or ambiguous slots produce clarification when the complete surrounding request is recognized.

Dispatch distinguishes **unclaimed construction** from a claimed result. Only the former may enter existing compatibility parsing. A claimed ambiguity or refusal is terminal; no subsequent frame may reinterpret it. This prevents a second parser becoming a refusal bypass.

Five production/test files suffice:

1. Existing `LearningTaskProposal.cs`: dispatch and checked projection into the unchanged proposal.
2. New `LearningCaseRequestSyntax.cs`: lexer, constituent parser and internal candidate types.
3. New `LearningCaseRequestSyntaxTests.cs`: composition and adversarial RED cases.
4. Existing `LearningTaskParserTests.cs`: scalar, status and compatibility invariants.
5. Existing `LearningTaskExecutionTests.cs`: newly recognized requests through policy and observation boundaries.

The exact safety boundary is **original text plus an untrusted candidate → checked operation/input → existing policy and observation path**. A candidate carries an operation enum, operand representation, source spans and structural roles. It carries no answer, dataset choice, source authority, approval or coverage assertion.

Checked projection requires:

- Raw request length 1–256 UTF-16 units; no controls or surrogates. Decoded controls also abstain.
- One requested operation applied to one operand. Competing alternatives clarify; multiple executable actions abstain, including repeated identical actions.
- Complete grammatical consumption establishing request scope. Negation, quotation of an instruction, embedded requests, locale/full-string transformations and extra actions abstain.
- Independent decoding of the original operand span: one Latin-1 scalar; bare letters only; matching quotes for literal digits, spaces and punctuation; explicit hexadecimal or decimal codepoint syntax. No character-name guessing, normalization or conversion of the operand’s case.
- Missing direction/input, malformed quoting and ambiguous operands clarify. Out-of-domain operands abstain.
- Dataset derived solely from the checked operation: `unicode17_upper_latin1` or `unicode17_lower_latin1`. Every non-ready result has null dataset/key.

The existing explicit `unicode upper N`/`unicode lower N` compatibility syntax stays intact.

**Shape validation alone is insufficient.** A model can emit a valid `lower`/81 pair for a negated request; native certification could then correctly answer the wrong question. A future semantic candidate must satisfy independently checked request scope and operand binding. Confidence or model-authored explanations cannot replace that check.

The public [task envelope validator](/home/marble/AI/CNET-worktrees/herdr-language-20260909/tools/discord_capture/task_protocol.py:59) requires exact fields and refusal semantics, so these remain unchanged. [LearningTaskCommand](/home/marble/AI/CNET-worktrees/herdr-language-20260909/dotnet/CnetControlPlane/Learning/LearningTaskCommand.cs:10) retains policy checks before observation. Source approval, capture identity, coverage and native certification retain authority; capsule packaging is untouched.

Meaningful RED tests should precede production edits, with `TASK_CONSTITUENT_RED` failures captured against the baseline. Illustrative development cases—not independent evidence—include:

| Text | Required proposal |
|---|---|
| `Could you kindly render the single quoted character 'É' in lowercase?` | ready, lower, 201 |
| `Please write 'ñ' in caps.` | ready, upper, 241 |
| `Could you kindly render the single quoted character in lowercase?` | clarify |
| `Please write 81 in caps.` | clarify |
| `Please write 'ñ' in caps and email it.` | abstain |

Cross independently specified wrappers, both directions and operand representations; do not generate expectations from production grammar tables. Include minimal counterexamples for recipient versus operand, property versus conversion, missing operands, alternative inputs, mismatched quotes and hidden extra actions. Preserve original-key checks for `µ` versus out-of-domain `μ`, quoted punctuation, all encoded controls, and 256/257-unit boundaries. Forge inconsistent candidate spans and operation roles to test rejection before projection.

Existing [execution tests](/home/marble/AI/CNET-worktrees/herdr-language-20260909/dotnet/CnetControlPlane.Tests/LearningTaskExecutionTests.cs:15) must demonstrate that unauthorized proposals create no experience, transport uncertainty remains unknown, and proposals create neither demand nor jobs. Existing native coverage checks must continue distinguishing a well-formed `ß` proposal from an uncovered action.

After Claude’s challenge and implementation authorization, the sequence is:

1. An independent custodian freezes and hashes both contract-compliant 128-case collections before edits. Authors receive only the contract. No confirmation access is assumed.
2. Score qualification on the baseline, retain every miss, and classify failures by mechanism. Once exposed, it is development evidence.
3. Record meaningful RED failures; implement the five-file slice incrementally; run parser, approval, capture, capsule and native-coverage regressions in isolated fixtures.
4. Require qualification to meet all frozen floors before freezing candidate source. Subsequently authorized confirmation runs once, reported separately.

Go requires zero wrong ready proposals; at least **72/80** exact ready pairs, **34/40 per operation**, **22/24** clarifications and **23/24** abstentions; no executable non-ready fields; and intact existing gates. The reported 1,861 passes are regression evidence, not fresh language evidence. Retain hashes, category/family counts, all misses and exclusions without relabeling, dropping rows or pooling collections.

The plan is disproved if misses principally require meanings outside these constituents; improvements require continued sentence enumeration; role or scope counterexamples become ready; qualification cannot meet the floors within this slice; or authorized confirmation fails any floor. Passing generated combinations alone would not rescue it.
