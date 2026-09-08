# Bounded MCP read brick

Owner request: a brick that can call MCP to look up new material safely.
Starting source: `5f72514`; implementation stays in the clean source worktree.
Existing live daemon, private learning state and shared MCP service are not
replaced as part of source development.

## Contract

An existing-format certified capsule maps two explicit host-parsed actions to
two approved MCP tool identifiers: Wikipedia search and single-page web read.
The host owns string parsing, permissions, transport and evidence handling.
The capsule certifies only this finite dispatch mapping, never a page's truth.
Missing, corrupt, uncovered or unexpected capsule output refuses before I/O.
This is a specialized tool-use capability, not unrestricted language coverage.

Use the existing local MCP transport and capsule export/import format. There is
no arbitrary tool-name selection, remote MCP server discovery, shell execution,
write tool, automatic training or content-driven follow-up tool invocation.
The supported initial commands are `wiki search <query>` and `web read <url>`.
A single-page reader is a building block for crawling; recursive crawling and
general search-engine coverage remain outside this first bounded capability.

Both ends are opt-in (`CNET_MCP_READ_ENABLED=1`). The server owns an exact host
allowlist (default `en.wikipedia.org`) and requires public HTTPS on port 443.
It disables redirects, proxy credentials and cookies, validates DNS answers at
connection time and connects to the checked address. Query, response, total
time and concurrency budgets are fixed, not supplied by retrieved content.

Results use MCP text plus structured content with source URL, retrieval time,
SHA-256 and explicit `trusted:false`, `certified:false`. Errors use `isError`.
The native client checks the RPC envelope and bounded evidence schema before
presenting anything; transport/tool/schema failures never become success text.
Web text stays untrusted data. No certification floor or learning provenance
rule changes. Checksums identify retrieved content, not source authenticity.

## Ordered verification

1. RED tests: disabled/missing capsule/unknown intent, malformed MCP replies,
   private destinations, redirects, bad arguments and resource limits.
2. Backend read-only tools and strict native evidence client, independently
   testable with controlled HTTP and local MCP fixtures.
3. Generate the dispatch capsule through existing teaching/export facilities;
   prove both covered routes, OOD refusal and portable reload with gates intact.
4. Register tools, connect the native brick to the daemon and provide a CLI.
5. Run existing transport/protocol/capsule regression, adversarial review,
   an isolated end-to-end MCP test and an opt-in public Wikipedia smoke test.
6. Document measured results, supported commands, opt-in configuration,
   deployment boundary and limitations. No source build implies live rollout.

No new package dependency is planned. Fresh Astra review is used under the
doubt-driven development skill; the owner's Astra-only preference excludes
external model CLIs.

## Primary references

- [MCP tools specification](https://modelcontextprotocol.io/specification/2025-11-25/server/tools):
  tool schemas, `tools/call`, text/structured results and `isError` semantics.
- [MediaWiki API etiquette](https://www.mediawiki.org/wiki/API:Etiquette):
  bounded API use and identifying clients.
