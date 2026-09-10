# CNET improve-from-information (not model clone)

Date: 2026-08-20  
User intent: *get info so CNET can improve itself and understand better — no hardcoded answers, not about Bonsai.*

## Point

Self-improve = **absorb information → structured memory → later CERT if verified**.  
Not: canned FAQ. Not: distill 27B into a second parrot.

```text
incoming info (chat, file, miss, teach)
        │  ingest (C)
        ▼
  knowledge jsonl  (claims/chunks, claimed_cert=0)
        │  recall by overlap (understand better now)
        │
        ▼
  propose gold / capsule / brick   (never silent seal)
        │  verify
        ▼
  LOCAL CERT recall (understand better permanently)
```

## Law
- Ingest ≠ CERT
- Teacher/draft labels stay `auto_cert=false`
- Anti-collapse: do not train students on CNET LOCAL answers
- Hardcoded skill strings are not “understanding”

## Live verbs (cnetd PEER)
- `learn this: <text>` — ingest
- `what do you know about <topic>` — recall ingested chunks
- existing `remember that` / `recall` — session notes

## Tools
- `bin/cnet_ingest_info` — file/stdin → knowledge jsonl + DISTILL-style propose
- Knowledge file: `$CNET_MINIMAL_ROOT/var/marble_knowledge.jsonl` or `var/marble_knowledge.jsonl`

## Overnight (Claude)
Wire ingest into more of cnetd, harvest miss_log prose into knowledge, don’t add FAQ skills.

---

## Overnight completion (writer lane, 2026-08-20)

Doctrine above unchanged. This records what shipped.

### 1. Miss-log prose harvest

`bin/cnet_ingest_info --from-miss-log PATH [--max N]` mines organic demand into
the same knowledge jsonl, `claimed_cert=0`.

It has to be **selective**. On this host the miss log is
**17976 of 19978 rows "ABSTAIN: no local skill..."** — harvesting it wholesale
would fill Marble's memory with itself saying it does not know things. Dropped:

| dropped | why |
|---|---|
| abstain / refusal / "not sure" answers | not information |
| CNET's own self-description | **anti-collapse** — our output must not return as a learned note |
| blocklisted probe strings (`zzq`, `mystic ooze`, …) | test junk |
| chunks already stored | dedupe |

Live run: `scanned=19978 kept=36 skipped_abstain=17976 skipped_self=5
skipped_blocked=22 skipped_dupe=68`. Re-running keeps 0 — the dedupe probe
compares against the **stored escaped** form, which an earlier version did not,
so repeat runs used to re-add every note containing a quote.

### 2. Ingested notes outrank the residual draft

The point: someone tells Marble what a *gold hash* is, and it stops inventing
Bitcoin.

`kb_recall` matches the whole topic as a substring — right for an explicit
"what do you know about X", useless for ordinary chat. Added
`kb_recall_overlap()`: significant-word overlap against stored chunks, hooked in
`handle_client` **before** the residual stage drafts.

The threshold is the part that matters. A weak match that shadows chat with an
irrelevant note is worse than the guess it replaced, so a hit needs **two
distinct significant words, or one ≥7-char specific term**.

```
before ingest: SOURCE CORE  core_open_chat  "A gold hash in CNET refers to a
                                             specific type of hash value used in
                                             the platform's security systems…"
learn this:    SOURCE ACTION learn_info     CLAIMED_CERT 0
after ingest:  SOURCE INFO   kb_recall  MISS 1  CLAIMED_CERT 0
               "From what I was told (not certified): [1] A gold hash in CNET is
                the first 16 hex chars of SHA-1 over the normalised query…"
```

Notes are **not CERT**: `SOURCE INFO`, `miss=1`, `claimed_cert=0`, still
learnable. A topic with no note still reaches the residual — checked in the gate,
because silent shadowing would be the easy way to break chat.

Precedence is now: **sealed pack → ingested note → residual draft**.

### 3. Repeat demand proposes (never seals)

Every note-answered turn is counted in `var/kb_hits.jsonl`. At
`CNET_KB_PROPOSE_N` (default 3) distinct hits for the same normalised query, a
pending proposal lands in `var/capsule_inbox/kbnote-<ts>/PROPOSE.json` with
`auto_cert:false`, `claimed_cert:0`, `status:pending_verify`.

It fires **exactly at the crossing**, not on every hit after it — `>=` minted a
fresh identical directory on every repeat ask.

Promotion is unchanged: gold / multi_stable+reviewer. Nothing here seals.

### 4. Unattended

`cnet-info-harvest.timer` — hourly, `Persistent=true`, rebuilds the binary first
(`bin/` is gitignored, and a missing artifact is how an unattended loop dies
quietly). First run `Result=success`.

### Gate

`make improve_info` → `IMPROVE_INFO_PASS` (13 checks): harvest selectivity and
idempotence, notes-beat-residual, no-shadowing, sealed identity still wins, and
nothing on the path claiming CERT.
