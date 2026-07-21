# Next-5 implementation (2026-07-21)

## 1 Serve path
- `cnet_use_skill` / `cnet_list_skills` MCP tools
- `bin/serve_named_skills` → **SERVE_NAMED_SKILLS_PASS** (8/8 certified, reli~0.67)
- `scripts/serve_named_skills.sh`, `artifacts/janitor/SKILL_MAP.md`

## 2 Skill quality
- `chunk_*` quality seeds queued (juice, core verb, testimony, hit pipeline)
- `artifacts/janitor/skill_quality_seeds.md`

## 3 Residual + reliability
- `CNET_SOUL_RESIDUAL_PREFER_HERMETIC=1` (avoid dual GGUF fight)
- Serve path raises reliability (serve exercise → milli 667)

## 4 Content factory
- `scripts/content_factory.sh` — Obsidian/research scan + chunk seeds

## 5 Hermes UX
- Weekly timer: brief + skill map + pin prune + serve report (Sun 10:00)
- Skill map for when to call which unit

## Verify
```bash
make serve_named_skills
bash scripts/write_skill_map.sh
# after gateway recycle:
# cnet_list_skills / cnet_use_skill
```
