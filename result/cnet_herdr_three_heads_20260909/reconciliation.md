# First cross-model design checkpoint

Baseline source: `5041f00`; actual managed probe DLL:
`50c7dd24adec1024437320a6670f1e4e4ca2ca58f5f27fc7cef05338ff7a2568`.
No new acceptance population was scored. All review examples are synthetic
development probes, training-ineligible; no native action was executed.

## Actual completed artifacts

- Astra returned [a concrete five-file design](astra-design.md) using
  `gpt-6-astra`, xhigh, in a read-only repository session.
- Claude returned [the complete review and usage receipt](claude-response.json),
  successful one-turn completion in 402,445 ms, resolved primary model
  `claude-opus-5`. The CLI also records an auxiliary Haiku request. Built-in
  tools and MCP were disabled. Its reported USD 1.033421 is list-price usage
  accounting, not evidence of an actual charge against the user's subscription.
- Root executed [15 diagnostic probes](review-probes.json) against the pinned
  DLL to check specific findings. This is not a new benchmark population.
- Hermes/Grok returned no corpus in its first two attempts: event-idle guards fired at
  12 seconds, then 90 seconds on a bounded invocation-local retry. Both failures
  exhausted three internal retries. A later minimal same-backend health request
  returned `HERMES_GROK_HEALTH_OK`; four smaller authoring batches followed.
  No holdout is admitted without complete structural and independent label review.

## Findings reconciled against implementation and compatibility

| Finding | Classification | Disposition |
|---|---|---|
| R1: unrelated/full-string operands become clarification | Valid/actionable | Reproduced both supplied examples; address explicit unknown residue and role binding. |
| R2: punctuation/space cliffs | Valid/actionable | Reproduced all three examples; normalize only outside literals and keep original spans. |
| R3: `code point 41` necessarily denotes hexadecimal65 | Contract misread/unestablished premise | Existing decimal codepoint semantics must not silently become hex. Preserve compatibility; clarify future authoring instructions without changing frozen labels. |
| R4: remove canonical `unicode upper 65` | Contract misread | The review packet omitted the legacy canonical exception. Keep its exact public behavior and tests. |
| R5: canonical leading-zero refusal differs from natural decimal parsing | Valid documented trade-off | Different lexical contracts do not by themselves prove a wrong-ready bug. Preserve canonical strictness and existing natural decimal behavior. |
| R6: fabricated direction during operand validation | Valid structural concern | Introduce typed operand/direction evidence in the new slice; no demonstrated current key leak, and no unrelated rewrite. |
| R7: dataset name constitutes source/coverage approval | Contract misread | This is an untrusted routing field; downstream policy, evidence and native coverage remain authoritative. Do not change the proposal API. |
| R8: unclear domain/clarification precedence | Valid contract concern | Preserve existing control/OOD refusal precedence; supply complete precedence rules to any future label reviewer before freeze. |

Optional negation examples already abstain in the actual probe; preserve them.
No new per-family floor is introduced mid-experiment. Punctuation normalization
must not rewrite quoted punctuation, erase actions or accept arbitrary residue.

## Adopted bounded implementation

The two independent design outputs converge on a quote-aware, role-bearing
token/constituent parser for a small class of single-clause case requests, with
exact original operand evidence, bounded depth, full consumption and terminal
refusal after a construction is claimed. Legacy canonical and declaration/
reference paths stay compatible. Only an unclaimed construction may fall back.

Astra's second bounded run attempted that slice but stopped before any production
or test-source edit. Its nested sandbox failed on file writes, and the no-restore
test lacked build assets. [The exact handoff](astra-implementation-blocked.md)
records zero executed assertions, no `TASK_CONSTITUENT_RED`, and no green or full
regression run. Coordinator files were preserved. The requested outer limit was
1,200 seconds; the process exited before implementation, not after a successful
test run. The next slice needs coordinator-brokered patch/test execution and
independent changed-boundary review. Neither design agreement nor infrastructure
failure establishes fresh-language acceptance.

Hermes/Grok remains the requested third backend. Short inference works; long
streaming reliability failed twice. Four bounded batches then returned all 128
draft rows, with the requested 80/24/24 status counts and 40 ready rows per
operation. [Metadata-only validation](hermes-draft-metadata.json) rejected the
draft for one duplicate pair and four exact overlaps with exposed corpora.
All rows and original outputs remain retained; none was scored, relabeled or
silently dropped. Independent label review has not happened. This is a draft
admission failure, not another scored CNET quality failure.

All bounded model jobs have exited; the Herdr server remains attachable. No
background source implementation or automatic follow-up provider run is active.
No provider fallback occurred. Documentation checks (`asi_framing`, execution
tiers and diff whitespace) passed; production/test source remains unchanged.
Fresh acceptance remains WITHHELD until a valid independently reviewed corpus
and frozen candidate exist under all unchanged original floors.
