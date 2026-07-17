# Runtime Provenance and Gap-Service Unification Plan

> **For Hermes:** Use subagent-driven-development skill to implement this plan task-by-task.

**Goal:** Close two live seams in CNET's self-improvement loop: make the local gap-lane service fail safely when its ignored daemon artifact is absent, and project complete CNB artifact/runtime provenance through SoulHost, .NET, and MCP.

**Architecture:** Preserve existing authorities. The systemd unit remains a local deployment descriptor and never starts a model as part of a build; a condition turns an absent executable into a clean skip, while a preparation target only builds and validates. CNB remains provenance authority; add a new scalar SoulHost accessor rather than changing the existing C ABI signature, then expose the value unchanged through the managed descriptor and MCP JSON.

**Tech Stack:** C11, GNU Make, systemd unit syntax, Python unittest for portable static config validation, .NET P/Invoke/C#, xUnit/MCP integration tests.

**Place:** The seam between the certified gap ledger and the local teacher daemon, and the seam between persisted Oracle identity and external observability.

**Dilemma:** The core loop is gated, but an ignored daemon binary can disappear while an enabled service restart-loops; linked-runtime identity is persisted yet hidden above native CNB.

**Social consequence:** A self-improving system that cannot explain its actual teaching runtime—or repeatedly fails before teaching—cannot earn operational trust.

**Systematic variables:** executable presence, unit condition ordering, persisted digest bits, ABI return codes, managed JSON field names.

**Irreducible variables:** model quality and GPU driver/kernel numerics. These are not exercised or claimed in this CPU-only slice.

---

### Task 1: Gap-service fail-safe deployment tracer

**Objective:** An absent `bin/gap_lane_run` must cause a clean systemd condition skip rather than `203/EXEC` restart churn; preparation must build/test without starting the model.

**Files:**
- Modify: `config/cnet-gap-lane.service`
- Create: `tests/test_gap_lane_service_config.py`
- Modify: `Makefile`
- Modify: `docs/ARCHITECTURE.md`

**Step 1: Write failing tests**

Assert the tracked unit contains `ConditionFileIsExecutable=` matching the exact `ExecStart` executable, that the condition is in `[Unit]`, and that `make gap_lane_service_config` validates the file. Add a `gap_lane_service_prepare` target depending on `gap_lane_run_build` and the config gate; it must not invoke `systemctl` or execute the daemon.

**Step 2: Verify RED**

Run: `python3 -m unittest tests/test_gap_lane_service_config.py -v`
Expected: FAIL because the condition and targets do not exist.

**Step 3: Minimal implementation**

Add the systemd condition, static gate, and build-only preparation target. Do not enable or start the service. Document the explicit operator sequence.

**Step 4: Verify GREEN**

Run: `make gap_lane_service_config gap_lane_service_prepare`
Expected: `GAP_LANE_SERVICE_CONFIG_PASS`, executable daemon produced, no model process started.

**Step 5: Commit**

Commit only the four owned files.

---

### Task 2: Complete artifact/runtime provenance projection tracer

**Objective:** Project `CnetOracleIdentity.runtime_libs_digest` and the existing native `artifact_sha256` from authoritative CNB descriptors through managed `OracleDescriptor` and `cnet_list_oracles` JSON while preserving the old C ABI.

**Files:**
- Modify: `include/soul_host.h`
- Modify: `src/soul_host.c`
- Modify: `tests/test_soul_host.c`
- Modify: `dotnet/Cce/CceNative.cs`
- Modify: `dotnet/Cce/SoulHost.cs`
- Modify/add focused tests under: `dotnet/CnetMcpServer.Tests/`
- Modify: `dotnet/CnetMcpServer/CnetTools.cs`

**Step 1: Native RED**

Set a nonzero runtime digest in the existing persisted Oracle fixture and assert a wished-for `soul_oracle_runtime_libs_digest(host,index,&out)` returns it. Assert invalid host/index/out-pointer fail. Run `make soul_host_test`; expected compile failure because the accessor is absent.

**Step 2: Native GREEN**

Add a separate exported accessor. Do not append an argument to `soul_oracle_identity`; old callers must remain ABI-compatible. Return the persisted value unchanged, including zero for pre-v5/unattested descriptors.

**Step 3: Managed/MCP RED→GREEN**

Add P/Invokes for the new runtime accessor and existing full-hash accessor, append optional/defaulted `ArtifactSha256` and `RuntimeLibsDigest` tails to `OracleDescriptor` so existing eight-argument source callers remain valid, fetch exact values per native descriptor, and include `artifactSha256` as 64 lowercase hex characters plus `runtimeLibsDigest` as lowercase fixed-width hex in MCP JSON. Add a focused test that opens the real native fixture and proves the exact values reach JSON; do not rely only on a managed synthetic descriptor.

**Step 4: Verify GREEN**

Run: `make soul_host_test && make cnet_dll && make mcp_compression_test`
Expected: native marker and .NET tests pass; exported symbol exists in `cnet.so`.

**Step 5: Commit**

Commit only the owned native/managed files.

---

### Task 3: Integration review and authority closure

**Objective:** Accept only behavior-backed changes and leave master clean.

**Files:**
- Modify after evidence: `plans/delegation_master_report.md`
- Modify after evidence: `plans/toolchain_digest_linked_runtime.md`
- Modify after evidence: `README.md`

**Step 1: Spec review**

Confirm service config never starts/enables a model, exact condition/ExecStart agreement, new accessor preserves ABI, and MCP reports descriptor provenance rather than runtime admission.

**Step 2: Adversarial quality review**

Challenge missing/invalid indices, pre-v5 zero semantics, P/Invoke symbol loading, clean-tree behavior, service condition behavior, and accidental model start.

**Step 3: Integrate candidate commits**

Cherry-pick into `feat/unified-self-improvement`, update master report/status docs to evidence-backed results, and run diff hygiene.

**Step 4: Single umbrella acceptance**

Run one consolidated command covering `gap_lane_service_config`, `soul_host_test`, MCP integration, `gap_lane`, and `release_integrity`. Preserve/restore only allowlisted generated evidence. Do not start the Gemma-backed systemd service.

**H0:** the loop remains operationally fragile or linked-runtime provenance disappears above CNB.

**H1:** the missing executable is a clean skip with a build-only recovery path, and exact CNB full-artifact plus linked-runtime provenance reaches native/managed/MCP observers under passing unified/release gates.
