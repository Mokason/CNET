# CNET 5.1.1 Integrity Closure

> **For Hermes:** Execute with isolated worktrees, strict RED→GREEN tracer bullets, centralized integration, and one final `make release_integrity` acceptance.

**Goal:** Close CNET’s remaining silent-corruption, residency, admission-authority, persistence, MCP-survival, and release-artifact integrity gaps without adding features.

**Architecture:** Keep the live certified registry and model manager as the only runtime authorities. Fail closed at artifact/protocol boundaries, make state transitions transactional, and make the generated source archive prove itself. Independent slices are implemented in isolated worktrees and cherry-picked into `master` only after focused evidence.

**Tech stack:** C11/pthreads, GGUF/GGML reference vectors, POSIX/Windows file durability wrappers, .NET 10 JSON-RPC stdio, GNU Make and shell.

**Final umbrella:** `make --no-print-directory release_integrity`

## Ordered slices

1. `plans/integrity_q5_gguf.md` — exact Q5_K decode and malformed-metadata refusal.
2. `plans/integrity_model_runtime.md` — callback-outside-lock and failed-relocation preservation.
3. `plans/integrity_specialist_authority.md` — explicit unsafe registry append boundary and certified production default.
4. `plans/integrity_persistence.md` — atomic KB publication and restart restoration.
5. `plans/integrity_mcp_protocol.md` — malformed JSON-RPC survival and bounded lock scope.
6. `plans/integrity_release_gate.md` — extracted-archive build/consume and one release authority.

## Closure contract

H0: any slice remains unwired, the extracted archive is not self-building, or the umbrella command is unexecuted/failing.

H1: all focused tests are mandatory prerequisites; the clean-tree umbrella exits zero; exact terminal markers are present; no sanitizer/fatal marker occurs; archive extraction builds and runs a consumer; no process is left running.

No external push is part of this cycle.
