# Unified CNET Runtime Implementation Plan

> **For Hermes:** Execute autonomously with TDD and one final H0/H1 acceptance check. Do not commit or disturb unrelated dirty-tree artifacts.

**Goal:** Make CNET's certified BTN units, CCE models, native host, .NET API, and MCP tools reachable through one contract-governed runtime and one native library while preserving modular payloads.

**Architecture:** Keep the pure-C contract/planner/executor as the source of truth. Introduce a BTN-compatible runtime adapter callback so any deterministic specialist can enter the existing planner without duplicating DAG logic. Expose canonical registry enumeration through `soul_host`; make .NET and MCP consume that API instead of deriving state from `.gaps.txt`. Build a superset `cnet.so` that exports CCE and soul-host APIs; retain `cce.so` as a compatibility artifact.

**Tech Stack:** C11, CCE C runtime, CNB1, C ABI, .NET 10 LibraryImport, stdio MCP, Make.

**Status:** Implemented. `make unified` completed with `CNET_UNIFIED_PASS`.

---

## Task 1: Canonical soul-host enumeration

**Files:**
- Modify: `include/soul_host.h`
- Modify: `src/soul_host.c`
- Modify: `tests/test_soul_host.c`

**RED:** Build a self-contained CNB base with one certified unit, open it through `soul_open`, and assert `soul_unit_count` and `soul_unit_name` enumerate the loaded certified registry. Compile before adding the APIs; expected failure is missing declarations/symbols.

**GREEN:** Add bounded, P/Invoke-safe enumeration over `SoulHost.reg`, returning only units admitted by certification replay. Keep names borrowed internally and copy into caller buffers.

## Task 2: Universal contract adapter

**Files:**
- Modify: `include/nn.h`
- Modify: `src/nn.c`
- Create: `tests/test_unified_runtime.c`

**RED:** Register a callback-backed primitive with typed ports, plan it through `route_plan`, and execute it through `route_execute`; expected initial failure is missing `btn_init_adapter`.

**GREEN:** Append runtime-only adapter fields to `BinaryTransformNetwork`; add `btn_init_adapter`; dispatch callbacks from `btn_forward`; release context exactly once in `btn_free`; reject training/persistence paths that require matrix weights. Existing matrix BTN behavior stays byte-for-byte at the API level.

## Task 3: CCE model adapter and certified planner composition

**Files:**
- Create: `include/cce/cce_contract_adapter.h`
- Create: `src/cce/cce_contract_adapter.c`
- Extend: `tests/test_unified_runtime.c`

**RED:** Build a tiny real `cce_model`, wrap it with typed ports, certify its observed exemplar through the normal contract machinery, admit it with `registry_add_certified`, and assert the canonical route executor returns the same output as direct CCE forward.

**GREEN:** Convert contract-boundary doubles to CCE floats, call `cce_model_forward`, convert back, and support explicit model ownership. No invented certification: callers must supply and replay a real contract before certified-mode planning.

## Task 4: One native library and canonical managed roster

**Files:**
- Modify: `Makefile`
- Modify: `dotnet/Cce/CceNative.cs`
- Modify: `dotnet/Cce/SoulHost.cs`
- Modify: `dotnet/CnetMcpServer/CnetTools.cs`
- Modify/Create tests under `dotnet/Cce.Tests/` or `dotnet/CnetMcpServer.Tests/` as project structure permits

**RED:** Managed host test calls `SoulHost.Units()` against the self-contained test base or native smoke fixture; MCP list behavior must not depend on a `.gaps.txt` sidecar.

**GREEN:** Export enumeration from `cnet.so`; point both managed CCE and soul bindings to the superset library; make `UnitRoster()` call the host enumeration API. Sidecars remain optional provenance only, never authority.

## Task 5: Unified acceptance gate

**Files:**
- Modify: `Makefile`
- Modify: `tests/verify_logs.sh` only if a new log marker is needed
- Update: `README.md`

Add `make unified` as a CPU-only vertical gate covering:
1. CNB save/load and certification replay.
2. Canonical host enumeration and named execution.
3. Callback adapter planning/execution.
4. Real CCE model adapter certification and route execution.
5. Superset `cnet.so` build.
6. .NET build/tests and MCP canonical roster path.

Final one-check:
- **H0:** CNET still has parallel runtime truths or the unified path is unexecuted.
- **H1:** The public catalog→registry→planner/executor→host→.NET/MCP path runs from real artifacts with tests passing.
