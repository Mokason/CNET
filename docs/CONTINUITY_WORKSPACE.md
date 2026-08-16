# Continuity workspace

**Imitates continuity of control and self-report. Does not instantiate consciousness.**

## What it binds (0 LLM tokens)

| Block | Content |
|-------|---------|
| **body** | DA / 5HT / ADO + persona affect |
| **mind** | token-free thought chain + intent |
| **place** | query, pack, skill, source |
| **story** | who (SOUL one-liner), HAVE / MISS inventory |
| **sleep** | wake / drowsy / consolidate from ADO |

## One line

```text
continuity: marble | DA0.52 5HT0.50 ADO0.54 | ctrl>imp | doing: identity_local @ pack_soul_marble/soul_who | have: … | miss: … | sleep: wake | law: never_self_cert · not_conscious · not_agi
```

## Commands

```bash
make cnet_continuity
python3 scripts/cnet_continuity.py --query "who are you"
cat logs/governor/continuity_line.txt
```

## Integration

- Front door ask → prints continuity line + thought chain  
- Autonomous cycle → `continuity_line` in `AUTONOMOUS_CYCLE.json`  
- Disable thought/continuity chatter: `ROE_NO_THOUGHT=1`

## Law (hard)

```json
{
  "not_conscious": true,
  "not_agi": true,
  "never_self_cert": true,
  "continuity_is_not_cert": true
}
```

Continuity is for **ops UX and control**.  
It is **never** a promote/CERT input by itself.
