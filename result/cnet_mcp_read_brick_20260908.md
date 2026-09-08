# MCP read brick — measured handoff

Source implementation: `a050d57`, based on `5f72514`, on local branch
`feature/mcp-read-brick-20260908`. Operator contract:
[MCP read brick](../docs/MCP_READ_BRICK.md).

Implemented and exercised a certified two-action dispatch capsule, strict
native MCP client, bounded read-only Wikipedia/page tools, daemon integration
and a standalone CLI. No live daemon, shared MCP service, Discord integration,
resident learning inventory, policy or GPU service was replaced. No remote
push or master merge is part of this handoff.

## Results

| Gate | Measured result |
| --- | --- |
| `make mcp_read_verify` | PASS; complete native + managed + hermetic integration sequence |
| Managed MCP tests | 131 passed, 0 failed, 0 skipped; includes 110 safe-web cases |
| Native dispatch | 9 test methods pass; two covered actions, wrong identity/mapping and OOD refusal |
| Native strict protocol/display | 5 test methods pass; errors, schemas, hashes, dates, budgets, injection refusal |
| Daemon read boundary | 3 test methods pass; disabled/Unicode refusals, valid Unicode evidence, no text CERT/END injection |
| Shared transport regression | 12 checks pass; short writes, one deadline, closed-peer SIGPIPE refusal |
| Default real-process integration | 2 pass; live-network test explicitly skipped by default |
| Explicit live-network integration | 3 pass including native capsule → UNIX test bridge → real stdio MCP → Wikipedia |
| `make knowledge_capsule` | PASS, 94 checks, 5 coverage rows |
| `make coverage_abstain` | PASS, 55 checks; existing held-out fixture 4/4 correct |
| `make knowledge_composition_bench` | PASS, 3 members, coverage at every hop, root/intermediate refusal |
| GCC ASan + UBSan + leak checks | 5 strict-client test methods pass; no sanitizer findings |
| NuGet vulnerable/transitive audit | No vulnerable packages returned for CnetMcpServer against configured NuGet source |

These are bounded mechanism/security regressions, not a factual-accuracy,
general-language, crawl-coverage or autonomous-learning benchmark. No full
repository/GPU/OS security audit was run for this change. Existing unrelated
compiler warnings in capsule-table/flagship builds were not changed; new
native MCP builds pass `-Werror`, managed rebuild has zero warnings/errors.

The full managed rerun initially exposed absent private fixtures. The existing
native `soul_host_test` now retains its already certified synthetic 13-unit
growth base under the existing `CNET_KEEP_TEST_BASE` flag. The managed reload
test swaps from its one-unit fixture to that base, checks the new count/name
and old-name absence. It no longer needs an operator's `flagship.cnb`. This
proves generation replacement with a 13-unit synthetic fixture, not the former
private 256-unit model's capacity. Certification floors were not modified.

## Real retrieval

At `2026-09-08T08:49:48Z`, the complete native/stdio path searched
`Quasicrystal` and returned three sources. The three-test live integration
invocation completed in 0.809 seconds on this host; this is a single smoke
timing including fixtures/process startup, not a latency percentile.

- [Quasicrystal](https://en.wikipedia.org/?curid=25350), excerpt SHA-256
  `2cdf4e30978ffb906e3b5f5065161b4a16caf6b345f4d8a4eaa8a84841182d3f`.
- [Paul Steinhardt](https://en.wikipedia.org/?curid=1982016), excerpt SHA-256
  `c14bcb738b3dd00057de65cf15ac923a9b1e2ba4384f96979833453b4b567b92`.
- [Dan Shechtman](https://en.wikipedia.org/?curid=3674396), excerpt SHA-256
  `6ad5249b6784881ae088d7a0f7291a4563c9d0d3bb3671ee54dbc6f8d18f1f25`.

Both the native client and independent Python integration checks recomputed
the exact UTF-8 excerpt hashes. All evidence retained `trusted:false` and
`certified:false`. No page content was converted into a new knowledge capsule.
The fixed dispatch capsule is the only capsule this feature's builder creates.

## Portable artifact

A generated local copy is under
`artifacts/mcp_read_brick_v1_20260908/capsules/mcp_read_dispatch_v1/` in the
source worktree. It is reproducible with the documented builder, not committed
as a new packaging format or installed into live state.

- `unit.cnb`: 676 bytes, SHA-256
  `d22269d5f406432195a76570a9e4189886db1ce405a3786897201abc8f186549`.
- `manifest.cknow`: 334 bytes, SHA-256
  `b081bc6e759403d0d7e97b15bfc16cb5c1aabe3ff9b159c57eb1cf3f0dc5191f`.
- Total logical package size: 1010 bytes. This is disk payload, not measured
  per-capsule resident RAM or the memory footprint of the HTTP/runtime host.
- Behaviour digest: `4751295810174180492`; two labelled/covered rows,
  sampled scope, robust minimum margin `0.500000` against unchanged `0.05`
  floor. Finite-domain compilation, not open-ended training/generalization.

## Test-first security review

RED evidence preceded production additions for missing native dispatch/client,
MCP tool registration and disabled daemon routing. Adversarial tests then
reproduced and closed: SIGPIPE process termination, unverified excerpt hashes,
invalid timestamps, malformed upstream/argument UTF-16, unsupported MediaWiki
parameters, UTF-8 truncation, Unicode separator fallback, and external text
injecting `CLAIMED_CERT` or `END` into line-oriented replies.

Fresh Astra review found no remaining must-fix issue in the final production
delta. Skill-guided interface/security review kept permission checks in the
host, made refusals terminal, and kept web evidence outside automatic learning.
No external model CLI was used. Review is not a blanket vulnerability-free claim.

## Remaining scope

Deployment into an explicitly selected live daemon/shared MCP pair is not
done. Recursive crawling, arbitrary remote MCP servers, dynamic tool discovery,
authenticated browsing, broad search-engine support, unrestricted language
intent and verified factual learning from prose remain WITHHELD. Older lookup
and administrative tools are unchanged and are not covered by this safe-read
claim. The new capability is disabled unless both endpoints enable it.
