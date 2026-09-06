# Offline controller experiment — 2026-09-06

## Decision

**Do not promote this controller.** The two-device training/evaluation mechanism
works, but the small recurrent network does not beat its matched-compute
feedforward control consistently. Keep the deterministic planner and verifier;
use the existing local model as a proposal source where useful. Broader
capability and real capsule integration remain WITHHELD.

The preceding source checkpoint was pushed to `origin/master` as
`08c9cebb7a7aa6ce4a6f416d8455900be989ada6`; its fresh source verification passed
all 28 suites. This experiment is separate, on
`experiment/offline-controller-20260906`, with no live deployment.

## Results

Frozen test set: 512 distinct synthetic graph topologies, 291 reachable goals
and 221 unreachable goals. Three initialization seeds reuse that same test set;
they are not 1,536 independent test graphs. Scores below are verified complete
paths, not fluent answers or self-grades.

| Model | Active parameters | Matrix FLOPs per action | Completed / 291, seeds 1 / 2 / 3 | Mean completion |
|---|---:|---:|---|---:|
| Single update | 9,865 | 28,832 | 38 / 61 / 30 | 14.8% |
| Four recurrent updates | 13,961 | 109,088 | 40 / 64 / 39 | 16.4% |
| Four untied layers | 49,993 | 109,088 | 46 / 65 / 34 | 16.6% |
| Deterministic planner control | — | Not compared | 291, one run | 100% on this fixture |

The local Mistral Small 3.2 24B Q5_K_M baseline completed **15/20 reachable**
tasks on the first 32 test cases. On exactly that subset, single-update completed
2/2/0, recurrent completed 1/3/2, and feedforward completed 2/2/0 across seeds.
Mistral also produced 50 illegal proposals across 125 attempts, all rejected, and
abstained on 7/12 unreachable cases. It is useful as a proposer, not an authority.
This local-model comparison is not FLOP- or latency-matched to the tiny networks.

The networks overproduce abstentions: optimal-first-action accuracy is roughly
42–43%, while always abstaining scores 221/512 (43.2%) on that metric. Complete
path results are therefore the more informative outcome. No held-out tuning was
performed after inspecting these results.

## What actually ran

- Eight-node directed graphs, explicit edge compatibility, current and goal
  features; these are routing fixtures, **not certified CNET capsules**.
- 8,192 training, 512 validation and 512 test topologies, with no overlap; source
  generator and SHA-256 hashes are retained. Each architecture saw the same
  sampled examples for a given seed: 600 batches of 128, learning rate 0.15.
- Standard tanh state updates trained by full backpropagation through all four
  steps. Tied gradients accumulate before any weight update. This is a custom
  experimental controller, not a claim of a new neural architecture.
- Discrete GPU 1 trained; discrete GPU 0 evaluated frozen copies. Both were
  identified as AMD Radeon AI PRO R9700 / gfx1201. Host code computes activations,
  labels and control flow; the existing native AMD math code handles matrices.
  Training and evaluation were sequential phases, not a concurrent service test.
- Recurrent and untied models execute identical forward dense matrix operation
  counts; their parameter counts differ. All models have eight action attempts.
  The evaluator executes eight full batches even for already finished cases.
  Reported batch-amortized times (~0.013 ms single, ~0.043 ms four-step) include
  logging and bookkeeping, **not standalone request latency** or a GPU speedup.
- The small-GEMM FLOP offload threshold was explicitly overridden for this
  experiment; the library's minimum tile dimensions were retained through
  padded output matrices. This is not a production offload recommendation.
- Training labels came only from an independent shortest-path algorithm; no
  CNET answers, local-model outputs or live memory were used as labels.

## Verification and boundaries

- Independent BigInt edge/type checks and an all-pairs planner audited **22,118
  neural action records**: zero observed false acceptances. A deliberately forged
  acceptance is rejected by the audit regression. These figures concern these
  synthetic traces only; they do not certify an unrestricted controller.
- CPU finite differences sampled 72 weights across all architectures; maximum
  absolute gradient discrepancy was below 4.5e-5. CPU/GPU forward and SGD parity
  on both devices was below 6e-8. Loss-reduction, frozen-copy, malformed-response,
  inherited-socket refusal and network-denial tests passed. The CPU network test
  also passed AddressSanitizer/UndefinedBehaviorSanitizer.
- Forced first-action failures were rejected without changing final completion
  counts. Recovery here is the deterministic rejection mask, **not learned
  repair**, capsule swapping, or evidence of solving a new domain.
- The neural process denies network syscalls after GPU initialization, checks
  inherited sockets, and applies its syscall filter across threads. Local-model
  clients may use only their preconnected loopback:8092 socket; reconnects and
  new sockets are denied. The already-running model server is **not claimed to
  be network-isolated**. Its raw-completion baseline is constrained to a single
  valid action digit per request and receives verifier rejections as an action
  mask, matching the neural proposal interface.
- A fresh isolated rebuild reproduced the action records and weight checkpoint
  bytes. Native research checkpoints remain local under `/tmp`; they are not
  portable capsules or a second capsule packaging format.

## Reproduce and inspect

From the experiment worktree:

```sh
bash experiments/offline_controller/run.sh --local-baseline
```

Requires the installed HIP toolchain, two compatible AMD GPUs, Node.js for the
independent audit, and the existing local completion server on port 8092 for the
optional baseline. No server is started, stopped, or reconfigured. Omitting
`--local-baseline` runs the complete neural comparison and deterministic control
without using that server. Artifacts are created in a fresh temporary directory.

Evidence: [`cnet_offline_controller_evidence_20260906/audit.json`](cnet_offline_controller_evidence_20260906/audit.json),
raw action records, fixed datasets, test logs and hashes in the same directory.
Implementation: `experiments/offline_controller/`; protocol:
`plans/cnet_offline_controller_20260906.md`.

## Remaining product scope

No daemon hook, resident capsule swapping, new domain competence, unattended
training service, promotion workflow or deployment was implemented here. A next
controller trial should compare structured graph/capsule features against this
baseline on newly frozen held-out families before changing any serving path.
