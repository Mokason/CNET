# Personal AI ops scheduler

**Place:** Operator automation above learner + serve + JTC.

**Dilemma:** Manual doctor/restart/seal does not scale; failures happen while you are away; unknown questions need an external brain (Hermes) without faking certification.

**Consequence:** A 15‑minute oneshot timer runs `personal_ai_ops_tick.sh`:

1. Heal (rebuild `gap_lane_run`, restart learner, optional Hermes restart)
2. Ensure `json_toolcall_v0` sealed when missing
3. Drain a small TODO queue (`logs/personal_ai_ops/todo.jsonl`)
4. On remaining issues, ask Hermes (`hermes chat -q …`) and store lessons
5. Write `logs/personal_ai_ops/last_tick.json`

## Install

```bash
scripts/personal_ai_ops_tick.sh install
# or
scripts/personal_ai_auto.sh ops-install

systemctl --user list-timers cnet-personal-ai-ops.timer
scripts/personal_ai_ops_tick.sh status
```

Uninstall: `scripts/personal_ai_ops_tick.sh uninstall`

## TODO queue

Append JSONL lines:

```bash
echo '{"ts":"'"$(date -Iseconds)"'","id":"t1","status":"open","priority":1,"text":"jtc seal if missing"}' \
  >> logs/personal_ai_ops/todo.jsonl
```

Heuristics: `jtc`/`json_tool` → seal; `learner` → start; `observe` → metrics; else Hermes.

## Hermes learn path

```bash
scripts/personal_ai_ops_ask_hermes.sh "Why is the personal AI learner inactive?"
# → logs/personal_ai_ops/lessons/lesson_*.md
```

Env: `config/personal-ai-ops.env` (`OPS_ASK_HERMES`, `OPS_RESTART_HERMES`, …).

## Safety

- Default **does not** restart Hermes gateway (`OPS_RESTART_HERMES=0`)
- Learner restart and JTC seal are on by default
- Min interval 300s soft throttle + lock file
