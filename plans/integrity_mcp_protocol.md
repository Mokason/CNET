# Integrity Slice — MCP Protocol Survival

**Place:** `dotnet/CnetMcpServer/Program.cs` and its tests.

**Dilemma:** malformed request shape can throw outside the tool boundary and terminate the stdio server; one global lock covers unrelated long calls.

**Consequence:** one bad client request kills service, while compression blocks health/read tools.

**Systematic component:** deterministic JSON-RPC streams. **Noise:** timing only for concurrency latency, kept out of correctness assertions.

## TDD

1. Feed malformed JSON, invalid/missing id, missing params, malformed tool arguments, unknown method, then a valid tools/list request to one process; current server must fail the survival assertion.
2. Return JSON-RPC -32700/-32600/-32601/-32602 errors without leaving the read loop.
3. Preserve request ids when valid and keep stderr diagnostics separate from stdout protocol frames.
4. Restrict the native-authority lock to registry/SoulHost mutations; keep independent compression outside it.
5. Focused marker: `MCP_PROTOCOL_SURVIVAL_PASS`.
