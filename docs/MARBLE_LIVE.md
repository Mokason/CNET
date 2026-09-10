# Marble Live — Neuro-level *inside* CNET (not Twitch VTuber)

Date: 2026-08-19  
Scope: phases **1,2,3,4,6** — **no** VTube/Twitch avatar show.

## Product intent

Marble is a **continuous specialized self** on this host:
- chats with improved dialogue / personality / context
- remembers session + durable episodic notes
- learns **actions** and improves via **capsules** over time
- can **propose** capsules (never self-CERT seal)
- talks to Hermes on **PEER socket** (not MCP mouth)
- optional **Discord** as another peer
- voice out (+ optional listen)
- residual stage for improv only (`claimed_cert=0`)

**Not:** AGI monobrain, Twitch entertainer primary, domination fantasy.

## Law rails

| Rail | Rule |
|------|------|
| CERT | sealed skills / capsules / bricks only |
| Residual stage | draft / voice / social — `auto_cert=false` |
| Capsule mint | **propose → verify → human/gold/reviewer** — never silent seal |
| Memory | dialog ctx (warm) + episodic file (durable) — never raises floors |
| Actions | allowlisted verbs; heavy actions zen-gated |
| MCP | factory only |
| Peer | cnetd PEER / Discord → same front door |

## Architecture

```text
Discord ─┐
Hermes  ─┼──► cnet_peer / PEER ──► cnetd
User CLI─┘         │
                   ├─ showrunner (mood, cooldown, bit queue)
                   ├─ dialog_ctx (anaphora, peer name, last skill)
                   ├─ episodic memory (jsonl, capped)
                   ├─ CERT first (packs, bricks, RLM)
                   ├─ residual stage (Bonsai :8081) if miss + stage_on
                   ├─ action bus (allowlist) → tools / capsule_propose
                   └─ voice out (TTS) if may_voice
                            │
              miss_log → evolve/gold → pack_personal / capsules
```

## Modules (C-first)

| Module | File | Job |
|--------|------|-----|
| Showrunner | `include/cnet_showrunner.h` `src/serve/cnet_showrunner.c` | mood DA/5HT/ADO snap, cooldowns, last-N turns |
| Episodic | part of showrunner or `cnet_episodic.h` | append-only memory ring → disk |
| Peer client | `bin/cnet_peer` | already |
| Voice loop | `bin/cnet_voice_loop` | peer ask → if may_voice → TTS |
| STT | `bin/cnet_listen` (optional) | wav/mic → text → peer (delivery only) |
| Discord bridge | `bin/cnet_discord_peer` or gateway unit | Discord messages → PEER discord |
| Capsule propose | `bin/cnet_capsule_propose` | export propose dir + inbox row; no seal |
| Residual stage | env in cnetd | stage draft only when CERT miss |

## Action allowlist (v1)

```text
status, identity, remember_note, recall_notes,
propose_capsule, open_miss_log_tail,
ask_peer_echo (debug)
```

Heavy (zen / governor): `run_cert_learn`, `structure_mine`, `seal_capsule` — **not** free chat.

## Capsule growth loop

```text
organic miss / successful residual draft
  → miss_log + optional episodic
  → gold or multi_stable+reviewer
  → pack_personal skill
  → OR capsule_propose(unit) → verify ladder → import
```

`propose_capsule` creates **pending** under `var/capsule_inbox/`; never live CERT.

## Discord

- Bot token: `CNET_DISCORD_TOKEN` (user secret, never pack)
- Channel allowlist: `CNET_DISCORD_CHANNELS`
- Each message: `PEER discord <user>: <text>` → cnetd
- Reply back to channel; rate limit + cooldown from showrunner

## Residual stage (phase 6)

- Prefer local Bonsai `:8081` (`CNET_HELD_MODEL_*` / residual HTTP)
- Only after CERT miss
- Tag `stage_draft=1 claimed_cert=0`
- May voice if `CNET_OPEN_CHAT_MAY_VOICE=1` and never as LOCAL CERT source name

## Success gates

| Gate | Marker |
|------|--------|
| showrunner unit | `SHOWRUNNER_PASS` |
| voice loop CERT ask | `VOICE_LOOP_PASS` |
| peer memory anaphora | existing `DIALOG_CTX_PASS` + extend |
| capsule propose dry | `CAPSULE_PROPOSE_PASS` |
| discord dry (no token) | skip / `DISCORD_PEER_SKIP` |
| full slice | `MARBLE_LIVE_PASS` |

## Out of scope

- Twitch, Live2D, VTube Studio
- Self-CERT from stage chatter
- Unbounded tool shell from chat

---

## Implementation notes (writer lane, 2026-08-19)

Foundation was already green (`SHOWRUNNER_PASS`, `VOICE_LOOP_PASS`,
`CAPSULE_PROPOSE_PASS`, `DISCORD_PEER_PASS`, `LISTEN_PASS`). This pass wired the
foundation *into the turn path* and made the law-bearing parts testable.

### New module: `cnet_marble_live`

`include/cnet_marble_live.h` + `src/serve/cnet_marble_live.c`, linked into
`cnetd`. Split out of `cnetd.c` deliberately: the episodic-path resolution, the
argv sanitiser, and the stage gate are the parts that can hurt you, so they are
unit-tested without needing a daemon (`make marble_live_wiring` →
`MARBLE_LIVE_WIRING_PASS`, 16 checks).

### Turn path in `cnetd`

```text
PEER <from> <q>
  ├─ cnet_dialog_ctx_set_peer()          who is speaking (informational)
  ├─ cd_action()   ── allowlist hit? ──► SOURCE ACTION, CLAIMED_CERT 0
  │                     remember_note / recall_notes / propose_capsule
  ├─ cd_ask()      ── CERT first ──────► SOURCE LOCAL, sealed skill
  │        └─ miss && CNET_STAGE_RESIDUAL=1 ──► STAGE draft beside the answer
  └─ cnet_sr_on_turn()                   mood, cooldown, last-N ring
```

**Actions run before CERT, but only three verbs claim a turn.** `who are you`
and status questions fall through to their sealed soul skills — the action bus
must never shadow a CERT answer. An action replies as `SOURCE ACTION` /
`DOMAIN ACTION`, so it is never confusable with a sealed skill.

### New reply fields

| Field | Meaning |
|---|---|
| `PEER <name>` | who asked (`-` = local user) |
| `ACTION <name>` | allowlisted action that handled the turn (`-` = none) |
| `STAGE_DRAFT 0\|1` | a residual improv draft is attached |
| `STAGE <text>` | the draft itself (only when `STAGE_DRAFT 1`) |
| `CLAIMED_CERT 0\|1` | 1 only for a verified, non-miss, non-stage answer |

`CLAIMED_CERT` is computed as `verified && !miss && !stage_draft`. A client
never has to infer certification from the source string.

### Episodic memory

Default `$CNET_MINIMAL_ROOT/var/marble_episodic.jsonl`, overridable with
`CNET_EPISODIC_PATH`; the parent directory is created. One JSON line per
remembered note (`ts`, `peer`, `note`). Warm context only — it never raises a
floor and is never consulted by routing.

```bash
cnet_peer --peer hermes "remember that the r9700 lane runs at 21:00"
cnet_peer --peer hermes "what do you remember"
```

### Residual stage (phase 6)

**Off unless `CNET_STAGE_RESIDUAL=1`, and only after a real CERT miss.**

```text
CNET_STAGE_RESIDUAL=1
CNET_STAGE_HTTP=http://127.0.0.1:8081     # Bonsai; /v1/chat/completions appended
CNET_STAGE_MODEL=held
CNET_STAGE_TIMEOUT=20
```

The draft is attached as `STAGE`, **beside** the answer. `SOURCE`, `SKILL`,
`DOMAIN` and `MISS` are left exactly as CERT reported them — the stage cannot
rename itself `LOCAL`, and `CLAIMED_CERT` stays `0`. The honest miss utterance
remains the `ANSWER`.

The stage deliberately does **not** reuse `cnet_held_model_*`: that global
endpoint is CERT plumbing shared with other planes, and the stage must not be
able to steer it. Separate transport keeps the two provably apart.

Observed:

```
SOURCE CNET / SKILL utter_self / DOMAIN ABSTAIN / MISS 1
STAGE_DRAFT 1 / CLAIMED_CERT 0
STAGE  Lisbon. It's the largest city and the capital.
ANSWER You asked about what is the capital of portugal. I have no sealed skill
       for that yet. Logged to miss_log for later gold review. Law: never self-cert.
```

### Capsule propose from chat

`propose capsule for <name>` → `bin/cnet_capsule_propose --unit <name>` → a
pending row under `var/capsule_inbox/`, `auto_cert=false`. Propose ≠ admit; the
reply says so out loud.

**Chat is the only untrusted input that reaches argv**, so the unit name goes
through `cnet_ml_sanitize_unit` (strict `[a-z0-9_]`, everything else dropped)
before it is shell-quoted. Verified: `propose capsule for x; touch /tmp/PWNED`
mints unit `x_touch_tmppwned` and creates no file.

### Discord live bridge

`tools/cnet_discord_peer.c` gained a real `--live` mode: REST polling over
`curl` (no websocket stack, no new link deps).

```bash
export CNET_DISCORD_TOKEN=...            # env only; never a pack, never logged
export CNET_DISCORD_CHANNELS=123,456     # MANDATORY allowlist
export CNET_DISCORD_PREFIX='!marble '    # optional
cnet_discord_peer --live
```

Each message becomes `PEER discord <user>: <text>` through the same front door
as Hermes. Bot-authored messages are skipped so it never answers itself. A stage
draft is posted labelled `_(stage draft, not certified)_`, never merged into the
answer.

**The allowlist is fail-closed:** empty, missing, or non-numeric
`CNET_DISCORD_CHANNELS` yields zero channels and `--live` exits 2 rather than
defaulting to "all channels". `make marble_live` asserts that exit.

`--selftest` (14 checks) covers allowlist rejection, shell/JSON escaping, and
message parsing offline, so the bridge is gated without a token.

### Gates

| Command | Marker |
|---|---|
| `make marble_live_wiring` | `MARBLE_LIVE_WIRING_PASS` (16) |
| `./bin/cnet_discord_peer --selftest` | `DISCORD_PEER_SELFTEST_PASS` (14) |
| `make marble_live` | `MARBLE_LIVE_PASS` |
| `make third_way_peer` | `THIRD_WAY_PEER_PASS` (27) |
