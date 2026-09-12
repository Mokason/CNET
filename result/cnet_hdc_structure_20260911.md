# Structural HDC: measured experiment, 2026-09-11

The experiment supports explicit role encoding and specialized factor recovery. It rejects the tested coupled solver as an improvement over direct unbinding, and does not establish fixed-cycle success as codebooks grow. On the controlled relational contract, an ordinary exact solver is faster and more accurate than either HDC prototype. Broad transformer superiority is WITHHELD: no transformer was included in the scored benchmark.

This experiment changes no production source, certification threshold, capsule, or live crawler behavior. Earlier uncommitted production edits remain separate. The three capsule safety findings in `cnet_vsa_v3_review_20260911.md` are outside this experiment and are not resolved here.

The reproducible driver is `experiments/hdc_structure/run.py`; methodology, algorithms, and commands are in `experiments/hdc_structure/README.md`. Raw outcomes are in the adjacent `.json`; inputs were frozen in `.fixtures.json` before scored execution.

## Directed composition and refusal

360 procedural fixtures, 32 entities, four relations, 1/2/4 hops, 8/32/96 distractors, unique/reversed/missing/ambiguous/contradictory families. Exact traversal independently implemented in Python supplies gold; the C exact lane agrees on all fixtures. There are 137 valid unique answers and 223 required refusals. The development seed is disjoint; no parameters were fitted. Shared vector seeds and repeated conditions mean these are descriptive sample results, not independent estimates of population accuracy.

| Method | Correct valid answers / 137 | Wrong accepted / 360 | Correct refusals / 223 | Median warm query |
|---|---:|---:|---:|---:|
| Exact signed-fact traversal | 137 (100.0%) | 0 | 223 | <1 µs |
| Production topical router, synthetic fact entries | 20 (14.6%) | 18 | 208 | 3.81 µs |
| Direct structural HDC | 135 (98.5%) | 225 | 0 | 4.94 µs |
| Direct HDC + exact verification | 135 (98.5%) | 0 | 223 | 5.00 µs |
| Coupled HDC, 8 cycles | 132 (96.4%) | 228 | 0 | 1063.45 µs |
| Coupled HDC + exact verification | 132 (96.4%) | 0 | 223 | 1062.81 µs |

Both raw HDC prototypes always propose an entity. All 223 invalid cases therefore receive a wrong answer without verification; this is an explicit missing capability, not an abstention success. The verifier solves the entire contract itself and rejects disagreeing proposals. Direct HDC plus verification still loses two valid answers. No credit for perfect refusal belongs to the HDC proposal alone.

The production topical encoder makes `Alice feeds Bob` and `Bob feeds Alice` byte-for-byte identical at 2048 int8 coordinates. Distinct entity IDs do remain distinct. The topical row is a diagnostic of applying a router to relational answer selection, not the quality of certified CNET capsules: entries are synthetic, radius is 0.9, and production margin/ambiguity defaults are retained. The structural lanes receive explicit subject/object roles.

The synchronous prototype also shows the cost of propagation. On the same 24 unique four-hop cases, accuracy by cycle budget is:

| Cycles | Correct / 24 | Median query |
|---|---:|---:|
| 1 | 3 | 322.1 µs |
| 2 | 6 | 643.8 µs |
| 4 | 18 | 1290.5 µs |
| 8 | 21 | 2574.9 µs |

The 1/2/4-cycle rows use one timing sample per fixture; other rows use three warm samples. This local synchronous update scheme does not transmit arbitrary path information instantaneously. It is one prototype, not a lower bound for every possible HDC architecture.

## Opaque three-factor recovery

1,728 scored conditions sweep 4/16/64 entries per factor, two vector seeds, 0/10% coordinate sign flips, iteration caps 4/16/32, and four engine/width combinations. Each table cell below contains 18 present-factor cases; six absent-factor cases per condition test refusal. Repeated caps, lanes, and timing repeats do not create additional independent products.

Clean products, maximum 32 cycles; entries show exact, accepted factor tuples:

| Engine | Width | 4 candidates/factor | 16 candidates/factor | 64 candidates/factor |
|---|---:|---:|---:|---:|
| Production, native bipolar keys | 512 | 18/18 (100.0%) | 8/18 (44.4%) | 1/18 (5.6%) |
| Production, paired bipolar keys | 512 | 18/18 (100.0%) | 2/18 (11.1%) | 0/18 (0.0%) |
| Prototype, sign + outer-product cleanup | 512 | 18/18 (100.0%) | 11/18 (61.1%) | 1/18 (5.6%) |
| Prototype, sign + outer-product cleanup | 2048 | 18/18 (100.0%) | 16/18 (88.9%) | 4/18 (22.2%) |

Production `cnet_vsa_random` generates normalized bipolar floats, not Gaussian vectors. The two production rows use different deterministic codebooks. Lane 1 and the 512-dimensional prototype share exact vectors, isolating the cleanup algorithm. Different widths share the procedural seed but not identical codebooks.

The prototype improves this small sample at 16 candidates: 512-dimensional paired recovery rises from 2/18 to 11/18; widening reaches 16/18. But 64 candidates remains unreliable: 4/18 at 2048 dimensions. At that size, clean recovery is 3/18 at 4 cycles, 3/18 at 16, and 4/18 at 32. Increasing the cap does not guarantee escape from an unsuccessful state. With 10% sign flips, 2048-wide recovery at 16/64 candidates is 15/18 and 5/18; the small noise-related increase at 64 is not evidence that noise generally helps.

No wrong acceptance occurred in this finite factor sweep, under either native acceptance or the additional reconstruction check. Most difficult cases fail by non-acceptance at the iteration cap. Exact IDs and acceptance are recorded separately. Production convergence floors remain >0.85 during updates and >0.70 after the cap; the separately reported reconstruction check is ≥0.75. No threshold was tuned to test results.

The existing `make cnet_vsa_reason_bench` remains green: 20/20 products, average two iterations, all five checks pass. Inspection shows those 20 products are only five distinct index triples repeated four times, using five entries per book. Its 4×4 puzzle is solved with recursive backtracking. This gate establishes its bounded mechanisms, not broad fixed-cycle constraint satisfaction.

## Time and resources

CPU only, pinned to CPU 0, GCC 13.3, `-O3 -march=native`. Query timings are native monotonic-clock intervals and include timer overhead; the exact solver is too short for a precise standalone nanosecond claim, so the table reports <1 µs. No GPU, power, or energy measurements were made. Python dispatch is outside timed native queries. Topical routing includes text query encoding; structural inputs are already typed. Fact parsing and world construction are separate.

| Clean factor case, cap 32 | Correct / 18 | Median solve on valid cases | Logical codebook bytes |
|---|---:|---:|---:|
| Production paired, 16/book | 2 | 398.3 µs | 101,376 |
| Prototype 512, 16/book | 11 | 92.2 µs | 24,576 |
| Prototype 2048, 16/book | 16 | 150.5 µs | 98,304 |
| Production paired, 64/book | 0 | 1409.3 µs | 405,504 |
| Prototype 2048, 64/book | 4 | 7176.3 µs | 393,216 |

The 2048-wide int8 factor books have approximately the same logical key storage as 512-wide float books. This is not proof of equal total resource use: float projection scratch, transient allocations, duplicate generation buffers, and codebook setup must also count. At 64 candidates the wider prototype is substantially slower despite better recovery.

The relational structural representation stores 128 KiB of entity keys plus 16 KiB of int16 relation memories (144 KiB total). Explicit verification facts add 128–1616 logical bytes; the harness actually reserves a fixed 8 KiB edge array inside an 8,256-byte World. Its full production registry allocation is 18,546,736 bytes (fixed 4,096-entry capacity), not an optimized compact fact store. These are different quantities from one capsule's 2 KiB centroid.

Median world key/memory construction was 411.7 µs; synthetic topical-entry construction was 451.4 µs. The complete Python/native process peaked at 57,544 KiB RSS, including fixtures and duplicate experiment buffers; this is not a per-engine footprint.

Compiler stack reports: coupled update frame 26,560 bytes; common factor-trial harness frame 49,792 bytes; production factor function adds a 25,088-byte frame on its call path. Logical codebook storage alone omits these. The factor harness retains both byte and float generation arrays even when only one form is needed by the solver.

For n candidate entities, d dimensions and h hops, direct unbinding scans approximately hnd coordinates. The tested coupled updates scan approximately Thnd coordinates for T cycles. Three-factor outer-product cleanup scans approximately Td(Mx+My+Mz) coordinates. Fixed T therefore does not mean fixed total work as dimensions, domain size, or graph depth change. More parallel hardware can change latency, but its resource cost must be measured.

## Verification, decision, and limits

- Default-encoder role-collision RED diagnostic reproduced; distinct entity encodings checked. Finite grammar parser exactly recovered 34,608 active/passive sentences; unsupported grammar refuses. This proves those templates only.
- Independent C/Python oracle agreement on 45 development and 360 scored fixtures; four singleton factor sanity cases pass.
- AddressSanitizer and UndefinedBehaviorSanitizer smoke test passes at 128 entities/codebook entries, 512/2048 dimensions, and present/absent factors.
- Existing factor/puzzle gate passes unchanged. No claim that the new experiments are certification gates.
- Review corrections: use the exported default encoder in the role check; correctly label native random vectors as bipolar; disclose synthetic registry entries, verification-as-solver, duplicate buffers, and correlated cases. No production fixes are implied.

For the next implementation, use typed signed facts and exact specialized operators when the contract admits them. Retain centroid routing for capability selection. Direct structural HDC is a candidate associative proposal mechanism; the tested coupled solver should not replace it. Keep the bipolar outer-product factor prototype as an experiment: its small-codebook gains justify more tests, not deployment or a universal advantage claim.

WITHHELD: arbitrary structural analogy, unknown-rule induction, unrestricted semantic grounding, open-ended synthesis, scaling beyond this sweep, GPU speed, energy, and superiority over transformers. Testing those capabilities does not require retaining transformer endpoints, but declaring a comparative win will require a comparable measured baseline or a valid published evaluation protocol.

Fixture SHA-256: `17aa4bd7c77a2b15381ae4c47babd09bf71b0d472457a7a9add6cdff0596dc17`. Native/source hashes and every prediction, latency, reconstruction, iteration count, and logical storage value are retained in the raw JSON. All work remains uncommitted.
