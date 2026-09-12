# Certified answer path: v4 passages, route then rank (2026-09-12)

Gate: `make cnet_vsa_answer_bench` (CNET_VSA_ANSWER_BENCH_PASS). Registry bin/ resealed to v4 (812 capsules, same
812/12 outcome as the v3 reseal; digests re-pinned nowhere: the lexicon is unchanged, tag 0xbde45edf).

## What was built

- **Capsule format v4**: `CnetVsaPassageBlock` after the topical block, digest-covered: the sentences the capsule was
  sealed from (NUL-separated, up to 128 passages / 32 KB; 812 capsules hold 31 to 88, median 59, none truncated),
  plus a within-capsule answer floor. File grows 33,320 bytes (5,600,640 -> 5,633,960). v1 to v3 files load as
  before; a v3 capsule answers NO_PASSAGES.
- **Answer floor, calibrated like the radius** (`cnet_vsa_gencap_calibrate_passages`): for each of the 512
  off-topic negatives the z of its best passage over the capsule's passages is noise; z_min is the 90% quantile
  of those (median 3.78, range 3.20 to 4.60), floored at 1.0. The training-question probes are measured against it
  and recorded (68% clear it). A first version calibrated from the probes themselves gave floors up to 12 and
  refused real questions: probes written from a sentence nearly quote it, real questions do not.
- **Answer path** (`cnet_vsa_registry_answer`, CLI `answer` / `answer-batch`): route (radius, margin, ambiguity,
  term gates), then rank the winner's passages against the query in the same int8 space and return the top k
  (<= 4) with the best passage's z; refused when z < z_min. Passage vectors are built on a capsule's first answer
  and kept (about 60 x 2 KB per capsule touched). `CNET_VSA_PASSAGE_ZMIN` overrides the floor for measurement.

## Latency (production registry, 812 capsules with passages)

| | route | rank (56 passages) | total per answer |
|---|---|---|---|
| warm, same prompt repeated (bench binary) | 52 to 54 us | 4 to 7 us | 57 to 61 us |
| cold (first answer for a capsule) | | builds ~60 passage vectors | ~400 to 700 us once |
| hermetic 2-capsule registry, 48 passages | | | 11 us warm, 250 us cold |

The compact retriever measured earlier in Python sat at 330 us p50; this path is the same idea in C at a fifth of that.

## Speed, end to end, on the production registry (812 capsules, 43 MB lexicon, single thread)

The bench binaries are built with gcc -O3 -march=native; the CLI was built by hipcc -O3 WITHOUT -march=native, so its
int8 centroid scan was not vectorised for the host and every CLI-measured latency today was about 3x too slow
(194 us per route where the same code routes in 59 us). mk/vsa.mk now passes -march=native to hipcc. Measured
after the fix with the C path's own timers over the 3,994 frozen questions, one process, registry loaded once:

| | p50 | p95 |
|---|---|---|
| route, refused at a gate (2,077 queries) | 32 us | 46 us |
| route, accepted (1,917; includes the term gate's re-encodes) | 56 us | 82 us |
| passage rank, warm (56 passages) | 6 us | |
| first answer for a capsule (builds its ~60 passage vectors once) | 410 us | |
| any query, route + rank | 46 us | 444 us (the p95 is first-hit builds) |
| throughput on fresh queries, cold caches included | 8,900 queries/s | |
| steady state, every touched capsule warm | ~16,000 queries/s | |

Bare centroid scan, 812 dots of 2048 int8: 37 us; it is the floor of every route and scales linearly with the
registry. Registry load: 2.1 s for 4.3 GB of capsule files (read + digest of every file); resident memory 111 MB idle,
191 MB after answering across 726 capsules (their passage vectors cached). For comparison: the Python compact
retriever measured 330 us p50; an embedding transformer costs milliseconds per query on a GPU.

## Quality (Mistral judge: does the returned text answer the question; same protocol as the generation record)

400 frozen teacher questions (third per capsule, never calibration probes), one per capsule:

| floor | answered | judged correct (of answered) | correct (of all 400) | wrong (of all 400) | refused |
|---|---|---|---|---|---|
| none (`CNET_VSA_PASSAGE_ZMIN=0`) | 33.2% | 62% | 20.5% | 12.8% | 66.8% |
| **sealed (negatives 90%)** | 19.8% | **76%** | 15.0% | **4.8%** | 80.2% |
| fixed z >= 3 | 26.8% | 70% | 18.8% | 8.0% | 73.2% |
| fixed z >= 4 | 17.5% | 80% | 14.0% | 3.5% | 82.5% |

Correctness by the best passage's z (floor off): z < 2: 0% (n=4); 2 to 3: 32% (22); 3 to 4: 51% (37); 4 to 5: 77% (30);
5 to 7: 81% (21); 7+: 84% (19). The sealed floors sit at the knee. Under the arena's cost rule (a wrong answer costs
two right ones) the sealed floor scores +5.4 against -5.1 with no floor and +7.0 at a fixed z >= 4; it is kept
because it is calibrated per capsule with a receipt rather than a constant. Returning two passages did not help the
judge (73% vs 76%): the second sentence is usually about something else; k = 2 stays available, the answer is P1.

On the 100 prompts of the generation record (first frozen question per capsule): 15 answered, all 15 judged
correct, 0 wrong, 85 refused (63 at the route, 22 at the floor). Generation on the same prompts: 0 of 71 correct.

## Reading

The answer path is precise and fail-closed: when it answers it is right three times in four on single
questions, and never from a wrong capsule under the floor (2 of 79 from a sibling). What it lacks is coverage:
two thirds of single questions are refused at the ROUTE, before any passage is considered. That is the operating
point of the routing gates, not the answer path, and is the next step.

## Operating point (step 2): the ambiguity constant was the coverage cost

Where the 3,994 never-probed frozen questions were refused at the route under the defaults (k = 2, term gate on):
ambiguity gate 49.2% (and in 61% of those the top capsule was the right one), margin gate 15.5% (31% right),
term gate 4.0% (88% right), radius 0.6%. Sweep of the two runtime knobs on the production registry:

| ambiguity k | term gate | accept | correct | wrong | abstain | score (correct - 2 wrong) | aliens accepted |
|---|---|---|---|---|---|---|---|
| 0 | on | 70.5% | 54.7% | 15.8% | 29.5% | +922 | 0/12 |
| 0 | off | 83.9% | 62.8% | 21.0% | 16.1% | +830 | 2/12 |
| **1** | **on** | **48.0%** | **43.2%** | **4.8%** | **52.0%** | **+1341** | **0/12** |
| 1 | off | 55.7% | 48.9% | 6.7% | 44.3% | +1416 | 2/12 |
| 1.5 | on | 38.3% | 35.9% | 2.4% | 61.7% | +1247 | 0/12 |
| 2 (old default) | on | 30.6% | 29.4% | 1.2% | 69.4% | +1077 | 0/12 |
| 3 | on | 19.3% | 18.9% | 0.4% | 80.7% | +726 | 0/12 |

Calibration targets are not the lever: the radius gate refuses 0.6% of questions at 0.80/0.90, and raising the
accept target to 0.90 or 0.95 refuses 122 and 345 capsules as not separable (their remaining questions then route
at 53.9% / 60.2% accept, on a smaller registry). 0.80/0.90 stays.

Chosen: k = 1 with the term gate on, now the compiled wide-space default. It gives up 3.6 wrong-accept points
against k = 2 for 13.8 more correct routes, keeps every alien out, and keeps the single-word hole closed (k = 1
without the term gate scores 75 more but admits 2 of 12 aliens). Answer level at the new point, 400 questions:
answered 34% (from 20%), 76% of those judged correct (unchanged), 26% correct over all questions (from 15%),
8% wrong (from 4.8%); under the cost rule +10 against +5.4.

## Verify ladder (step 4)

The VSA gates were not in `make verify`; they only passed when someone ran them. Now:
- `mk/vsa.mk` holds every VSA target (the root Makefile was over its line budget after this work; it is 8,494
  lines again and the ceiling was lowered to that).
- T1 (`make verify`): the hermetic gates, capsule format (`cnet_vsa_gencap_bench`), calibration, lexicon, answer
  path, q8 parity, stemmer, each with a ledger row in tests/verify_logs.sh so a missing or stale log fails.
- T2 (`make verify-t2`): the gates that need the production registry or minutes (router bench on bin/, CLI bench,
  hermetic arena, encoder sweep, the frozen transformer arena).
- One pre-existing integrity failure fixed on the way: src/cnet_vsa_hybrid.c guarded its curl include with a form
  the curl guard does not recognise. `make build_integrity` passes with the tier sync at 37 T1 / 26 T2 targets.

`make verify` (T1, 37 targets) passes with the six VSA gates in the run-bound log ledger: "all 34 suites reported
success in THIS run". The frozen arena and the registry-dependent gates run under `make verify-t2`.

WITHHELD: answer correctness beyond the judge's yes/no; multi-sentence answers; anything against a language model
answering the same questions.
