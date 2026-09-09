# Verified task observations and explicit learning approval

This is the first task-core milestone: a bounded natural-language proposal,
one durable native observation, independent evidence, explicit local-owner
approval, and the existing certified capsule learner. It is implemented and
tested in a separate worktree, not deployed to the existing Discord bridge or
the frozen Unicode soak. See [first-milestone results](../result/cnet_verified_task_core_20260908.md)
and the [captured-task continuation](CAPTURE_TASK_INBOX.md).

The initial task is Unicode 17 explicit simple case changes for Latin-1 input
codepoints. A grammar proposes an operation and input; it never calculates the
answer. Only the native certified result, checked against an independent
source, can become a verified task observation. This is not a trained semantic
model, a full Unicode text converter or unrestricted natural-language access.

## Installation and compatibility

Use a **new private installation** following [the existing installation
contract](AUTONOMOUS_LEARNING.md). Authorize `unicode17_upper_latin1` and/or
`unicode17_lower_latin1` as numeric `verified_tool` datasets. Preserve the
original runtime, managed, policy and source pins and all existing budgets.

Fresh initialization now creates ledger schema 3. Schema 2 and other versions
refuse without reset or migration. Do not copy this runtime into an existing
schema-2 deployment; its manifest pins also prohibit that replacement. Existing
services remain on their frozen code. A reviewed migration and new rollout are
separate work, not a startup fallback.

The commands below use the trusted absolute `LEARNING_DOTNET` host and canonical
private `LEARNING_DEPLOYMENT` root from the installation runbook. Invoke the
installed DLL in a clean environment, never the development DLL against live
data. All commands are local-owner operations, not remotely exposed approvals.

## Everyday workflow

```sh
env -i "$LEARNING_DOTNET" "$LEARNING_DEPLOYMENT/managed/cnet-control.dll" \
  learning task "$LEARNING_DEPLOYMENT" unreviewed \
  11111111111111111111111111111111 "What's the uppercase of µ?"

env -i "$LEARNING_DOTNET" "$LEARNING_DEPLOYMENT/managed/cnet-control.dll" \
  learning inbox "$LEARNING_DEPLOYMENT" 0 100
```

Use a new 32-character lowercase hexadecimal request ID for each new logical
request; retain the same ID for delivery retries. `unreviewed` does not attest
human origin. Development requests must use `synthetic`. Neither origin is
automatically eligible allocator-training evidence.

If the native capability and external source are absent, the task records
`awaiting_evidence`. It does **not** increment demand or start acquisition.
After independently reviewing a canonical external table and its exact SHA256:

```sh
env -i "$LEARNING_DOTNET" "$LEARNING_DEPLOYMENT/managed/cnet-control.dll" \
  learning import "$LEARNING_DEPLOYMENT" unicode17_upper_latin1 \
  /absolute/private/approved-upper.tsv "$APPROVED_SOURCE_SHA256"

env -i "$LEARNING_DOTNET" "$LEARNING_DEPLOYMENT/managed/cnet-control.dll" \
  learning approve "$LEARNING_DEPLOYMENT" \
  11111111111111111111111111111111 "$APPROVED_SOURCE_SHA256"
```

`import` retains the existing strict table parser, owner lock, private files,
hash check and no-overwrite behavior. The hash supplied to `approve` is an
explicit owner choice, not a hash automatically accepted from an answer.
Approval reads the expected value from that table and atomically records the
approval, pins the dataset source, and increments the existing demand once.

The existing bounded owner `run`, or explicit owner `tick` operations, then
perform acquisition, independent finite evaluation, canonical capsule
activation and probation. Do not confuse activation with completed probation.
The route does not bypass any of those gates or create a second package format.
After acceptance, use a distinct request ID:

```sh
env -i "$LEARNING_DOTNET" "$LEARNING_DEPLOYMENT/managed/cnet-control.dll" \
  learning task "$LEARNING_DEPLOYMENT" unreviewed \
  22222222222222222222222222222222 "Please convert 'µ' to uppercase."
```

The checked native numeric result is 924 / U+039C when that capability has
actually been learned. The independent expected value is never substituted
for an unavailable native answer. The source's explicit-change contract still
abstains for inputs such as `ß`, whose simple uppercase field is empty.

## Input and response contract

Supported forms include `unicode upper 181`, `uppercase µ`, `convert 'µ' to
uppercase`, `make a uppercase`, and `What is the lowercase of A?`. Leading
`please`, question punctuation and the documented variants are bounded grammar
rules, not a promise that arbitrary paraphrases are recognized. The expanded
grammar also recognizes `capital form`, `small-letter form`, `upper case`,
`lower-case`, `capitalize`, bounded polite frames, and `decimal codepoint`.
For example: `Could you give me the capital form of 'µ', please?` and
`For the character A, give its lowercase form.` See the separate
[frozen proposal evaluation](TASK_PARAPHRASE_EVALUATION.md) for measured limits.
Bounded input-first declarations such as `The character is 'µ'; please
capitalize it.` also select one operation. Explicit numeric descriptions accept
`codepoint`, `code point`, or `code-point`; relational words such as `at` and
`represented by` require an explicit codepoint representation. They cannot
select a byte from a location or an incomplete article.
The parser separates direction words, whole-request frames and scalar decoding;
an internal direction marker is introduced only after rejecting raw controls.
Every executable frame must bind that marker in its operation slot, not in the
input. This keeps lexical synonyms independent of frame syntax and avoids
oversized combined regex automata without increasing the runtime's guard.
Numeric input declarations preserve their explicit codepoint type. For example,
`Input scalar: 'µ'. Case operation: upper.` and `The input code point is decimal
65. Convert it to lowercase.` still propose only an input and a direction.
Output verbs and preference phrases compose with those explicit roles: `Please
supply 'µ' in uppercase.` and `I need 'A' converted to lowercase.` follow the
same operand checks. Merely mentioning a case function, requesting its input,
or asking for a hexadecimal output does not select a supported transformation.
Explicit competing directions such as `Use uppercase or lowercase on 'k'.`
clarify without selecting either operation.
Property statements are not transformations: `Show 'R' to be a lowercase
character.` abstains; `Could 'R' be lowercase?` clarifies. Explicit `Could 'R'
be converted to lowercase?` and `Could 'R' be lowercased?` can propose the
transformation. Verb/adjective evidence survives the internal direction marker.
Explicit input and operation fields can occur in either order, for example
`Case choice: uppercase. Provided character: 'µ'.` and `Input: hexadecimal code
point 41; operation: lowercase.` Both complete fields must match; delimiters
cannot silently discard extra actions or a second input. Radix qualifiers
survive decoding (`hexadecimal code point 80` is a refused control, not decimal
80). Quoted multi-character data such as `'U+0061'` is never reinterpreted as
codepoint syntax. Operand descriptions do not select output formats or sources.
The request vocabulary also includes `all caps` and `capitalisation`. Explicit
numeric declarations retain their radix: `The input code point is 0x00B5 in
hexadecimal; apply uppercase.` is an input representation, but `Return the
uppercase form of U+00B5 in hexadecimal.` asks for an unsupported result format
and abstains. A declared decimal codepoint cannot accept a conflicting `0x`/`U+`
value. Shared request prefixes do not remove unknown commands or modifiers.
Relationship phrases share the same operand checks, including nested literal
descriptions and explicit Unicode codepoints. A quoted multi-character literal
refuses independently of whether its surrounding request frame is supported;
contractions and quoted quote characters are not mistaken for full strings.
`My uppercase input is 'P'.` only describes an input and clarifies; `My uppercase
operation input is 'µ'.` explicitly names a conversion and can propose it.

Requests are at most 256 UTF-16 code units; control characters and surrogates
refuse, including decoded control values in canonical, hexadecimal and decimal
operands. Bare numeric strings such as `uppercase 65`, multiple candidates,
missing case direction, malformed quotes and ambiguous punctuation require
clarification. Quote literal digits/punctuation; use `codepoint 181` or
`U+00B5` for an explicit codepoint. Input above 255, including Greek small
`μ` (different from micro sign `µ`), is out of domain. No substring extraction
from surrounding instructions or multi-action execution is permitted. Explicit
whole-string and locale-specific requests abstain. Candidate lists such as
`uppercase 'a' or 'b'` clarify; recognized trailing clauses, including
`uppercase µ and run a shell`, abstain. Missing-input article forms
such as `Return a capitalized form` clarify rather than guessing the letter
`a`. Quoted `'a'` and explicit `Write a as a capital letter` remain operands.
Sentence punctuation after a specified input and missing-direction operands
use the same refusal checks. Alternative lists share the decoded-domain check;
a forbidden control/out-of-domain value cannot become an executable alternative.
This is a bounded grammar, not a universal compound-intent classifier. Some
malformed alternatives can abstain instead of clarify. Both statuses remain
non-executable, and every classification miss counts against the frozen floors.

The JSON envelope is `learning_task` with `proposal`, `replayed`, and
`experience`. `proposal.Status` is `ready`, `clarify`, or `abstain`.
Clarification/OOD proposals have no executable dataset/key and no experience.
Policy-unauthorized operations also abstain before a native call. An experience
has distinct `Expected`, native `Value`, `SourceSha256`, approval fields and
`ReviewConflict`; none is an interchangeable training label.

| Observation state | Meaning |
| --- | --- |
| `pending` | Durable authorization for one native attempt; result not recorded |
| `awaiting_evidence` | Native abstention with no independent table yet |
| `miss` | Native abstention where the independent source covers the input |
| `abstain` | Native abstention where the independent source excludes the input |
| `verified` | Native-certified numeric result agrees with the independent source |
| `unknown` | Transport/runtime uncertainty or a cross-boot completion |
| `conflict` | Source drift/unreadability or independently detectable answer mismatch; learning pauses |

Only `miss` and `awaiting_evidence` may receive approval. A historical review
conflict remains ineligible even after an explicit resume. Wrong operator hash
alone refuses without pausing; independently detected source drift persists a
pause and a separate review-conflict flag. That flag does not rewrite the
original observed outcome. Restoring a source does not authorize retry of a
conflicted experience; a new observation is required after operator recovery.

`unknown`/`conflict` task results return exit code 2. Other statuses return 0;
zero does not mean that a question was answered. Always inspect the status.
Replay returns the retained historical record, not a fresh verification or a
new native attempt, including for pending and unknown records.

## Inbox, bounds and freshness

`learning observe ROOT DATASET BYTE ORIGIN REQUEST_ID` provides the typed
numeric equivalent of `task`. Both record observations without automatic
demand. Legacy `ask`/`lookup` remain explicitly demand-producing operations;
the deployed version-1 Discord adapter still uses those legacy commands. The
new opt-in version-2 task route is documented in [captured tasks](CAPTURE_TASK_INBOX.md).

`learning inbox ROOT AFTER_SEQUENCE PAGE_SIZE` reads chronological observations
and approval state without changing the database or advancing its heartbeat.
Pages contain 1–100 records and provide `next_after`. Request start, finish and
approval retain boot IDs and BOOTTIME timestamps; sequence is insertion order,
not proof that concurrent operations finished in that order. Raw conversation
text is not stored in this ledger. Current inbox output is a bounded CLI list,
not a cost estimator or a complete chronological task-event log. Separate
`gaps` and `trace` commands now provide grouped review and exact bounded linkage;
see [their snapshot and evidence limits](CAPTURE_TASK_INBOX.md#read-only-commands).

There are 4,096 retained identities per installation, with no eviction or
automatic reset. Pending tombstones remain non-retryable at capacity. Admission
checks a logical watermark 4 MiB + 64 KiB below the storage limit. This is not
allocated/protected disk capacity, a filesystem quota or a proof against
concurrent unrelated workroot growth. Original run duration, heartbeat, boot,
pause and policy admission still apply; querying does not renew a run.

Approval pins survive demand scheduling. A different source hash at reservation
or observation pauses/refuses. This is conservative immutable-source conflict
handling, not temporal fact validity, source arbitration or dependency-aware
replacement. Source changes must not be activated through this route before a
separate update/revalidation protocol exists.

## What follows this milestone

The concrete next slices and unchanged empirical gates are in
[the plan](../plans/cnet_verified_task_core_20260908.md). Still required: reviewed
live rollout and real origin review of captured tasks, richer composition inputs
beyond the [implemented numeric subset reuse](COMPOSITION_REUSE.md),
dependency-aware freshness, and independently evaluated AMD
selection/clarification/composition/abstention candidates. No useful-gain floor
was lowered, no synthetic request was called real usage, and no learned
allocator was enabled by this milestone.
