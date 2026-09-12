# Fail-closed arena and HD encoder (2026-09-11)

Gates: `make cnet_vsa_arena_bench` (CNET_VSA_ARENA_BENCH_PASS, 5/5).
Existing gates re-run green: calibration, gencap, router.

This replaces the broken 824-row table (capsule names as queries, covered
topics counted as aliens, accept read as success).

## Scoring rule

`score = n_correct - 2 * n_wrong_accept`. Abstain = 0.
Wrong answers cost more than silence. A transformer on the same questions
must be allowed to abstain. That comparison is WITHHELD until a QA endpoint
exists; :8084 is a story mouth and the 27B teacher wrote the gold.

## Hermetic arena (`make cnet_vsa_arena_bench`)

Neighbor pair shares unigrams (`thermal coating fails under … during quench`)
and differs by one collocation (`shock` vs `expansion`).

| encoder | in correct | in wrong | OOD wrong | score | shock–expand gap |
|---|---|---|---|---|---|
| bag (word-hash) | 6/6 | 0 | 0 | 6 | 0.200 |
| HD (char-trigram + bound bigram) | 6/6 | 0 | 0 | 6 | 0.236 |

HD collocation sim (`thermal coating shock` vs `thermal coating expansion`):
bag 0.652 → HD 0.595. Paraphrase retrieve of a sealed sentence: sim 0.699
(not an exact stored string; not 1.000).

## Live registry, bag v1 capsules (48-capsule slice, corpus sentences, not names)

Tool: `tools/cnet_vsa_route_eval.py --dir bin --limit 48`. Queries: last two
corpus sentences (probes=0 on this tree). OOD: 10 hand aliens after dropping
`quantum` and `thermal` as covered-topic.

| set | n | new accept | correct | wrong | abstain | top-1 self | score |
|---|---|---|---|---|---|---|---|
| in-domain | 96 | 0.531 | 0.281 | 0.250 | 0.469 | 0.375 | -21 |
| ood | 10 | 0.000 | 0 | 0 | 1.000 | - | 0 |

Margin gate: OOD 10/10 refuse. In-domain is still a neighbor problem on the
bag encoder (28% correct, 25% wrong-accept). Live capsules are version-1 bag;
new seals default to HD (`gencap-create --encoder hd`). Mixed registries encode
the query twice.

Live vs-transformer slice (24 questions, route-to-self as correct): CNET
6 correct / 10 wrong / 8 abstain, score -14. Transformer lane WITHHELD.

## What this does not claim

- Beating a transformer on language. Not measured.
- Generated n-gram text quality. WITHHELD.
- Full 824-capsule table under the new query rule (slice of 48).
- HD encoder on the live 824 (those files were sealed with bag hashes).

Next measured lever: reseal from `var/distill` with `--encoder hd` and re-run
`cnet_vsa_route_eval.py` without `--limit`. Do not touch radius targets until
that sweep exists.

---

SUPERSEDED 2026-09-11 (later): "new seals default to HD" no longer holds. The
registry-wide sweep (result/cnet_vsa_encoder_default_20260911.md) measured HD at
5.5% separable vs BAG 51.1% at 0.80/0.90 and 10x the encode cost; the default is
now STEM, selected by `make cnet_vsa_encoder_sweep_bench`. The hermetic table above
stands as a mechanism test only.
