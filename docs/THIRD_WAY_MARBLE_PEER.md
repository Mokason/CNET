# Third way: Marble peer (not MCP monobrain, not AGI)

Date: 2026-08-19  
Repo: `/home/marble/AI/CNET`

## Intent

CNET should **chat, track a sentence, carry a personality**, and **talk to Hermes as itself** — not as an MCP tool dump and not as a second monobrain that self-CERTs.

This is **ASI = Artificial Specialized Intelligence** under verifiers.  
Not superintelligence. Not “kill all humans.” Not AGI cosplay.

## Third way (doctrine)

```text
                    ┌─────────────────────────┐
  Human / Hermes ──►│  PEER socket (cnetd)    │  ← identity path
                    │  Marble voice + packs   │
                    └───────────┬─────────────┘
                                │
              always CERT-first (soul, law, skills, bricks)
                                │
              miss ──► dialog + self-utterance (persona)
                                │
              optional residual OPEN_CHAT draft
                   claimed_cert=0  never_self_CERT
                   tables/gold may promote later
```

| Path | Role |
|---|---|
| **MCP CnetMcpServer** | Factory / SoulHost tools / health — **not** the chat mouth |
| **cnetd PEER/ASK** | Live Marble self Hermes (and user) talk to |
| **CERT packs** | Truth + identity skills (floors never lowered) |
| **OPEN_CHAT residual** | Warmth / paraphrase only; **auto_cert=false** |
| **Governor / evolve** | Unattended garden; gold/multi_stable only |

## Safety rails (non-negotiable)

1. Residual / open chat **never** auto-CERTs.  
2. Logic miss does **not** fill with creative mouth by default.  
3. Persona biases **delivery**, not certification floors.  
4. No second monobrain (“second_brain: 0” on soul pack).  
5. Hermes is a **peer client**, optional; CNET runs without Hermes.

## Gap found (2026-08-19)

- `pack_soul_marble` exists (who/oath/hermes pattern).  
- Soft questions like “how do you feel about Hermes?” → generic `utter_self` miss.  
- In `cnetd.c`, teacher/open-chat residual is **hard-killed** (`teacher_on_miss = 0` always; RLM OPEN_CHAT → `open_chat_answer_killed`).  
- Hermes default integration is **MCP**, not PEER socket.

## Build sequence

1. **PEER protocol** on cnetd: `PEER <from> <query>` + session dialog (C).  
2. **C client** `bin/cnet_peer` (no Python sock client for product).  
3. **Persona CERT skills** for peer relationship + feelings (sealed lines).  
4. **Utterance bank** Marble-flavored miss lines (delivery).  
5. **Optional residual OPEN_CHAT** only behind env, `claimed_cert=0`, never voice-as-CERT.  
6. Hermes: prefer `cnet_peer` / sock for chat; MCP stays tool factory.

## Success proofs

```bash
bin/cnet_peer "who are you"                 # LOCAL soul_who
bin/cnet_peer "how do you work with Hermes" # LOCAL persona skill
bin/cnet_peer PING                          # PONG
# Hermes chat path uses PEER socket, not MCP mouth
```

Markers: `THIRD_WAY_PEER_PASS` when gate exists.
