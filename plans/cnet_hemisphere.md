# CORE middle ground — "what is what"

Status: **CORE ≠ AGI — CORE = ARBITER — CERT logic-strong — LLM creativity-strong**

## Doctrine

```
CORE is not competence-as-AGI.
CORE handles what is what:

  CERT plane     → logic-strong   (exact, math, wiki, capsules)
  OPEN CHAT mind → creativity-strong (draft, invent, poem, story)

  claimed_cert only on CERT binds.
  open chat never auto-CERTs.
```

```
                         USER TURN
                             │
                             ▼
                 ┌───────────────────────┐
                 │   CORE  (discern)     │  middle ground
                 │   intent = LOGIC |    │  not AGI mouth
                 │            CREATIVE | │
                 │            MIXED      │
                 └───────────┬───────────┘
                     │               │
          CERT first │               │ then (if allowed)
     logic-strong    │               │ creativity-strong
     claimed_cert=1  │               │ claimed_cert=0
```

## Routing (“what is what”)

| Intent | After CERT try |
|--------|----------------|
| **LOGIC** | bind CERT, or **abstain** (default: no creative fill-in) |
| **CREATIVE** | CERT if exact hit, else **open chat** draft |
| **MIXED / UNKNOWN** | CERT then open chat if enabled |

Discern is a **cheap heuristic** (`cnet_core_discern`) — no LLM in the router.

## API

```c
CnetCoreIntent i = cnet_core_discern(turn);  /* LOGIC | CREATIVE | MIXED | UNKNOWN */
cnet_core_ask(turn, &policy, &r);
/* r.via_core=1  r.intent=…  r.plane=CERT|OPEN_CHAT  r.claimed_cert=… */
```

Env:

| Var | Default | Meaning |
|-----|---------|---------|
| `CNET_LOGIC_OPEN_CHAT_FALLBACK` | off | if 1, logic miss may use open chat |
| `CNET_CORE_OPEN_CHAT` | on | enable creativity plane |
| `CNET_NEVER_VOICE_LLM` | 1 | open chat not voiced |
| `CNET_OPEN_CHAT_MAY_VOICE` | 0 | rare voice for drafts |

## Gate

```text
make cnet_hemi
logic_strong=1 creative_strong=1 discern=1 core_middle=1 residual_never_cert=1
```

## RLM outer host

`cnet_rlm` wraps CORE + both planes with a bounded step budget:

```text
user → RLM → CORE → CERT | OPEN_CHAT
```

- API: `include/cnet_rlm.h` · `make cnet_rlm`
- Live: `cnetd` uses `cnet_rlm_ask` as the primary ask path
- Plan: `plans/cnet_rlm.md`

## Default residual (2026-08-16)

- **Default OPEN_CHAT / held:** `config/cnet-bonsai-held.env` → Bonsai-8B GPU `:8081`
- **Optional MAX:** `config/cnet-max-held.env` → Qwen7B float32 `:8000`
- E2E: `make cnet_core_e2e` / `scripts/cnet_core_e2e_smoke.sh`
