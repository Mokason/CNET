# Isekai/RPG corpora: no certifiable epoch yet (2026-09-12, step 5)

937 Isekai/RPG corpora (var/distill_isekai, 858 with crawler probes) had no capsules and no lexicon epoch. Two
options were built and measured; neither ships.

## Setup

- Option A, epoch 1: seal the Isekai corpora under the production table (bin/registry.lex, technical vocabulary
  only), negatives from both corpus sets, and add them to the technical registry (var/regA, 1,420 capsules).
- Option B, epoch 2: a combined lexicon over both corpus sets with the v3.2 recipe (14,617 words + 8,192 phrases,
  distilled technical words, training on the v2 questions plus 16 sentences per corpus of both sets, 778 s), then
  every capsule resealed under it (var/epoch2/reg, 1,691 capsules).
- Isekai calibration probes: the crawler probes minus their last two lines; those two lines per corpus (1,118 under
  A, 1,584 under B) are the held-out routing queries. Technical queries: the frozen teacher questions as before.

| registry | technical: accept / correct / wrong / score (3,994 q) | Isekai held-out: accept / correct / wrong / top-1 self / score | aliens |
|---|---|---|---|
| bin (epoch 1, technical only) | 48.0 / 43.2 / 4.8 / +1341 | | 0/12 |
| A: epoch 1 + 608 Isekai (329 not separable) | 49.6 / 43.8 / 5.8 / +1284 | 19.2 / 10.9 / 8.3 / 32.0 / -64 | 0/12 |
| A at k = 2 | 32.9 / 31.4 / 1.5 / +1134 | 8.5 / 4.8 / 3.7 / 32.0 / -28 | |
| A at k = 3 | 21.5 / 21.1 / 0.5 / +803 | 2.8 / 1.6 / 1.2 / 32.0 / -8 | |
| B: epoch 2, 822 technical + 869 Isekai (68 not separable) | 45.3 / 38.0 / 7.3 / +943 | 56.0 / 30.7 / 25.3 / 44.4 / -316 | 0/12 |

## Reading

- The combined vocabulary makes the Isekai corpora separable at seal time (869 vs 608) and routes them more
  often, but a quarter of their held-out probes are accepted by the WRONG capsule, and the technical block loses
  six points of correct routes and gains wrong ones. Epoch 2 is rejected.
- Under epoch 1 the technical block is intact (one cross-set wrong accept in 3,994) but the Isekai block is
  negative at every operating point: the encoder puts the right capsule on top for only 32% of held-out probes,
  and no gate can fix ranking. The corpora are trope variants of one another (the sibling structure the frontier
  dedup note has flagged since the first day), written in a register the lexicon has not learned.
- Decision: bin/ stays epoch 1 with the 812 technical capsules; no Isekai capsule enters production. The staging
  registries stay under var/ for the next attempt, which needs (1) an Isekai lexicon epoch trained on ISEKAI
  question pairs, not sentences alone, and (2) a frontier that merges or distinguishes sibling tropes before
  sealing. A separate Isekai registry with its own registry.lex is the deployment shape when that arrives.

WITHHELD: any Isekai routing claim; Isekai answer quality.
