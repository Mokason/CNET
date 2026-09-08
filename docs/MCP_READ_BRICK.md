# MCP read brick

`mcp_read_dispatch_v1` is a portable, certified dispatch capsule in CNET's
existing `unit.cnb` / `manifest.cknow` format. It selects two approved MCP read
tools. It is not a raw LUT brick or an MTK weight-delta cartridge.

| Explicit command | Certified dispatch | MCP tool |
| --- | --- | --- |
| `wiki search Quasicrystal` | action 1 → tool 1 | `cnet_safe_wiki_search` |
| `web read https://en.wikipedia.org/wiki/Quasicrystal` | action 2 → tool 2 | `cnet_safe_web_read` |

Keywords accept ASCII case variations; arguments retain their original case.
The two words and argument require single ASCII spaces. Recognized malformed
separators refuse terminally, including Unicode control/whitespace separators.
Other commands are outside this interface. This is an explicit command adapter,
not a general natural-language tool selector. Argument contents never select a
tool, a capsule, permissions or a server.

## What is certified

The capsule has two independently specified host-ABI labels, input/output
`PORT_BINARY_MSB` widths 2, and tags `mcp_read_action` / `mcp_read_tool`.
Inputs 1 and 2 are covered; 0 and 3 abstain. The host additionally checks the
unit identity, one-hop result and expected tool identifier. A changed capsule
cannot expand the allowlist. The exported coverage gate is mandatory.

The certificate proves only this finite dispatch mapping. It proves neither
the truth of a retrieved page nor arbitrary language comprehension. Web text
cannot become a certified answer or `verified_tool` training label simply
because it arrived through MCP. No automatic web learning is added here.

The separate `CNET_MCP_READ_CAPSULES` inventory is reopened and validated for
every invocation, so subsequent calls see a replaced valid capsule without a
process restart. Missing/corrupt/unsupported inventories refuse; they do not
fall back to an older lookup or teacher. Keep this operator-owned inventory
separate from numeric learned-domain inventories. Capsule checksums are local
integrity checks, not signatures from a trusted publisher.

## Network and evidence boundary

Both client and server require `CNET_MCP_READ_ENABLED=1`. The server snapshots
`CNET_MCP_READ_HOSTS` at startup: comma-separated exact ASCII DNS names,
default `en.wikipedia.org`, at most 32. Empty/invalid policy refuses. There are
no wildcard hosts or tool-argument policy overrides.

Only HTTPS GET on port 443 is supported. URL credentials, IP literals,
fragments, nonapproved hosts and private/special-use destinations refuse.
All DNS answers must pass policy; the socket connects to one checked IP
without resolving again. Normal TLS hostname/certificate checks stay enabled.
Redirects, proxies, cookies, authentication and automatic decompression are
disabled. These checks apply at the connection boundary as well as URL parsing.

Budgets per call: 10 seconds through body receipt, 256 KiB body, 16 KiB headers,
at most two concurrent calls (one Wikipedia call); excess calls return `busy`.
Wiki queries are at most 200 UTF-16 units. The native URL bound is 2048 UTF-8
bytes; the server also bounds URI serialization. Wikipedia batches up to three
introductory plaintext extracts in one API request. Web reads accept only
UTF-8/ASCII `text/plain` or `text/html`. HTML extraction is deliberately simple;
no JavaScript, browser, downloads, link traversal or recursive crawl runs.
Oversized/compressed/partial/unsupported responses are errors, not truncated
successful HTTP documents. Display excerpts are separately capped at 6000
UTF-16 units (Wikipedia API extracts are requested at 1200 characters).

Successful results contain MCP `content`, `structuredContent` and
`isError:false`. The evidence schema is `cnet.web-evidence.v1` with `tool`,
`status:"ok"`, `trusted:false`, `certified:false` and 1–3 `sources`:
`url`, `title`, `text`, `retrieved_at` (UTC), `sha256` (exact UTF-8 excerpt).
The hash does not cover the original whole HTTP body and does not authenticate
the author. The native client checks the RPC ID/version, duplicate fields,
schema, UTC calendar timestamp and recomputed excerpt hash. It never falls
back to arbitrary MCP text when structured evidence is missing or erroneous.
The RPC receive frame itself is bounded to 256 KiB.

The daemon returns `source:MCP_READ`, `verified:false`, `miss:true`, skips
automatic gap-learning/starvation hooks, and renders a marked, single-line,
UTF-8-safe excerpt. A page cannot inject `CLAIMED_CERT`/`END` framing. Full
evidence is available from the CLI; the conversational answer is only a display
excerpt. Neither returned text nor URLs are fed back into command selection.

## Build and try without changing live services

From the source checkout:

```sh
make mcp_read_verify
CNET_MCP_LIVE_TEST=1 python3 tests/test_mcp_read_integration.py -v
```

The first command builds native/managed components, creates synthetic native
fixtures and runs isolated tests. The second additionally performs a real
Wikipedia lookup through the native capsule, a private UNIX test bridge and a
new stdio MCP process. Neither changes the installed shared server or daemon.
Do not run fixture-generating verification in a dirty production checkout.

To create a portable capsule in an explicitly selected private directory:

```sh
make capsule_core bin/cnet_mcp_read
bash scripts/build_mcp_read_brick.sh /absolute/private/mcp-read-capsules
```

The builder uses fixed dispatch labels and existing certification, history,
evaluation and export gates. It is an operator step, never launched by web text.
An existing conflicting target is not overwritten.

To use the CLI with an **updated**, explicitly approved local MCP peer:

```sh
CNET_MCP_READ_ENABLED=1 CNET_MCP_CLIENT=1 CNET_MCP_TIMEOUT_MS=12000 \
CNET_MCP_SHARED_SOCK=/absolute/private/mcp.sock \
bin/cnet_mcp_read /absolute/private/mcp-read-capsules 'wiki search Quasicrystal'
```

The server must separately run this version of `CnetMcpServer` with its own
read enable/host policy. The shared-socket broker owns MCP initialization and
the trusted stdio session; the native client sends only approved `tools/call`
requests. This release does not install a new broker. Set the native timeout to
12000 ms to accommodate the backend's 10-second budget; its legacy default is
2000 ms and may refuse slow but otherwise valid requests.

For daemon use, also set `CNET_MCP_READ_CAPSULES` in the updated daemon's
environment. Server policy changes require replacing/restarting that server;
they are not chat actions. The daemon is serial, so a read can delay subsequent
requests for its configured deadline. Disable reads at either endpoint to stop
this capability. Never broaden permissions just to make a fetch pass.

Existing legacy `cnet_web_search`, `cnet_wiki_lookup`, other chat actions and
administrative MCP tools are unchanged. They are **not** certified safe by
this work. Protect daemon/MCP socket access; the entire existing service is not
a read-only public endpoint. This brick can call only its two new safe tools.

## Scope and references

The September 8 live rollout also repairs the **existing** shared broker,
versioned as `tools/cnet_mcp_shared.py`; it is not a second MCP host. Backend and
new thin-client responses are bounded to 2 MiB, including final serialization
after restoring client IDs. Overflow returns an explicit error or stops the
broker, never clipped metadata or a silently dead reader. Invalid UTF-8, partial
frames, parser recursion and pipe failures stop the broker. The deployment uses
`Restart=always` because the inherited SIGTERM cleanup exits successfully; an
explicit operator stop still stops the service. `CNET_MCP_WORKDIR` preserves the
old relative fact bank, and `CNET_MCP_SERVER_BIN` selects the frozen executable.
Already-running old thin-client processes retain their old 64 KiB limit until
they reconnect; no unrelated application is restarted automatically. Native
safe-read evidence keeps its separate 256 KiB bound. Run
`python3 tests/test_mcp_shared_frames.py` for the hermetic framing gate.

Recursive crawling, arbitrary remote MCP servers, dynamic tool discovery,
authenticated websites, general web search and factual verification/training
from prose remain unimplemented. A larger tool vocabulary needs a reviewed
host adapter, new independently labelled contract and unchanged refusal gates.

- [MCP tools](https://modelcontextprotocol.io/specification/2025-11-25/server/tools):
  schemas, structured/text results and error semantics.
- [MediaWiki API etiquette](https://www.mediawiki.org/wiki/API:Etiquette) and
  [TextExtracts parameters](https://www.mediawiki.org/wiki/Extension:TextExtracts#API):
  identifying User-Agent, bounded batched API access and valid extract limits.
- [IANA IPv4 special registry](https://www.iana.org/assignments/iana-ipv4-special-registry/)
  and [IPv6 special registry](https://www.iana.org/assignments/iana-ipv6-special-registry/):
  conservative nonpublic/special-use address refusal policy.
- [Measured result](../result/cnet_mcp_read_brick_20260908.md).
