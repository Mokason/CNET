# Controller failure investigation — 2026-09-06

## Outcome

**The revised GPU-trained controller passes the preregistered 95%/95% research
hurdle on a new, bounded structural-holdout fixture. No production promotion.**
The original low score was not a hardware ceiling. Structured inputs and more
diverse independently verified training data materially improved the same small
FP32 network. This does not certify a deployed capsule controller.

On the **same 2,048 confirmation cases**, 1,284 goals were reachable and 764 were
unreachable. The three seeds reuse these graphs; do not count them as independent
datasets. The candidate had no access to confirmation graph signatures during
training. The historical comparator is not an equal-data or equal-budget control.

| Configuration | Reachable completion, seeds 1 / 2 / 3 | Mean | Unreachable abstention, seeds 1 / 2 / 3 |
|---|---|---:|---|
| Original recurrent4, 600 updates | 191 / 292 / 176 of 1,284 | 17.11% | 761 / 756 / 762 of 764 |
| Revised recurrent4, 30,000 updates | 1,261 / 1,267 / 1,254 of 1,284 | **98.18%** | 761 / 758 / 761 of 764 |

All candidate seeds exceed both unchanged floors: **97.66–98.68% completion** and
**99.21–99.61% abstention**. On the 794 paths requiring at least two edges, the
candidate completes 771 / 777 / 764 (**96.22–97.86%**). The direct-goal control
completes only 490/1,284 (38.16%). Immediate-legality masking changes neither
candidate nor historical-comparator completion here; it removes the comparator's
invalid attempts. The candidate makes zero illegal proposals on this fixture,
even without that mask. Verification remains enabled in every configuration.

The independent Floyd-Warshall/BigInt auditor checked **35,596 action records**,
observing **zero invalid acceptances**. Six deliberately corrupted trace/identity
variants are refused. Scores alone are not a certificate or a safety guarantee.

## Why the original experiment failed

Controlled development ablations use the original validation set (319 reachable
goals), never the old test set for training or selection. All rows below have the
same 13,961-active-parameter recurrent model, FP32 arithmetic, batch 128, learning
rate .15 and 6,000 updates. Encoding-only rows preserve input width and sampling.

| Intervention | Reachable completion, seeds 1 / 2 / 3 | Mean |
|---|---|---:|
| Raw edge/type matrices and absolute node positions | 79 / 93 / 91 | 27.48% |
| Precombine immediate edge and type compatibility | 112 / 106 / 118 | 35.11% |
| Also normalize current/goal coordinates | 293 / 292 / 290 | **91.43%** |
| Raw features, sample all nonterminal states on fixed graphs | 110 / 131 / 95 | 35.11% |
| Normalized features, sample all nonterminal states | 301 / 302 / 295 | 93.83% |
| Raw features, reduce abstain training frequency to 1/9 | 123 / 105 / 119 | 36.26% |

1. **Representation was the largest measured obstacle.** The flat model had to
   learn immediate type conjunctions and arbitrary current/goal placements along
   with path selection. The aligned encoder puts current at 0, goal at 7, remaps
   the remaining six nodes and labels exactly, and zeros unused padding. It
   contains no teacher distances, reachability, paths or proposed answers. This
   is endpoint normalization, not full permutation equivariance.
2. **Longer training alone overfits.** For raw seed 1 at 6,000 updates, the fixed
   training-probe loss is .719 versus validation 2.211. Better training fit did
   not yield comparable validation completion. All-state exposure helps, but
   the aligned fixed-data model still misses one validation floor at 30,000.
3. **Abstention imbalance contributes, but is not the main solution.** Unreachable
   labels occupy 3,144/8,192 (38.38%) of the original training set, whereas each
   individual node action receives roughly 7–8% target mass. Resampling helps
   the raw model somewhat but increases illegal proposals and does not approach
   the aligned model. Forcing action or suppressing abstention is not justified.
4. **Diverse evidence makes further training useful.** The final candidate sees
   3,840,000 accepted, freshly generated graph examples per seed with independent
   shortest-path labels, not CNET answers. Same model and optimizer; no added
   model capacity. This is a changed data regime, not a pure architecture test.
   Three seeds finish at 314 / 314 / 312 of 319 reachable validation tasks before
   any confirmation generation. The old `training_probe` label in retained
   streaming curves means the original fixed reference fixture, **not streaming
   training loss**; the current runner names it `fixed_reference_probe`.

These controls do not establish a universally superior optimizer, recurrent
architecture or learning procedure. Tied versus untied comparisons were not
rerun under the improved data regime. Uniform shortest-path ties also give a
nonzero target entropy; zero cross-entropy is not a necessary success criterion.

## Holdout correction and audit provenance

Fresh review found that hashing raw node-indexed adjacency could separate
node-renamed copies of identical aligned inputs. This was a demonstrated design
flaw, **not evidence that a measured overlap actually occurred**. Variant 6's
promising development results were rejected as confirmation evidence.

Variant 7 assigns data using the sorted multiset of joint (out-degree,in-degree)
pairs, FNV-1a 64-bit hashed modulo 5. Every node permutation preserves assignment.
Training rejects partition 0 and all 505 distinct validation signatures. The
confirmation generator uses partition 0 and rejects effective graphs appearing
in any original fixed split. Signature computation is only for dataset routing;
it never becomes a neural input or a label.

This is a **structural holdout**, not IID sampling from the unconditioned generator.
Some nonisomorphic graphs share a signature, and confirmation signatures can
repeat. No IID confidence interval or independent-seed sample inflation is
claimed. The old raw comparator may have trained on some confirmation signatures;
it is a historical paired comparator, not a matched structural-holdout control.

Replaying the training generator accepts 3.84 million of approximately 4.88
million draws per seed: approximately 989,000 partition refusals and 52,000
validation-signature refusals. Complete path-length counts and refusal counts
are in `data-profile.jsonl`. The scheme's coarseness changes the training
distribution; it is not graph canonicalization or proof of broader transfer.

Before confirmation existed, `freeze.json` fixed seeds, checkpoints, preprocessing,
floors, generator, retained evaluator binary/source hashes and exact invocation
mapping. The evaluator hashes the actual loaded weights; the audit binds this
to the frozen role and seed. Raw pre-mask proposals are traced and independently
checked. The training process never loads `test.tsv`; confirmation's exclusion
step reads old test topology only after selection. All accuracy processes apply
the existing offline syscall boundary after GPU initialization.

## GPU findings

Both AMD Radeon AI PRO R9700 / gfx1201 devices performed real FP32 training.
After warmup, 128 identical updates were compared against the scalar C reference,
with CPU/GPU order alternating across three repeats:

| Device | GPU update time, approximately | Speedup over scalar CPU, range |
|---|---:|---:|
| GPU 0 | 1.37 ms | 4.23–4.25x |
| GPU 1 | 1.66 ms | 3.52–3.55x |

Maximum differences after the sequence: weights **4.77e-7**, probabilities
**4.95e-6**, loss **2.15e-6**, all below the predeclared 1e-4 tolerance. No silent
CPU fallback is permitted. The CPU comparator is not optimized BLAS/multicore;
this does not prove either GPU beats the best available CPU implementation.
Both cards were shared with live workloads; no services, clocks or power limits
were changed. These are not exclusive-device peak-throughput measurements.

The current path remains inefficient: each recurrent update performs nine linear
kernels and two SGD kernels, with **35 synchronous host/device copy API calls**
and **1,825,472 transferred bytes** at this shape. Weights and intermediate
activations return to the CPU repeatedly. In the synthetic profiler run, 24
training updates plus one inference probe produced exactly 221 linear launches,
48 SGD launches and 855 `hipMemcpy` calls. `hipMemcpy` consumed 79.11% of recorded
HIP API duration, including synchronization/kernel waits—not 79% of application
wall time or proof of pure PCIe bandwidth saturation. The profiler process is
not the sealed accuracy process; it leaves profiler descriptors intact and
accesses no service or network data.

The highest-priority optimization is device residency and fewer synchronization
boundaries, then suitably batched ROCm matrix kernels. This follows AMD's guidance
to retain intermediate data on-device, consolidate transfers and use streams.
The existing `_dev` linear/SGD APIs and memory pool are starting points, but the
SGD device API still synchronizes and the present kernels are not WMMA GEMMs.
[AMD HIP performance guidelines](https://rocm.docs.amd.com/projects/HIP/en/latest/how-to/performance_guidelines.html).

## Research-supported next architecture

A structured, shared per-node/edge processor is a better next experiment than
simply enlarging the flat 8-node MLP. Neural graph-execution research trains
intermediate algorithm steps and compares graph aggregation choices; its results
support testing structure-aware processing and auxiliary reachability supervision,
not claiming that our model already has those properties.
[Neural Execution of Graph Algorithms](https://arxiv.org/html/1910.10593v2).

All-state sampling is not on-policy correction. A future teacher-labelled rollout
dataset should include states reached by the candidate itself, without using the
candidate's answers as targets. This addresses the sequential observation shift
studied by [Ross, Gordon and Bagnell](https://proceedings.mlr.press/v15/ross11a.html).
Expand beyond routing to separately frozen task families; the
[CLRS benchmark](https://arxiv.org/abs/2205.15659) is a useful source of independently
checkable algorithmic tasks. These are proposed experiments, not completed gates.

## Floating-point candidate to core or capsule

```text
independent verified evidence -> GPU-trained floating-point candidate
  |-> core checkpoint -> regression/calibration -> shadow trial -> guarded activation
  `-> bounded typed task -> verified examples/distillation -> existing BTN contract
       -> robust certificate + coverage -> existing capsule export/import -> admission
```

There is no generic lossless conversion of arbitrary neural weights into certified
capsules. Quantization changes numeric representation, not knowledge provenance,
coverage or correctness. Core weights and capsule weights also need not be the
same architecture. A large candidate can supply proposals for an independently
verified distillation dataset; its predictions alone cannot become certified truth.

Reuse `cnb_export_subset`, `cnet_capsule_export`, the CNU1 capsule manifest and
`cnet_capsule_core_validate_growth`. Existing `teach` fits a bounded BTN and
enforces robust margin .05; its explicit `finite_domain_compile` fallback is
enumeration, **not evidence of GPU neural compression**. MTK `.tskill` cartridges
remain matching-host tensor deltas, not portable contract-certified capsules.

The concrete, acceptance-gated product sequence is in
[`../plans/cnet_gpu_candidate_product_sequence_20260906.md`](../plans/cnet_gpu_candidate_product_sequence_20260906.md).

## Verification, evidence and limits

Encoding/label-remapping tests cover 700 current/goal permutations, the concrete
raw-partition counterexample, and masking that preserves unreachable abstention.
The test was RED before missing encoder/mask/signature helpers were implemented.
The current encoding suite also passes AddressSanitizer/UndefinedBehaviorSanitizer.
Fixture, numerical-gradient/overfit, local boundary and both-device parity suites
pass. Mutation tests reject false acceptances, checkpoint substitution, inflated
raw-illegal totals, completion inflation, missing actions and altered distance
buckets. Production files and original experiment sources remain unchanged.
Fresh retraining in a directory containing only training/validation inputs
reproduced **all three selected checkpoint files byte-for-byte**. The corrected
source's extra trace/probe naming has no effect on training updates.

Evidence directory: `cnet_controller_investigation_evidence_20260906/` contains
the development curves (including rejected variants), frozen confirmation inputs,
raw traces/audit, selected and historical native checkpoints, source snapshots,
GPU timing/profile files and SHA-256 manifest. Native checkpoints and the retained
gfx1201 executable are research artifacts, **not a public serialization format or
portable capsules**. Follow `experiments/offline_controller/INVESTIGATION.md` to
audit or rerun them using the installed C/HIP, Node and OpenSSL development tools.

This passes a fixed eight-node routing fixture. There are only 16 five-hop and
four six-hop confirmation paths, and no seven-hop paths; performance on long
chains is still weak and sparsely measured. Variable capsule counts, actual
contract-bound capsules, unseen task families, learned repair, live acquisition,
checkpoint hot-swap and unattended promotion were **not** established here.
Broader capability claims remain **WITHHELD**. No master push or live deployment
was performed in this investigation.
