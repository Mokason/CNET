# Serve Boundary Hardening

**Status:** implemented in the 2026-09-03 working tree; claims remain limited to
the gates named below.

## Context

Several independent boundary failures could silently change what CNET served:
an unaddressed core-bus request could select an unrelated LUT, a missing named
tensor could fall back to a different site, same-size CNB replacement could
leave the managed host serving the old generation, and socket clients could
hold the single warm daemon without a deadline. Shell interpolation also made
governance and ROE document paths executable input.

## Decisions

1. Core-bus certification requires an addressed, resolved unit. Explicit
   tensor requests refuse a missing site instead of trying a generic Q/QKV
   tensor.
2. Managed CNB observation and application are separate identities. A stable
   SHA-256 identity must match across open. A changed same-size generation is
   unavailable by default rather than serving the previously applied host;
   process rotation is the normal recovery because repeated native reopen was
   measured to retain roughly 1–2 GiB per generation. Opt-in in-process reload
   remains available as `CNET_SOUL_RELOAD_SAME_SIZE=1` with
   `CNET_SOUL_RELOAD_MIN_SEC`.
3. `cnetd` remains serial because its warm `CdState` is mutable session state.
   The framing boundary is bounded instead: one newline-terminated request,
   one monotonic deadline, explicit overlength refusal, strict JSON field
   parsing, and peer identity reset on every request.
4. The outbound shared-MCP client uses a nonblocking connect, completes partial
   writes, and waits for a newline reply under one monotonic deadline. Timeout
   is configured by `CNET_MCP_TIMEOUT_MS`.
5. Governance copies bytes through an atomic same-directory temporary file.
   ROE document/OCR helpers canonicalize input operands and spawn fixed argv;
   paths never enter a shell command string.
6. Serving/chat rules live in `mk/serve.mk`; model-free security regressions are
   first-class Make targets. New standalone tools must have explicit targets or
   the orphan gate remains red.

## Alternatives rejected

- Per-client `cnetd` threads: unsafe without first partitioning or locking the
  mutable session state, and unnecessary to eliminate the indefinite wedge.
- Serving the last CNB after observing a different generation: availability at
  the cost of stale certification violates the fail-closed rule.
- Shell quoting helpers: every quoting scheme retains a second parser. Fixed
  argv and direct file I/O remove that parser entirely.

## Focused evidence

- `make core_bus_untagged`
- `make core_bus_tensor_refusal`
- focused `CnbGenerationTrackerTests` and `McpProtocolIntegrityTests`
- `make governance`
- `make roe_process_security`
- `make cnetd_protocol_boundary`
- `make cnet_mcp_transport`
- `make cnet_ood`
- `make build_integrity`

The anti-parrot study is wired as `make cnet_lm_antiparrot`, but its current
held-out neural score does not beat the n-gram baseline. `ANTIPARROT_DONE` means
the study completed; it is not a quality or certification pass.
