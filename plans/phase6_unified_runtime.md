# Phase 6 — Unified Contract Runtime

**Place:** The CNET boundary where CNB certified units, CCE specialists, native hosts, and MCP currently meet through separate seams.

**Dilemma:** Flattening everything into one monolith would erase CNET's modular trust model, while leaving separate registries makes evidence and routing disagree about what exists.

**Consequence:** Use one contract-governed runtime interface and catalog while keeping payloads independently sealed and loadable.

## Systematic component

Typed ports, contract replay, registry lifecycle, planner/executor behavior, bounded ABI buffers, and explicit ownership are deterministic and testable.

## Irreducible/runtime component

Model outputs, floating-point backend differences, unavailable optional artifacts, and external tool results remain measured evidence. They never become stored authority merely by being mounted.

## Deliverables

- Canonical soul-host enumeration.
- Callback-backed universal specialist adapter.
- Real CCE model adapter certified through normal contracts.
- Superset `cnet.so` with thin .NET/MCP projection.
- CPU-only `make unified` acceptance gate.

## Result

`make unified` returned `CNET_UNIFIED_PASS`. The CRLF-broken positive-marker
scanner and the independently reproduced legacy/CCE fixture failures were
subsequently repaired; `make test`, `make unified`, and `make build` now all
pass without masking failures.
