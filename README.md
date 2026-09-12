# CNET — certified, composable specialist intelligence

> **CNET builds ASI — Artificial Specialized Intelligence. In CNET, ASI never
> means Artificial Superintelligence. CNET is not claiming AGI. Broad semantic
> grounding exists to understand intent; deep competence comes from isolated,
> certified, portable Micro Tensor Kernels. Wikipedia-like accumulation may
> broaden coverage and composition over time, but unit count alone is not
> intelligence and all broader claims remain benchmark-gated.**

CNET is a C11 runtime for learned specialists with typed interfaces, explicit
coverage and executable certification. It assembles verified units into plans,
checks every handoff, and abstains when available evidence does not support an
answer. External teachers and approved tools can supply new evidence; they do
not grant themselves serving authority.

Start with the [documentation index](docs/INDEX.md). This repository contains
runtime components, experimental model paths and historical research. Their
presence is not evidence of deployment or a passed capability gate.

## Quick start

From the repository root on Linux, with GCC, GNU Make and Bash:

```sh
make verify-fast
make knowledge_capsule coverage_abstain
make capsule_core capsule_tool
```

These run the short edit-loop checks, exercise capsule transfer and coverage
refusal, and build the local capsule tools. No private model or GPU is needed
for this capsule path. Optional integrations have separate prerequisites; see
[build and verification](docs/BUILD_AND_TEST.md).

Use [the capsule guide](docs/CAPSULE_CORE.md) for a private-registry example with
independently supplied labels. Do not point experiments at a live base.

## How the parts fit

```text
input → typed intent → verified inventory → candidate plan
                                         → per-hop audit, contract and coverage
                                         → verified answer or abstention
uncovered request → independent evidence → train → certify → guarded publication
```

- A **specialist** is a planner-visible implementation admitted under a contract.
- A **certified capsule** carries a sealed unit, typed ports and its coverage gate.
- An **MTK cartridge** (`.tskill`, CMSK/MTSK) carries host-model tensor deltas.
  It is not a certified capsule and requires a matching host model.
- The **core** selects and executes specialists; scores never replace certification.
- The **residual/teacher lane** handles uncovered work under explicit policy.
  CNET's own Tier-A answers must never become its training targets.

See [architecture](docs/ARCHITECTURE.md), [dispatch](docs/dispatch.md) and
[execution boundaries](docs/EXECUTION_TIERS.md).

## Current implementation boundary

The deterministic capsule runtime supports up to 4096 capsules. Memory use
depends on the units and runtime state, not just serialized payload size.
The ordinary daemon retains request-pinned capsule inventories. Owner-only
named-set control supports same-process upgrade/switch, durable selection,
restart and rollback; failed candidates retain the incumbent. See the
[operator sequence](docs/CAPSULE_CORE.md#daemon-integration-and-swaps).
The [source-evidence adapter](docs/CNET_SOURCE_EVIDENCE.md) serves five exact
local literal facts with source freshness checks; it is not unrestricted code
understanding or a compiler-proof engine.
The [private learning supervisor](docs/AUTONOMOUS_LEARNING.md) can acquire
owner-authorized finite numeric tables and exact-token/text-label tables into
the same certified capsules. Symbolic vocabulary is policy-pinned; actual text
observations, source freshness, probation and rollback remain mandatory.
The pinned Unicode examples cover explicit case changes and 95 printable-ASCII
names mapped to each of two property labels, not unrestricted text learning.
See the [adapter/GPU/scale verification report](result/cnet_domains_gpu_scale_20260907.md)
for measured results and explicitly unqualified scope.

The opt-in GPU/core experiment implements resident AMD FP32 training, isolated
two-device workers, immutable core checkpoints, certified BTN/capsule conversion,
shadow checks, request-pinned activation and rollback. Its neural graph adapter
admits at most 62 capsules plus two endpoint nodes. It does not replace the
deterministic default or constitute a live deployment.

The September 6 measured training workload was 2.65× faster on GPU0 and 1.69× on
GPU1 than four-thread OpenBLAS. The tiny neural serving path was slower than
deterministic serving. These are separate workloads on shared hardware;
FP16/BF16 did not clear the unchanged numerical floor. Read the
[GPU guide](docs/GPU_TRAINING.md) and
[bound experiment results](result/cnet_gpu_product_sequence_20260906.md).

## Measured against transformer embeddings

A frozen routing arena (`make vsa_routing_arena`, fixture and hashes under
`benchmarks/vsa_routing_arena_20260911/`) scores the VSA topic router against
transformer embedding models on one protocol: 1068 teacher-written corpora,
held-out sentences, 5,258 teacher-written questions, seeded negatives. On that
task the learned lexicon over 2 KB int8 centroids matches a 137M-parameter
embedding model on ranking, exceeds a 4B-parameter one on the separability the
router gates on, and costs microseconds on a CPU instead of milliseconds on a
GPU; the 4B model leads on ranking accuracy by about nine points, and that loss
is a claim the gate checks too. Blending the 4B model's word vectors into the
lexicon offline (distillation, runtime unchanged) lifts separability and
question top-3 but not that gap (`result/cnet_vsa_lexicon_distillation_20260911.md`).
Learned phrase entries, learned subword backoff, and a supervised pass that
pairs each capsule's own sentences and teacher questions with its centroid
(lexicon v3, still a static int8 table, about 5 microseconds per query) take
teacher-question routing past the 4B model on the frozen arena while the
held-out sentence loss stays recorded
(`result/cnet_vsa_lexicon_phrases_subwords_training_20260912.md`). Retraining on
mixed-register paraphrase families with contrast pairs extends that lead to
the everyday register those families were written in, but everyday sets from
writers it never saw reverse it by 5 to 12 points, and training on those
writers' registers does not recover it: the advantage is bound to the content
of the training pairs, not their register
(`result/cnet_vsa_lexicon_register_training_20260912.md`). Capsule text
generation itself was measured once: on-topic seven-word phrase runs with no
sentence structure, never answering the prompt, against 85% for the capsule's
own passage returned verbatim; answers come from retrieval, not generation
(`result/cnet_vsa_generation_coherence_20260912.md`). Capsule format v4
carries those passages, digest-covered, and `cnet_vsa_cli answer` routes a
prompt and returns the winning capsule's best passages under a calibrated
floor in about 60 microseconds (`result/cnet_vsa_answer_path_20260912.md`).
The Isekai/RPG corpora have no certifiable epoch yet: measured and rejected
twice (`result/cnet_vsa_isekai_epoch_20260912.md`). The production
registry was resealed under the v3 block and the shipped lexicon
(`bin/registry.lex`, auto-activated by every registry load): on the
never-probed teacher questions wrong accepts fell from 35% to 1.2%, with a
term-dependence gate that refuses any route a single word could flip
(`result/cnet_vsa_registry_reseal_20260912.md`). Generation,
answer correctness and open-ended language are not compared. See
`result/cnet_vsa_routing_arena_20260911.md`.

## Verification and evidence

```sh
make verify
make capability_cert
make knowledge_accumulation_bench
make knowledge_composition_bench
make own_learning_health
make -C experiments/offline_controller product-test  # requires AMD ROCm
```

Capability receipts bind a particular source/worktree state. Do not edit source
or evidence concurrently with certification; capture console output outside the
worktree. A historical PASS is not a current release certificate.

Dated results live in [result/](result/), frozen protocols in
[benchmarks/](benchmarks/), and decisions in [plans/](plans/). Never rewrite a
failed result as a pass. The [generated claims ledger](docs/verified-today.generated.md)
is generated by `make claims`; its filename does not prove freshness.

## Safety and release status

Never lower certification floors to admit a candidate. Missing guards, corrupt
payloads, incompatible ports and exhausted budgets refuse. Supported GPU
development is AMD/ROCm; legacy vendor-specific experiments are not the product
path.

The control-plane SQLite provider has been migrated and its loaded native
version tested. Managed scriptlets now use a bounded Linux process sandbox;
the sample server has separate inference/admin authentication and resource
controls. These focused repairs are not an exhaustive security clearance;
see [security](docs/SECURITY.md). Live rollout remains separately authorized.

Nothing here authorizes a service restart, registry replacement, remote push or
public release. Follow [release policy](docs/RELEASE_POLICY.md).

## Contributing and retained data

Read [AGENTS.md](AGENTS.md). Add a failing regression before changing behavior,
implement a bounded slice and preserve evidence. Keep unrelated dirty files out
of commits. Operating guides belong in `docs/`; decisions belong in `plans/`.

Root `*_weights.txt`, `*_contract.txt` and `*_property.txt` files are fixtures or
demonstration inputs. English windows, derived `.words.txt` sidecars, goldens and
`pdf_corpus.txt` have active or provenance-bound consumers. They are not
disposable prose. [Documentation maintenance](docs/MAINTENANCE.md) explains
retention and recovery.

## License

CNET-authored source and documentation are Apache-2.0 under [LICENSE](LICENSE),
unless a file states otherwise. Licensing does not authorize changing repository
visibility or publishing private artifacts.
