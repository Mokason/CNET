# Personal AI — Local Library First, Big AI On Call

**Place:** The serving policy above SoulHost / gap_lane: who answers a request.

**Dilemma:** Defaulting every query to a datacenter model wastes cost and
privacy; never escalating leaves permanent holes.

**Consequence:** One explicit policy loop:

1. **Local first** — certified plan over the personal CNB library.
2. **Big AI on call** — only on no-plan: teacher oracle may answer *and*
   the miss is noted for the gap lane.
3. **Teach later / next tick** — budgeted mine+seal turns help into a local skill.
4. **Abstain** when no plan and no teacher — never invent.

Hermetic gate: `make personal_ai` → `PERSONAL_AI_PASS`.

## Result

**H1 (hermetic, 2026-07-17):**

```
make personal_ai → PERSONAL_AI_PASS (13)
  local certified unit serves without teacher
  novel goal → teacher help + gap noted
  tick seals unit → subsequent serve is local
  no teacher → abstain + gap
```

API: `personal_ai_open` / `bind_teacher` / `serve` / `tick` / `close`  
Env: `config/personal-ai.env`

## Making it automatic (no babysitter)

Two long-running pieces share one base + inbox:

| Process | Unit / script | Job |
|---|---|---|
| **Serve** | Hermes MCP via `scripts/deploy_hermes_mcp.sh` | Local units first; `CNET_GAP_INBOX` on miss; health tick |
| **Learn** | `cnet-personal-ai-lane.service` | Inbox → teach from teacher GGUF → seal CNB; idle teacher sleep |

One command:

```bash
# learner only
scripts/personal_ai_auto.sh start

# learner + Hermes serve (full loop)
SERVE=1 BASE_PATH=/path/to/soul.cnb TEACHER=/path/to/model.gguf \
  scripts/personal_ai_auto.sh start

scripts/personal_ai_auto.sh status
scripts/personal_ai_auto.sh doctor
```

Stop: `scripts/personal_ai_auto.sh stop`  
Gate: `make personal_ai_auto` → `PERSONAL_AI_AUTO_PASS`

New units appear on the serve side when MCP children recycle (Hermes) or on
process reopen — the lane never rewrites a running host’s memory in place.
