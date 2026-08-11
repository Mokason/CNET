# Explore tick — offline structured creativity

## Law

```text
idle (ADO headroom) → mutate CERT seeds → draft (synthetic|Teacher)
  → sandbox verify → curriculum (auto_cert=false)
  → gold | multi_stable+reviewer only later
NEVER → pack_personal direct
```

## Commands

```bash
python3 tools/roe_explore_tick.py           # respect neuromod idle gate
python3 tools/roe_explore_tick.py --force   # ignore ADO gate
python3 tools/roe_explore_tick.py --teacher # optional Ollama drafts
make roe_explore_tick
```

## Outputs

| File | Role |
|------|------|
| `artifacts/.../curriculum_explore.jsonl` | explore-only queue |
| `curriculum_harvest.jsonl` | merged hints (`auto_cert:false`) |
| `EXPLORE_TICK.json` | last tick report |

## Gate

- High ADO / `defer_new_explore` / high loadavg → **defer** (still PASS)
- Blocklist on query/draft
- C: `gcc -fsyntax-only`; bash: `bash -n`
- Prose drafts need operational keywords
