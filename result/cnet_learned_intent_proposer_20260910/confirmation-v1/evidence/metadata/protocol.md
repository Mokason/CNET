# Confirmation v1 predeclared protocol

Status: PREDECLARED before corpus authorship.
Worktree: /home/marble/AI/CNET-worktrees/herdr-language-20260909
Private workspace: /tmp/cnet-learned-confirmation-xAqJks4W
Durable metadata directory: result/cnet_learned_intent_proposer_20260910/confirmation-v1

Roles: fresh-context corpus author, independent custodian, fresh-context label-blind reviewer, frozen-candidate evaluator (root). Author and reviewer receive the behavioral contract only, with no candidate implementation, training specification, prior corpus text, prior scores, or failure feedback. Author writes data; custodian never authors cases, runs CNET, or scores. Reviewer initially sees only mechanically generated id/text records and independently labels every record. Independence is procedural fresh contexts on the same model/tools, not enforced OS isolation or independent organizations. No external model providers, credential/config access, commits, pushes, deployments, or GPU activity.

## Behavioral contract
Evaluate bounded Latin-1 Unicode case typed proposals, not answers. The only executable task is one explicit uppercase or lowercase request applied to one Latin-1 input scalar, 0..255. Ready carries operation upper/lower and ORIGINAL input scalar, never converted output. Native coverage may separately refuse; do not drop those rows. Direct commands and natural English paraphrases/common case synonyms are in scope. Single letters may be bare; literal digits, spaces and punctuation require matching quotes. U+ hexadecimal and explicit decimal codepoint operands are unambiguous. Do not interpret bare digit strings as codepoints or guess characters from names. Missing direction/input, multiple candidate characters, malformed quoting and bare numeric/punctuation operands require clarify. Unrelated tasks, negated or merely quoted instructions, compound/embedded requests, locale-specific/full-string casing and inputs outside Latin-1 require abstain. Controls, surrogates and >256 UTF-16-unit requests also abstain. Avoid linguistically debatable labels; resolve before freeze. All data synthetic, training-ineligible.

## Schema and quotas
Top level exactly schema=1, name=confirmation, cases. Each case exactly id,family,text,status,operation,key. id and family match [A-Za-z0-9_-]{1,48}. Exactly 128 distinct texts: 80 ready (40 upper,40 lower),24 clarify,24 abstain; at least 8 named syntax/input families. Non-ready operation and key are null. Requests are data only and are never executed as shell/code/paths.

## Bounded admission
Preserve every draft and blind-review output in private storage. At most two pre-score repair rounds, strictly for objective structural, label, or overlap issues; author alone repairs. At most three blind review passes: initial all 128, then only changed rows with unchanged-row hashes verified. No output-conditioned repairs and no continuation beyond these bounds. Any unresolved ambiguity or exhausted bound prevents admission.

Custodian checks exact schema, quotas, unique IDs/texts, scalar keys, control/surrogate/request-length status, and independent reviewer agreement. Strict load_corpus validator may be read from tools/task_paraphrase_eval/evaluate.py without reading candidate implementation. Contractual section in plans/cnet_paraphrase_evaluation_20260909.md may be read.

Prior text overlap uses mechanical comparison without displaying previous texts: benchmarks/task_paraphrases*/freeze.json, confirmation.json, qualification.json and result/cnet_herdr_three_heads_20260909/exposed-corpora/*.json. Reject exact and normalized overlaps internally and against previous exposed 128-case corpora. Normalization: Unicode casefold, remove every Unicode punctuation codepoint (category P*), remove all Unicode whitespace. Root supplies hash-only denylist for current training/calibration under this same normalization. Record number of source files/texts and hashes; reveal only offending NEW case IDs to author. No prior text is sent to author/reviewer.

After all checks pass, freeze corpus read-only, hash with SHA-256 and write metadata-only admission receipt identifying all paths/hashes, schema/quotas, reviewer evidence/dispositions, overlap evidence, role provenance, synthetic/training-ineligible status and no-score declaration. Keep cases and review labels private until root scores. Do not modify candidate-freeze. Root independently binds existing candidate-freeze and scores at most once after admission.

## Unchanged scoring floors (not author hints)
Zero wrong ready; ready >=72/80, upper >=34/40, lower >=34/40, clarify >=22/24, abstain >=23/24; no executable fields on non-ready. Custodian and author do not score or optimize using these floors.

