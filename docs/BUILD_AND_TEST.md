# Build and verification

Run commands from the repository root unless a command uses `make -C`.
Use a private development worktree. Build/test artifacts are not permission to
replace a running shared library or service.

## Prerequisites and portability

The native build uses GCC/G++, GNU Make, Bash, libm and pthreads. libcurl is
optional and detected by the Makefile; `CNET_NO_CURL=1` tests explicit HTTP
unavailability. Individual gates also use utilities such as `jq`, `rg` and
Node.js. Managed gates require the SDK selected by the .NET project files and
a successful package restore. Private-model gates require named local assets.

`PORTABLE=1` disables host-specific ISA tuning; `PORTABLE=v3` selects x86-64-v3.
The default is host-native. Do not copy a host-native binary to a different
machine and assume it is compatible.

ROCm/HIP, supported AMD hardware and access to the GPU device nodes are required
only for GPU targets. The current worker experiment targets `gfx1201` and Linux
Landlock ABI >=3. OpenBLAS is an optional benchmark dependency, not a dependency
of the deterministic capsule runtime.

## Choose the gate you need

| Command | Purpose |
| --- | --- |
| `make verify-fast` | Short edit-loop tier |
| `make verify` / `make test` | Source/core regression tier |
| `make verify-t2` | Specialty and extended CCE checks |
| `make verify-long` | Longer verification tier |
| `make unified` | Native/.NET/MCP CPU integration and generated claims |
| `make capability_cert` | Declared capability manifests and evidence integrity |
| `make capsule_core capsule_tool` | Build local capsule tools/library |
| `make knowledge_capsule coverage_abstain` | Capsule transfer and coverage refusal |
| `make knowledge_accumulation_bench knowledge_composition_bench` | Bounded knowledge mechanisms |
| `make own_learning_health` | Refuse unsafe unattended-learning configurations |

Gate membership is defined in [mk/verify_tiers.mk](../mk/verify_tiers.mk), not in
this table. Build fragments are indexed in [mk/README.md](../mk/README.md).
A missing optional dependency is not a PASS and should not trigger silent
substitution of a different backend.

## GPU product experiment

```sh
make -C experiments/offline_controller product-test
make -C experiments/offline_controller test gpu-test investigate-test
make -C experiments/offline_controller resident-test rocblas-test stream-test
```

The product target builds its prerequisites and runs in a new private temporary
directory. `OUT=/absolute/private/build-directory` overrides its output path.
It tests both discrete devices and only private registry activation. It does not
reserve GPUs, alter clocks, reset devices or interrupt existing jobs.
[The experiment guide](GPU_TRAINING.md) explains what each workload measures.

## Fresh evidence

Source-bound gates deliberately refuse if tracked or untracked state changes
during execution. Finish edits first, run these gates sequentially, and capture
console output outside the repository:

```sh
make capability_cert > /tmp/cnet-capability-cert.log 2>&1
```

Inspect the exit status and receipt, including source identity and failures.
Do not copy an old PASS marker into a new report. `make claims` runs its
verification scope and regenerates [the claim ledger](verified-today.generated.md);
`make claims_all` inventories available logs and is not fresh-run certification.

## Documentation and release checks

```sh
make asi_framing execution_tiers_doc_gate license_metadata_test claims_test
```

The documentation framing gate preserves the existing project doctrine.
Historical protocols and failed evidence are not edited to satisfy a claim.

`make release_integrity` is the separate clean-tree release authority. It is
not interchangeable with a successful build or `make verify`; read
[release policy](RELEASE_POLICY.md). Do not run release cleanup over an unrelated
dirty worktree.
