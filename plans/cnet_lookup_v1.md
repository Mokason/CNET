# web_lookup_v1 — typed fetch hop

Status: **HOOKED IN CNETD — NOT FLUENCY / COMPETE — OPEN CHAT WITHHELD**

On 2026-08-15, `make cnet_lookup_capsule` emitted:

```text
CNET_LOOKUP_CAPSULE_PASS contract=web_lookup_v1 bound=1 abstain=1
residual=0 broader_claims=WITHHELD
```

`tools/cnetd.c:cd_ask` offers a URL from the user turn to
`cnet_chat_lookup_cnetd_hop` **before** residual / teacher-on-miss / old
ROE `net.enable_lookup`. Bound scalars are spoken with
`cnet_lookup_speak` only. Fluency (`cnet_chat1_fluency`) and compete
still link utterance, not lookup. This remains a certified-shaped
**tool hop**, not a compete unit and not an MTK cartridge. It does not
join `CNET_COMPETE_UNIT_COUNT`. ASI-5 v5 and CHAT-1 floors are
untouched.

Unknown questions without a URL still abstain in CHAT. A turn that
contains a URL may bind+speak; without a bindable value it falls through
to the existing unknown-question abstain. No invented query. No page
summary.

## What it is

Input: URL + bind kind (`integer` / `token` / `line` / `year`).

Output: one bound scalar plus provenance, or abstain.

Provenance stored on a bound report:

- URL
- host (or `local-file` on the test-only file path)
- SHA-256 of the fetched body
- first-line-ish snippet (whitespace collapsed, capped)
- HTTP status / byte count

Speech is native C only:

```text
Marble reports <value> from <host> (web_lookup_v1, sha256=<hex>).
```

Unbound reports cannot be spoken.

## Conversation hop

`cnet_chat_lookup_cnetd_hop` (same function `cd_ask` calls) scans the
user turn for the first URL-like token (`http` / `https` / `file`, or a
refused scheme). It does not invent a search query and does not
summarize the page.

Bind kind is named in the turn (`integer` / `number` / `token` / `line` /
`year`). If unnamed, the default is **integer**: CHAT contracts already
speak one unsigned scalar (increment, minutes, crc, policy, compose),
and the lookup CLI defaults to `--bind integer`.

A bound value is a verified tool result. It may be spoken. It must not
be trained back as a CNET Tier-A answer. Residual / 8B is never the
mouth. The hop residual counter stays 0 because this path never calls
utter residual.

`file://` is test-only (`CNET_LOOKUP_F_ALLOW_FILE`). cnetd uses the
production path and refuses `file://`.

## What it is not

- Not residual speech. The 8B is never the mouth.
- Not an open-web answerer. It does not summarize pages.
- Not a search engine. It GETs one URL; it does not invent queries.
- Not an ASI-5 or CHAT-1 unit. Adding it to the six-unit compete set
  would break 1296-row ASI-5 geometry.
- Not hooked into fluency or compete.
- Broader claims (open chat, open-web Q&A, AGI) stay **WITHHELD**.

## Fail closed

| Refusal | When |
|---|---|
| `empty_url` | missing URL |
| `scheme` | not `http` / `https` (production); `file` only with the test flag |
| `host` | loopback, link-local, metadata, or redirect/connect there |
| `fetch` | curl failure or empty body |
| `http_status` | non-file URL with status ≠ 200 |
| `unbindable` | body has no value of the requested kind |

Schemes such as `javascript:`, `mailto:`, `ftp:`, and `data:` abstain.
Production also refuses `file://`. Redirects stay inside `http,https`
and are re-checked (Location header + resolved IP). TLS peer/host
verify is on.

SSRF deny list (name or literal IP, including redirects):

- loopback: `127.0.0.0/8`, `::1`, `localhost`
- link-local: `169.254.0.0/16`, `fe80::/10`
- metadata: `169.254.169.254`, `metadata.google.internal`

The unit fixture is `file://` under `/tmp` via `CNET_LOOKUP_F_ALLOW_FILE`.
No live internet is required for `CNET_LOOKUP_CAPSULE_PASS` or
`CNET_CHAT_LOOKUP_PASS` except a failing `.invalid` DNS for fetch-fail.

## Anti-collapse

A bound lookup value is a **verified tool result**. It may be spoken. It
must not be trained back as a CNET Tier-A answer.
