# Teach path (Teacher → miss_log → gold → CERT)

## Intended loop

```text
User ask
  → CERT hit?  LOCAL (CNET sealed answer)
  → else Teacher draft (UNTRUSTED, full answer to user)
       → miss_log learnable=true auto_cert=false
       → later gold_curriculum_harvest + reviewer
       → pack_personal / CERT  (then CNET answers itself next time)
```

Probes still short-circuit (no Teacher burn).

## Defaults

| Env | Default | Meaning |
|-----|---------|---------|
| `CNET_TEACHER_ON_MISS` | `1` | Teacher teaches on organic miss |
| `CNET_SELF_ANSWER` | `1` | CNET template if Teacher off/fail/probe |
| `CNET_NEVER_VOICE_LLM` | `1` | TTS only LOCAL/CNET lines |
| `ROE_LLM_NUM_PREDICT` | `1024` | avoid cut-off drafts |
| `ROE_ANSWER_MAX` | `4096` | C buffer for full drafts |

## Cut-off fixes

- Answer buffer **512 → 4096**
- `num_predict` **120/280 → 1024** (teach)
- Prompt: finish the thought, complete coverage
- Timeout 180s for cloud models

## Disable Teacher

```bash
CNET_TEACHER_ON_MISS=0
systemctl --user restart cnetd
```
