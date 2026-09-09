# Contract-only review reconciliation

Cycle 1 was a blind label derivation from all 128 texts, with author labels
withheld. Claude's returned statuses, operations and original-input integers
matched the author's 128 labels exactly. This agreement is label evidence, not
a CNET score. Eighteen adversarial findings were separately reconciled below;
none was silently discarded. Corpus texts and labels are not included here.

| Finding | Classification and disposition |
|---|---|
| 1 | Contract misread. The full approved author protocol explicitly permits 0x hexadecimal operands, as does the review contract. The reviewer inferred an absent implementation restriction. No row changed. |
| 2 | Contract misread. Malformed quoting explicitly requires clarification; permission for unquoted letters does not permit ignoring broken quotes. No row changed. |
| 3 | Actionable linguistic uncertainty (`h3_10`). Same-backend author replacement requested before admission, preserving its operation and original input. |
| 4 | Conservative actionable repair (`h3_25`). Ambiguous direction is explicitly covered by the contract, but a clearer request was requested to remove any possible reading as sequential work. |
| 5 | Actionable linguistic uncertainty (`h4_21`). Replacement requested with explicit case intent and unspecified direction. |
| 6 | Conservative actionable repair (`h1_23`). Multiple possible operands are covered by the contract, but replacement requested to remove advice phrasing. |
| 7 | Contract misread. Common case-operation synonyms on one explicit scalar are in scope; a grammatical plural does not create an additional operand. No locale or whole-string request is present in the cited rows. No row changed. |
| 8 | Contract misread. The typed key is the original input scalar; the contract constrains the input range and explicitly separates native coverage. No row changed. |
| 9 | Valid quality concern, repaired despite passing exact-text uniqueness. Same-backend replacements requested for `h3_27`, `h3_28`, `h4_22`, removing three punctuation-only near-duplicate pairs. |
| 10 | Valid limitation retained. Multiple paraphrases can target the same operation/input pair. Row counts do not establish independent trials or statistical confidence. No acceptance denominator changed. |
| 11 | Valid limitation with scorer-boundary verification. Batches share structure and IDs correlate with categories. The coordinator inspected the fixed scorer/probe and confirmed that CNET receives only original UTF-16 code units, without IDs, family names or expected labels. The custodian did not inspect source. No IID or independent-item claim is made. |
| 12 | Valid reporting limitation. Raw author family names include aliases; raw and normalized metadata counts will be distinguished. Raw family count is not a claim of that many independent syntactic mechanisms. |
| 13 | Contract misread. The unchanged predeclared refusal floor is 23/24 across the abstain population. Subtypes are reported without replacing that denominator. |
| 14 | Valid coverage limitation retained. The draft does not exhaust every abstention trigger; absent surrogate, overlength and merely-quoted-instruction coverage cannot be inferred from its abstention score. Corpus input bounds remain unchanged. |
| 15 | Valid coverage limitation retained. Named-character guessing without a supplied literal is not directly exercised. No score from this corpus certifies that boundary. |
| 16 | Contract misread. There are no native-coverage exclusions from this proposal evaluation. The coordinator inspected the scorer and confirmed all 128 rows and original floors remain; the probe calls the proposal parser directly. |
| 17 | Transport concern, addressed by protocol rather than altered labels. Frozen corpus bytes and source/binary/probe/evaluator identities are hashed; strict corpus loading and original UTF-16 code-unit transport preserve literal inputs. The coordinator verified the transport boundary. No new execution is claimed by the custodian. |
| 18 | Expected review limitation, addressed by separate validation. Labels were intentionally withheld from the reviewer. The full labeled artifact separately passed the existing strict validator, and all independent derived labels were compared for exact agreement. Final replacements must pass those checks again. |

The coordinator's scorer-boundary confirmation is external to the custodian's
label review: it did not expose implementation, candidate outputs or new
confirmation results to the custodian. Replacements are solely pre-admission
contract/wording repairs, with every original row and failed request retained.

Cycle 2 independently derived all seven replacement labels correctly and
returned seven critiques. Every critique is individually classified in
`review-cycle-2-dispositions.json`; all are explicit contract misreads or
already-considered reporting/benchmark limitations. No linguistic uncertainty
remains after the custodian's own review. This is not a claim that Claude
returned no findings. The stop condition is reconciled findings, not a forced
empty response or a new blind retry.

Mechanical comparison confirms that the other 121 final text/family projections
are identical to those independently labeled in cycle 1. Every one of the 128
final labels matches independent derivation; no final row is unreviewed. The
final labeled file passes the existing validator, retains the required 80/24/24
population and 40/40 operation counts, and has no exact overlap against the 12
prior collections. Admission, frozen bytes and SHA-256 are recorded separately
in `admission.json`. No CNET execution occurred during admission.
