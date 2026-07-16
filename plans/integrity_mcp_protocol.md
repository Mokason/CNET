# Integrity Slice — MCP Protocol Survival

**Place:** `dotnet/CnetMcpServer/Program.cs` and its tests.

**Dilemma:** malformed request shape can throw outside the tool boundary and terminate the stdio server; one global lock covers unrelated long calls.

**Consequence:** one bad client request kills service, while compression blocks health/read tools.

**Systematic component:** deterministic JSON-RPC streams and an injected two-second independent-operation delay. **Noise:** scheduler latency is bounded by a one-second protocol-response deadline and a separate proof that the injected operation remained active.

## TDD

1. Feed malformed JSON, invalid/missing id, missing params, malformed tool arguments, unknown method, then a valid tools/list request to one process; current server must fail the survival assertion.
2. Return JSON-RPC -32700/-32600/-32601/-32602 errors without leaving the read loop.
3. Preserve request ids when valid and keep stderr diagnostics separate from stdout protocol frames.
4. Restrict the native-authority lock to registry/SoulHost mutations; keep independent compression outside it.
5. Focused marker: `MCP_PROTOCOL_SURVIVAL_PASS`.

## Focused Evidence

- Primitive and array roots, wrong/missing JSON-RPC versions, and primitive/null params are rejected without terminating the live stdio stream.
- `initialize`, `tools/list`, `tools/call`, initialized, and unknown notifications produce no response; stdout remains JSON-RPC-only.
- A blocked compression notification runs outside the SoulHost authority path while `tools/list` completes within the bounded deadline; process exit proves the injected delay actually ran.
- The integrated .NET suite passes 12/12 and `make mcp_protocol_survival` emits `MCP_PROTOCOL_SURVIVAL_GATE_PASS`.
