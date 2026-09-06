# Controller investigation tools

These are isolated research programs, not production APIs. Original `run.c`,
`net.c`, capsule packaging, master and live services are unchanged. Results and
limits are in `result/cnet_controller_investigation_20260906.md`.

## Audit retained evidence

From the experiment worktree root:

```sh
node experiments/offline_controller/audit_investigation.mjs \
  result/cnet_controller_investigation_evidence_20260906 \
  result/cnet_offline_controller_evidence_20260906
node experiments/offline_controller/test_investigation_audit.mjs \
  result/cnet_controller_investigation_evidence_20260906 \
  result/cnet_offline_controller_evidence_20260906
```

The auditor checks the retained frozen sources/binary/checkpoints, actual loaded
checkpoint hashes, graph disjointness, structural partition, all action transitions,
aggregate counts and distance buckets. The mutation suite accepts the valid trace
and refuses six corruption classes. It creates a small scratch directory under
`/tmp` and does not modify retained evidence. The root `SHA256SUMS` also binds the
confirmation fixture, raw traces and audit results; verify with `sha256sum -c`.

## Build and reproduce selected training

Requires installed C/HIP gfx1201 tools, AMD GPU 1, Node.js, and OpenSSL headers/
libcrypto for SHA-256 in the confirmation loader. No Python/MAX/model server is
required. The small-GEMM offload floor override is explicit and experiment-local.

```sh
make -C experiments/offline_controller test investigate-test gpu-test \
  investigate-build investigate-confirm investigate-benchmark investigate-data-profile
```

Create a fresh directory with only the retained `train.tsv` and `validation.tsv`.
Do not place old `test.tsv` or `confirmation.tsv` there. Run:

```sh
/tmp/cnet-controller-build/investigate 7 1 30000 > seed1.jsonl
/tmp/cnet-controller-build/investigate 7 2 30000 > seed2.jsonl
/tmp/cnet-controller-build/investigate 7 3 30000 > seed3.jsonl
```

The resulting `diagnostic-aligned_structural_stream-N-30000.weights` should match
the retained `checkpoints/candidate-seedN.native-weights` byte-for-byte on this
toolchain/hardware. A different ROCm/compiler may have numeric differences; test
parity and task scores rather than silently treating different bytes as certified.
The native `Net` structure is deliberately not a portable or public file format.

Variant IDs: 0 raw, 1 immediate typed adjacency, 2 endpoint-aligned adjacency,
3 raw/all-states, 4 aligned/all-states, 5 raw/abstain-resampled, 6 rejected raw-ID
stream partition, 7 corrected structural stream partition. The runner accepts
seeds 1–3 and at most 30,000 updates. Variant 6 is development history only; do
not use it for the documented confirmation result. Original streaming logs label
the fixed reference probe `training_probe`; current logs correct that name.

`bench_train DEVICE UPDATES REPEATS` runs identical synthetic CPU/GPU sequences
after warmup, alternating order across repeats. Use `0 128 3` and `1 128 3` for
the reported shape. This process leaves profiler descriptors intact, unlike the
sealed accuracy processes, and performs no network/service calls.

```sh
rocprofv3 --stats --hip-trace --kernel-trace --memory-copy-trace \
  --output-directory /path/to/new-profile-directory --output-format csv -- \
  /tmp/cnet-controller-build/bench_train 1 16 1
```

`profile_data` replays the selected training generator without a network, reading
`validation.tsv` in its working directory. It reports refusal and path-length
distributions for all three fixed training streams.

## Fresh confirmation protocol

Do not use an already observed confirmation set to select more configurations.
For the recorded run, `freeze_investigation.mjs` was called **before** confirmation
generation. It requires all three final structural-stream validation results to
meet both .95 floors and writes new files exclusively, retaining evaluator and
checkpoint identities. Arguments are development directory, original baseline
weight directory, repository root and compiled confirmation binary. The historical
baseline files must be named `recurrent4-seedN.native-weights` and must be the
original 600-update checkpoints.

The frozen evaluator then generated `confirmation.tsv` with `confirm freeze
ORIGINAL_EVIDENCE_DIRECTORY`. Every exact evaluation argument vector is retained
in `freeze.json` under `invocations`. Each uses the immutable copied `bin/confirm`,
candidate or historical checkpoint, declared variant/seed/mask, and confirmation
split. The concatenated stdout is `confirmation.jsonl`; the audit consumes it.

Final source differs from the captured training source only in probe naming and
additional trace instrumentation; `training-source-v7.c` preserves the run's
training implementation. Makefile additions for later data profiling do not
change the retained evaluator. The frozen `source/` directory is an evidence
snapshot, not a second maintained implementation.

## Review record

Systematic debugging selected controlled ablations instead of interpreting a
single accuracy number. Performance profiling separated CPU/GPU parity from speed.
Three bounded fresh-context review cycles led to explicit imbalance/mask controls,
the permutation-invariant split correction, checkpoint identity binding, audited
pre-mask proposals and corrected streaming-probe terminology. Final review found
no remaining blocker in that audit boundary. Claude/Grok were available as CLIs;
no external review was invoked while exact-command confirmation was outstanding.
