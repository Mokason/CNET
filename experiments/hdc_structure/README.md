# Structural HDC experiment

This is an isolated research harness. It does not change production routing,
certification, capsule formats, or the live crawler. No model endpoint is used.

Run from the repository root:

```sh
bash experiments/hdc_structure/build.sh
python3 experiments/hdc_structure/run.py --self-test
python3 experiments/hdc_structure/run.py
```

Requires a C11 compiler and Python 3 on Linux. Only standard libraries and existing
CNET sources are used. The build uses CPU SIMD with `-O3 -march=native`; no GPU is
used. The driver pins itself to the lowest available CPU. Output defaults to
`result/cnet_hdc_structure_20260911.{fixtures.json,json}`; override the stem with
`--out`. The fixture manifest is written and hashed before scored execution.

## Relational contract

There are 32 entities, four directed relations, and positive/negative facts.
Follow the supplied sequence of 1, 2, or 4 relation IDs from the start entity.
Return the unique reachable endpoint. Refuse if there is no endpoint, more than
one endpoint, or a reachable positive edge also occurs as a negative fact.
Contradictions outside the reachable path do not trigger refusal. This is an
explicit experimental contract, not a universal interpretation of negation.

There are 360 test fixtures: three depths × three distractor counts (8/32/96) ×
five case families × eight replicates. Reversed edges can introduce additional
walks; the independent oracle determines gold even when a family's name suggests
otherwise. Distractor nodes are disjoint from path nodes. This is deliberately a
small controlled test, not arbitrary graph structure or unrestricted language.
45 separate development fixtures check implementation correctness, with no
parameter fitting. Test fixtures use a different procedural seed. Two vector
seeds are shared across cases, so outcomes are correlated, not 360 independent
estimates of real-world accuracy.

Lanes:

- `exact`: C traversal of explicit signed facts, checked against a separate Python
  set-based oracle. Scans facts and scans negative facts for conflicts; no index.
- `topical`: current production int8 encoder and query router, one synthetic fact
  per registry entry, STEM encoder, radius 0.9, production ambiguity/margin defaults.
  Certification flags are synthetic fixtures, not actual certified capsules. The
  query is start entity plus relation sequence; the returned entry's object is the
  proposed answer. This is a diagnostic of topical retrieval applied to a task it
  was not designed to solve, not a measurement of actual capsule capability.
- `direct`: independent int8 bipolar subject/object keys, an int16 superposition
  per relation, forward unbinding and exhaustive entity cleanup at every hop.
- `coupled8`: experimental synchronous soft candidate updates along the chain,
  eight cycles, softmax inverse temperature 4 and backward weight 0.5. This is not
  the production three-factor resonator. Unknown entities start uniformly.
- `*_verified`: same proposal followed by the exact solver; refuse if they disagree.
  Verification therefore solves this entire task itself. Its time and fact storage
  are charged; a perfect refusal score must not be attributed to HDC alone.

Warm query latency is the median of three native measurements after warm-up.
Lane order is shuffled deterministically per fixture. Python/ctypes overhead and
world construction are excluded from solve latency; encoding/build costs are
recorded separately. The 1/2/4-cycle diagnostic uses one measurement per fixture
on the 72 unique-case subset; do not compare its aggregate accuracy to the full
360-case eight-cycle aggregate without filtering the latter to the same subset.

A finite active/passive grammar parser checks two surface forms per fact. It is a
handwritten parser, not evidence of learned semantics or general paraphrase
understanding. Reversed-role centroid equality is a baseline failure diagnostic
(`HDC_ROLE_COLLISION_RED`), not a repaired production behavior.

## Factorization contract

Recover all three factor IDs from their elementwise bound product. Each factor
has a separate codebook. The suite varies 4/16/64 entries per book, 512/2048
dimensions, two key seeds, 0/10% independent coordinate sign flips, and iteration
caps 4/16/32. Each condition has 18 present-factor cases and six cases in which
the first factor is absent from its book. These are repeated conditions, not
1,728 independent products. Three timing repeats do not count as extra examples.

- Lane 0: existing production resonator, native `cnet_vsa_random` bipolar float
  keys, 512 dimensions. The native RNG is bipolar, **not Gaussian**.
- Lane 1: same production resonator with the exact bipolar codebooks used by lane
  2 at 512 dimensions; use this lane for matched-vector algorithm comparisons.
- Lane 2: experimental bipolar outer-product cleanup with a sign nonlinearity,
  stored int8 keys, float projection accumulators, asynchronous factor updates,
  decoded reconstruction acceptance ≥0.75. Runs at 512 and 2048 dimensions.

Production thresholds remain >0.85 during iteration and >0.70 at the cap. A common
additional reconstruction check ≥0.75 is reported separately from production
acceptance. Count exact IDs independently of acceptance. Absent-factor acceptance
is always wrong. Finite zero-error observations are not a safety guarantee.

All codebook generation and composite construction are outside measured solve
latency. Reconstructing the decoded tuple for checking is inside. Logical book
bytes include key arrays and production name storage, but exclude scratch and
experiment-only duplicate generation buffers. The combined Python/native process
peak RSS is recorded separately and cannot be attributed to any single lane.
Generated `.su` files expose native stack frames. This prototype does not establish
that a complete structural store fits in one 2 KB centroid.

## Scope

Measured: controlled directed composition, abstention under the stated contract,
opaque product factorization, CPU latency, encoding cost, logical storage.
WITHHELD: unrestricted language, structural analogy between arbitrary graphs,
unknown-rule induction, open-ended synthesis, GPU performance, energy, asymptotic
constant-time guarantees, broad transformer superiority.

Native sanitizer smoke test (maximum entity/codebook count, both vector widths,
present and absent factors):

```sh
cc -std=c11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude \
  -o /tmp/cnet-hdc-sanitize experiments/hdc_structure/sanity.c \
  src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_memory.c \
  src/cnet_vsa_ngram.c src/cnet_vsa_gen_capsule.c src/cnet_vsa_reason.c -lm -pthread
/tmp/cnet-hdc-sanitize
```
