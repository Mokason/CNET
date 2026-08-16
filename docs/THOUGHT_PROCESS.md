# Token-free thought process

Not LLM chain-of-thought. Not consciousness.  
**Structured operational thinking at 0 tokens** — same class as LOCAL CERT.

## Pipeline

```text
OBSERVE → AFFECT (DA/5HT/ADO) → INTEND → ROUTE → ACT → VERIFY → CONSOLIDATE
```

| Step | Source (no LLM) |
|------|-----------------|
| OBSERVE | query, route match, KPI |
| AFFECT | neuromod_state + persona affect |
| INTEND | rules (identity / prefer_local / consolidate / pack) |
| ROUTE | ROUTES.jsonl longest pattern |
| ACT | plan list (load pack, teacher gate, never_self_cert) |
| VERIFY | law checklist |
| CONSOLIDATE | REST / SORT / REMEMBER / HOLD from neuromod |

## Commands

```bash
make cnet_thought
python3 scripts/cnet_thought_process.py --query "who are you"
cat logs/governor/thought_last.json
# chain only:
python3 scripts/cnet_thought_process.py --query "format-truncation" | head -1
```

## Integration

- **Autonomous cycle** — thinks before each probe (persist log)
- **Front door** — prints `thought: …` chain after ask (disable: `ROE_NO_THOUGHT=1`)

## Law

- `tokens: 0` · `llm: false`
- never self-CERT · not AGI · not consciousness
- thoughts are **logs of control**, not truth claims
