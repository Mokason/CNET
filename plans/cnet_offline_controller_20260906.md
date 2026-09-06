# Offline controller experiment

Status: experiment only; deployment and broader capability claims WITHHELD.

## Frozen protocol

Question: do four internal state updates improve next-hop proposals on unseen
eight-node synthetic typed graphs? These are capsule-routing fixtures, not
certified capsules. No admission, daemon, packaging or live-memory changes.

Compare a one-update network, a four-update tied-weight recurrent network, and
a four-layer untied feedforward network. The latter two have identical forward
matrix operation counts but different parameter counts. Each action starts from
zero state; all models see the same adjacency/current/goal inputs and have eight
action attempts. Training uses an independent shortest-path oracle with uniform
labels over every equally short next hop. The final verifier only checks edges,
type compatibility, termination and arrival; it does not consult that oracle.

Freeze generated graph sets before training: 8,192 training, 512 validation,
512 test tasks; deterministic seed per split, reject duplicate graph topologies
across splits. Three initialization seeds, 600 minibatches of 128 examples per
architecture; learning rate 0.15. No test-driven tuning. Report loss, optimal
action accuracy, full-path completion, invalid proposals, unreachable abstention,
length buckets, rejection recovery, latency and forward FLOPs. A negative result
is a valid outcome. A deterministic shortest-path solver is an explicit control.

GPU 1 performs the training matrix operations; GPU 0 evaluates frozen copies.
Activations and orchestration remain on CPU. CPU reference, finite-difference
gradients and CPU/GPU parity must pass first. GPU errors fail loud, never silently
become CPU success. Tiny offload-floor override is experiment-local and disclosed.
The candidate process denies network syscalls after device initialization and
checks for inherited network sockets. A loopback-only local-model comparison on
the first 32 held-out cases is separate; its already-running server is not
claimed to be network-isolated. No local-model output becomes a training label.

## Slices and acceptance

1. Fixtures and verifier: RED then tests for ties, unreachable goals, incompatible
   edges, invalid actions and false arrival; frozen split hashes.
2. Trainable core: RED then finite-difference gradient checks, loss reduction,
   unchanged frozen copy, CPU/GPU forward and update parity on both devices.
3. Runner: all three models, all seeds, frozen held-out results and shared local
   baseline subset; record real metrics without changing success floors.
4. Fresh review, rerun regression tests, save source and evidence on the isolated
   experiment branch. No automatic merge or promotion.
