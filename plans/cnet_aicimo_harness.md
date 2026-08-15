# AICIMO HARNESS LANE — EXACT NEVER ESCALATES

Status: **SLICE — PORTED LAW, NOT AICIMO RUNTIME — OPEN CHAT WITHHELD**

This is not open chat. This is not F11. This is not a Bonsai convert.
Broader / general-mind stays **WITHHELD**.

AISIMOS = AICIMO. The law comes from
`docs/tiny-model-skill-call-demos.md` and commit `5f7b374`
(skill-route-server: deterministic exact lane — arithmetic never escalates).
CNET does **not** port the C# / .NET / Python runtime.

## Ported law

1. **Model-as-router + deterministic-skills-as-content.** The tiny LM never
   produces the substantive output. It only names a skill. The dispatcher
   runs the skill. In CNET the router is an explicit skill-name dispatch
   table (`src/cnet_skill_lane.c`), not compete WordLM and not
   `residual_gguf_oracle`.
2. **Exact lane never escalates.** If a capsule or lookup binds, that value
   is the answer. `residual_calls` stays 0. Teacher / 8B does not run.
3. **OOD abstains.** Subject must appear in the turn; else refuse. Do not
   force a nearest skill. Do not escalate OOD to residual / teacher.
4. **Propose ≠ authority.** The harness proposes a capsule name
   (`increment_mod256`, `crc8_atm`, `web_lookup_v1`). Admit / cert is
   unchanged. Teacher never speaks.

## Teacher cut from this lane

`tools/cnetd.c:cd_ask` calls `cnet_skill_lane_cd_ask` first. Exact bind or
OOD abstain returns before `roe_set_net` / `enable_llm` / teacher-on-miss.
The teacher stack stays in the file. It is not reached on this lane.

## What this is not

- Not open chat. Not a chatbot win.
- Not F11. Floors / 448 / CHAT-1 are not claimed improved.
- Not a Bonsai convert.
- Not AICIMO `TryExactAnswer` C# grammar. Skills are certified capsule
  names / lookup hop / already-bound A slots.
- Broader / general-mind **WITHHELD**.

## Gate

```text
make cnet_harness
CNET_HARNESS_PASS
checks=94 residual=0 teacher=0 python=0 broader_claims=WITHHELD
```
