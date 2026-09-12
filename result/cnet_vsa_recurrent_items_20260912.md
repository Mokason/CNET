# Two transfers from recurrent memory models, measured (2026-09-12)

From plans/cnet_vsa_recurrent_memory_analysis_20260912.md: item 1, the delta rule as the capsule's generative
memory; item 2, mixture-of-memories style top-2 answering. Both in C, nothing removed; gates:
`make cnet_vsa_delta_bench` (new, in T1) and `make cnet_vsa_answer_bench`.

## Item 1: delta-rule capsule memory (`src/cnet_vsa_delta.c`)

What: a d x d matrix memory S (d = 512, the n-gram engine's dimension) written by the delta rule
S += beta (v - S k) k^T from the capsule's own sentences, keys being the engine's permute-and-bind trigram
contexts and values the next-word vectors; derived from the v4 passages, never stored. Read: v_hat = S k, scored
against the vocabulary by cosine. Selectable at generation (`CNET_VSA_GEN_MEMORY=table|bundle|delta|both`).

Capacity (hermetic, random bipolar keys, 512-d): 256 pairs recall top-1 256/256 after one pass and reach cosine
0.99 after 8 passes; the Hebbian bundle of the same pairs ranks its own value first for 26 of 256. At n = d the
system is near-singular: top-1 stays 100%, cosine converges slowly. So the math holds: the erase term removes
the crosstalk the bundle drowns in.

Next-word recall on production capsules (98 capsules; in-sample = the capsule's own sentences; held-out = the
two arena sentences never sealed, capsules rebuilt from train rows for that measurement):

| memory | in-sample top-1 / top-5 (86,693 positions) | held-out top-1 / top-5 (1,856) | read per position |
|---|---|---|---|
| explicit table scan (shipped) | 84.5 / 96.8 | **25.2 / 67.2** | 302 us |
| delta-rule matrix | 83.9 / 94.6 | 17.3 / 27.1 | 292 us |
| Hebbian bundle (shipped, secondary) | 2.6 / 4.1 | 4.4 / 6.0 | 126 us |

Generation (96 in-domain prompts, 60 tokens; gemma4 perplexity; Mistral pairwise, both orders must agree):

| memory | perplexity | vs default |
|---|---|---|
| default (table + bundle) | 724 | |
| table only | 844 | 15 : 15, tie 71% |
| table + bundle + delta | 884 | 22 : 18, tie 60% |
| delta only | 1,644 | 9 : 40 |

Reading. The delta rule is exact where the bundle is useless, thirty times better on the same pairs, and that
part of the analysis was right. But the capsule's explicit table is the data itself, so in-sample nothing beats
it, and on unseen contexts the scan's soft accumulation over every stored key (cosine > 0.2) generalises far
better than a least-squares point read (top-5 67% against 27%). The read is not cheaper either: scoring the
vocabulary costs as much as the scan. Generation gets no better and delta-only gets worse. Verdict: the delta
memory stays as a measured optional mode and a gate; the table remains the generative memory. The bundle, at
2.6% recall, is the one component the numbers say is dead weight, and it is left in place as promised.

## Item 2: sibling answering (mixture-of-memories top-2 activation)

What: when the ambiguity gate refuses a route between two capsules that both passed the radius and margin
gates (the largest refusal class), `cnet_vsa_registry_answer` ranks the passages of BOTH, takes the better one
under its own calibrated floor, requires its z to beat the other capsule's best-passage z by `sibling_zgap`
(the passage-level ambiguity gate), and runs the term gate on the pick. Knobs `CNET_VSA_ANSWER_SIBLINGS`,
`CNET_VSA_SIBLING_ZGAP`. Aliens: 0/12 accepted with the path on (none of them reach it).

Route level, 3,994 never-probed frozen questions (answered = a passage returned; gold = from the right capsule):

| sibling path | answered from gold | from a wrong capsule | score (gold - 2 wrong) | sibling picks gold / wrong |
|---|---|---|---|---|
| off (previous default) | 26.7% | 2.1% | +898 | |
| on, zgap 0 | 36.2% | 6.5% | +926 | 380 / 177 |
| on, zgap 1 | 34.9% | 5.4% | +961 | 327 / 133 |
| **on, zgap 2 (new default)** | **32.4%** | **3.8%** | **+992** | 228 / 68 |
| on, zgap 3 | 30.5% | 2.9% | +988 | 152 / 32 |

Judged (Mistral: does the passage answer the question), 400 questions:

| sibling path | answered | judged correct of answered | correct of all | wrong of all | refused |
|---|---|---|---|---|---|
| off | 34% | 76% | 26% | 8% | 66% |
| on, zgap 0 | 50% | 72% | 36% | 14% | 50% |
| **on, zgap 2** | 41% | **76%** | **31%** | 10% | 59% |

Reading. Sparse top-2 activation converts a third of the ambiguity refusals into answers. Without a passage-level
margin the picks are only 68% from the right capsule and the judged precision drops; with the margin at two
standard units the picks are 77% right, judged precision is back at the main path's 76%, and the net is five more
points of correct answers for two more of wrong ones, +992 against +898 under the arena's cost rule. It is now
the default. The mixture idea transferred because it was applied where the system already had a mixture (the
registry) and a measure to gate it with (the calibrated passage floors), not as a learned router.

Speed: unchanged for accepted and refused routes; an ambiguity-refused query now costs one more passage
ranking (about 6 us warm, a first-hit build once per capsule).

WITHHELD: any claim that a learned recurrent model was tested; items 3 and 4 of the analysis are not started.
