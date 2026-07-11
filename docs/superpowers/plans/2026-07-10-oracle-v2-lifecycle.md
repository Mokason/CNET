# Oracle V2 Evidence-Carrying Lifecycle Implementation Plan

> **For Hermes:** Execute with strict RED→GREEN tracer bullets and one final `make test && make unified && make build` check.

**Goal:** Turn the legacy answer/abstain callback into a versioned, evidence-carrying, semantically validated Oracle lifecycle while preserving every v1 caller.

**Architecture:** Native C remains authoritative. `OracleEntry` gains an append-only v2 callback, stable component identity, explicit result status, confidence/evidence metadata, semantic validator, and per-status accounting. All acquisition and runtime adapter calls route through one `cnet_oracle_invoke` function. CNB2 persists Oracle identity while still loading CNB1. SoulHost/.NET/MCP project the native descriptors; they do not reinterpret Oracle semantics.

**Tech Stack:** C11, existing CNET ports/contracts/registry/CNB/SoulHost, .NET 10 P/Invoke, MCP stdio.

---

### Task 1: Define versioned Oracle result and identity ABI

**Files:**
- Modify: `include/acquire.h`
- Test: `tests/test_oracle_v2.c`

1. Write a compile-failing test for explicit statuses, ABI version/size, stable identity digest, confidence, and evidence digest.
2. Compile and confirm failure because v2 symbols do not exist.
3. Add fixed-width `CnetOracleStatus`, `CnetOracleValidity`, `CnetOracleIdentity`, `CnetOracleResult`, v2 callback and validator types.
4. Add append-only v2 fields/counters to `OracleEntry` while retaining legacy fields.

### Task 2: Centralize invocation, validation, and accounting

**Files:**
- Modify: `src/acquire.c`
- Modify: `src/specialist_adapters.c`
- Test: `tests/test_oracle_v2.c`
- Test: `tests/test_oracle_contract_adapter.c`

1. Test ANSWER, ambiguous abstention, verifier-undetermined abstention, policy refusal, transient failure, permanent failure, malformed result, invalid type, and semantic invalidity.
2. Implement `cnet_oracle_invoke`; legacy callbacks are mapped without changing behavior.
3. Implement `acquire_oracle_register_v2`.
4. Replace serial acquisition and runtime adapter callback/accounting duplication with `cnet_oracle_invoke`.
5. Keep the existing batch v1 path intact; v2 entries use the central serial path until a separately tested v2 batch ABI exists.

### Task 3: Persist bounded Oracle identity in CNB2

**Files:**
- Modify: `include/base.h`
- Modify: `src/base.c`
- Test: `tests/test_base.c`

1. Add a RED round-trip test for artifact, contract, config, retrieval snapshot, and toolchain digests.
2. Extend `CnbOracleDesc` and add `cnb_add_oracle_desc_v2`.
3. Write CNB version 2; load both versions 1 and 2, zero-filling identity for legacy files.
4. Bind legacy callbacks as before while attaching the persisted identity to the runtime entry.

### Task 4: Project authoritative Oracle descriptors through host/.NET/MCP

**Files:**
- Modify: `include/soul_host.h`
- Modify: `src/soul_host.c`
- Modify: `dotnet/Cce/CceNative.cs`
- Modify: `dotnet/Cce/SoulHost.cs`
- Modify: `dotnet/CnetMcpServer/CnetTools.cs`
- Modify: `dotnet/CnetMcpServer/Program.cs`
- Test: `tests/test_soul_host.c`

1. Add RED native enumeration tests.
2. Add bounded C ABI descriptor enumeration from the loaded native base.
3. Add managed immutable descriptor records.
4. Add `cnet_list_oracles` as a projection of native metadata only.

### Task 5: Umbrella gate and research positioning

**Files:**
- Modify: `Makefile`
- Create: `plans/phase7_oracle_v2.md`
- Modify: `plans/delegation_master_report.md`

1. Add `oracle_v2_test` to `make unified` and export-symbol checks to `unified_native`.
2. Verify RED/GREEN focused tests.
3. Run exactly one final closure check: `make test && make unified && make build`.
4. Record systematic guarantees versus irreducible Oracle uncertainty and compare the resulting lifecycle against OGIS, Snorkel, PATE, Expert Iteration, verifier-guided learning, and learning-to-defer.
