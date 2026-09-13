# Going after the answer path's refusals (2026-09-13)

Baseline (production registry, 812 capsules, sibling path on, k = 1, term gate on): on the 400 frozen teacher questions
of the answer-path record the path answers 41% (164), judged correct 77% of those (126), wrong 38, refused 59%; score
under the arena's cost rule (correct - 2 wrong) +50. On the 3,994 never-probed frozen questions it answers 36.2% (gold
32.4%, wrong 3.8%, score +992). This record asks where the 59% go and whether any of it can be answered without
lowering a certification floor. Judge: Mistral-Small-3.2-24B on the returned top passage, same protocol as before.

## Where the refusals go

Route level, 3,994 questions (`route-batch`): accepted 48.0%; refused by radius 0.6% (the gold capsule was top-1 in 20%
of those), margin 15.5% (31%), ambiguity 28.2% (49%), term 7.7% (75%).

Answer level, 3,994 questions (`answer-batch`): answered 36.2%; refused by the passage floor after an accepted route
19.2% (768); refused by the passage floor or the zgap on the sibling path 20.8% (831); margin 15.5% (619); term 7.7%
(306); radius 0.6% (25). The passage floor is the largest class: 40% of all questions reach a capsule and no passage
stands out by the capsule's calibrated z_min.

## Forcing the refused answers through (400 questions, judged)

| what is switched off | answered | judged correct of answered | wrong | net (c - 2w) | the extra answers alone |
|---|---|---|---|---|---|
| nothing (baseline) | 164 | 77% (126) | 38 | +50 | |
| margin, ambiguity and term gates (radius and floor kept) | 233 | 67% (155) | 78 | -1 | 69 extra: 29 right, 40 wrong (42%) |
| passage floor (route gates kept) | 236 | 64% (152) | 84 | -16 | 72 extra: 26 right, 46 wrong (36%) |

The floor-refused answers by how far the best passage fell below the capsule's floor (floor off, judged):

| bucket | n | judged correct | from the gold capsule | net (c - 2w) |
|---|---|---|---|---|
| accepted (z >= z_min) | 164 | 77% | 93% | +50 |
| z_min - 1 <= z < z_min | 45 | 44% | 91% | -30 |
| z_min - 2 <= z < z_min - 1 | 23 | 26% | 70% | -28 |
| z < z_min - 2 | 4 | 0% | 100% | -8 |

Reading: every refusal class is below the 67% break-even when forced. The calibrated floor sits exactly where judged
precision crosses it; the route gates refuse answers that would be 42% right. Nothing here is a floor to lower. The
telling number is the 91%: the floor-refused questions are on the right capsule; the capsule holds the answer and the
ranking does not find a passage that stands out.

## Margin rescue (measured, rejected)

The sibling path's passage-evidence logic applied to margin refusals as well (best and second capsule ranked, the pick
must clear its own floor by zgap over the other and pass the term gate), 3,994 questions:

| | answered | gold | wrong | score |
|---|---|---|---|---|
| baseline | 36.2% | 32.4% | 3.8% | +992 |
| + margin rescue, zgap 2 | 39.3% | 34.0% | 5.3% | +933 |
| + margin rescue, zgap 3 | 35.6% | 31.8% | 3.8% | +964 |

The 124 rescued answers split 63 gold / 61 wrong: 50% precision, below break-even. Margin refusals are right to refuse.
Removed after measuring; not a knob.

## A better passage ranker: lexical overlap (measured)

For every prompt the baseline routed to a capsule (317), the passage of that capsule with the highest idf-weighted
content-word overlap with the question was judged, against the VSA-best passage judged in the floor-off run:

| prompts | n | VSA-best judged correct | overlap-best judged correct | overlap right where VSA wrong / the reverse |
|---|---|---|---|---|
| all routed | 236 | 64% | 70% | 27 / 14 |
| accepted by the floor | 164 | 77% | 76% | 11 / 12 |
| refused by the floor | 72 | 36% | 56% | 16 / 2 |
| on the gold capsule | 213 | 69% | 75% | 24 / 13 |

Where the VSA ranking already stands out, overlap matches it; where the floor refuses, overlap finds the answering
passage 20 points more often. That is the lever: not a lower floor, a sharper statistic under the same floor.

## Hybrid score under the same calibration (simulated before any C change)

Simulation in Python of a hybrid passage score s_i = sim_i + lambda * overlap_i, where sim_i is the shipped int8
similarity (dumped from the CLI, `passage-sims`) and overlap_i the idf mass of the question's content words found in
passage i divided by the question's total idf mass (global idf over all 47,774 certified passages); z is computed
exactly as the shipped floor computes it (best against the capsule's other passages), and the floor is the quantile of
the best-passage z over 512 off-topic negatives per capsule, the quantile chosen per capsule so that lambda = 0
reproduces the shipped z_min as closely as the simulated negatives allow. Judged on the 317 routed prompts with the
judgments already in hand plus 20 new ones.

| lambda | answered | judged correct | wrong | precision | score (c - 2w) | gold-capsule share |
|---|---|---|---|---|---|---|
| 0 (VSA only, simulated floor) | 211 | 147 | 64 | 70% | +19 | 84% |
| 0.05 | 203 | 145 | 58 | 71% | +29 | 84% |
| 0.10 | 195 | 141 | 54 | 72% | +33 | 84% |
| 0.20 | 186 | 135 | 51 | 73% | +33 | 84% |
| 0.30 | 168 | 128 | 40 | 76% | +48 | 87% |
| 0.50 | 136 | 112 | 24 | 82% | +64 | 91% |
| shipped path (real floor) | 164 | 126 | 38 | 77% | +50 | 93% |

Two readings, kept apart:

- **The simulated calibration is looser than the shipped one.** At lambda = 0 the simulation answers 211 at 70% where
  the real floor answers 164 at 77%: the seal's negatives include the hardest ones (sibling capsules whose names start
  with the capsule's), which random sentences from other corpora do not reproduce, so the simulated floors sit lower.
  Absolute rows are therefore not comparable with the shipped path; the trend along lambda is.
- **The trend is monotone and large.** Every step of overlap weight raises precision and the net score (+19 -> +64
  from lambda 0 to 0.5) while lowering the number of answers (211 -> 136). Overlap sharpens the statistic: questions
  whose words are in the passage stand out more, off-topic negatives that share a stray word do not (the total-mass
  normalisation keeps a single shared rare word at a low score). Under the arena's cost rule the hybrid statistic
  dominates the VSA-only one at every operating point tried.

What it would take to ship: the overlap term needs a word idf that is identical at seal and at answer time, which the
lexicon already carries (`bin/registry.lex`, digest-pinned), a weight stored in the passage block so that a capsule is
answered with the statistic it was calibrated under (a v5 passage block), and a reseal of the 812 production capsules
so their floors are calibrated on the hybrid z with the seal's own negatives. Then the same 400-question judge run and
the 3,994-question hermetic run decide, with the real floors. Not done here: it is a format change and a full reseal,
and its expected effect is more precision at fewer answers, which is the opposite direction from the one this record
set out in.

## Conclusion

The refusals are right. Every class of refused answer, forced through, is judged correct well under the 67% the cost
rule needs (route gates 42%, the band just under the passage floor 44%, further down 26% and 0%); a margin-rescue
path measured at 50% precision and was removed. The capsule usually holds the answer (91% of floor refusals sit on the
gold capsule): the loss is in finding the passage, not in the gates. A lexical-overlap ranker finds the answering
passage 20 points more often than the VSA similarity exactly where the floor refuses, and a hybrid statistic under the
same calibration raises the net score monotonically in simulation, at the cost of coverage. That is the one lever
found, and it is a precision lever, not a coverage lever.

Not claimed: any shipped change to the answer path (nothing on the path moved; baseline +992 / +50 unchanged), the
hybrid's numbers under the real calibration.

## Verify

`make cnet_vsa_answer_bench` and `make cnet_vsa_router_bench` pass on the unchanged path. The scorer of the recurrent
LM record was unlinked from the CLI and the answer bench (the registry hook stays, gated with a stub scorer; the CLI is
back to 380 KB). New CLI diagnostics: `passages-dump <dir>`, `passage-sims <dir> <capsule>`,
`CNET_VSA_ANSWER_DUMP_SIMS=1`.
