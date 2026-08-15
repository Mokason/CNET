# CAPSULE LOOP — BOUNDED CALL LOG

Status: **SLICE — STEAL DEEPSEEK LOOP SHAPE, NOT THEIR RUNTIME — OPEN CHAT WITHHELD**

This is not open chat. This is not F11. This is not a general mind.
Broader / general-mind stays **WITHHELD**.

Steal the while + stop enum + step budget. Do not steal Cordis, Code Mode,
npx dsh web, RLM host REPL, or teacher-on-miss.

## Ported shape

1. **Bounded call log.** At most `CNET_CAPSULE_LOOP_MAX_STEPS` (2)
   `CnetCapsuleCall` records beside `cnet_skill_lane`.
2. **Hard-switch route.** Reuse `cnet_skill_lane_route`. No soft mix.
   No nearest-skill force. Do not fork a second exact-lane law.
3. **Monotonic pre-execute.** Permission is a closed enum:
   allow / abstain / deny. Deny cannot become allow. No ask-the-8B.
4. **Existing bind.** increment / crc8 / lookup stay in `cnet_skill_lane`.
   Exact bind never escalates. `residual_calls` stays 0. Teacher does not run.
5. **OOD abstains.** bake bread / fix the repo / 41 plus 1 without a subject
   → `ood_no_skill`. Residual hop → `residual_mouth` (not exact).
   Unverified fixture → abstain.
6. **Two-step in-scope.** "increment 41 then crc8" → two EXACT records,
   last A spoken, teacher_calls=0. A third named skill stops at 2.
7. **Propose ≠ authority.** The loop names a capsule. Admit / cert is unchanged.

## Teacher cut from this lane

`tools/cnetd.c:cd_ask` tries `cnet_capsule_loop_cd_ask` first when the turn
names two table skills, else `cnet_skill_lane_cd_ask`. Exact bind or
OOD abstain returns before `roe_set_net` / `enable_llm` / teacher-on-miss.
The teacher stack stays in the file. It is not reached on this lane.

## What this is not

- Not DeepSeek Harness. Not Claude Code. Not aider. Not RLM-in-WordLM.
- Not open chat. Not a chatbot win. Not F11.
- Not auto-CERT. claimed_cert=1 only after exact bind.
- Not a process sandbox. Permission is the seam.
- Broader / general-mind **WITHHELD**.

## Gate

```text
make cnet_capsule_loop
CNET_CAPSULE_LOOP_PASS
residual=0 teacher=0 python=0 broader_claims=WITHHELD
steps<=2 exact_never_escalates=1 ood_abstains=1
```
