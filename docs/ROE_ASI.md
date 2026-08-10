# ROE-ASI (concept + live)

**Regulated Open-Ended Artificial Specialized Intelligence**

```
query
  → LOCAL certified skill?     → answer (≈0 tokens)
  → static LOOKUP corpus?
  → mock TEACH curriculum?
  → LIVE lookup (DuckDuckGo)?  → untrusted
  → LIVE LLM (Ollama)?         → untrusted
  → ABSTAIN / ask user
verify votes (≥2 or --accept×2) → promote → save pack
```

## Commands

```bash
cd /home/marble/AI/CNET

# Offline concept gates
make roe_asi
make roe_asi_train
make roe_asi_live
make roe_asi_cli

# Live Ollama teacher (qwen2.5:7b on :11434)
export ROE_LIVE=1
export ROE_LLM_MODEL=qwen2.5:7b   # default
export ROE_LLM_URL=http://127.0.0.1:11434/api/generate

./bin/roe_asi_cli train --catalog artifacts/roe_catalog
./bin/roe_asi_cli ask "what is gravity?" --live --catalog artifacts/roe_catalog --accept
# second accept promotes; third ask should be LOCAL

./bin/test_roe_asi_live --live
```

## Token economics (measured)

| Mode | Hit | Save vs all-LLM |
|---|---:|---:|
| Offline train epoch 1 | ~52% | ~28% |
| Offline train epoch 2–3 | ~78% | ~**94%** |
| After promote, local hit | 100% on that query | ~100% for that skill |

Live: one Ollama call only on miss; later turns free.

## Persistence (ROE text capsules)

```
artifacts/roe_catalog/
  catalog.jsonl
  skills/<id>/SKILL.roe
  skills/<id>/manifest.roe
```

Not CNU1 BTN (text skills). Same shell law: load only via ROE; still fail-closed.

## Env

| Var | Meaning |
|---|---|
| `ROE_LIVE=1` | enable live LLM + lookup |
| `ROE_LLM=1` / `ROE_LOOKUP=1` | finer enable |
| `ROE_LLM_URL` | default Ollama generate |
| `ROE_LLM_MODEL` | default `qwen2.5:7b` |
| `ROE_TIMEOUT_MS` | HTTP timeout |

## Basic coding curriculum

```bash
make roe_asi_coding
# catalog → artifacts/roe_coding_catalog (15 skills after train)
./bin/roe_asi_cli ask "python for loop example" --catalog artifacts/roe_coding_catalog
./bin/roe_asi_cli ask "function def syntax" --catalog artifacts/roe_coding_catalog
```

Measured: epoch1 teach/promote → epoch2–3 **~92% hit, ~98% token save**; OOD still abstains.

Skills include: hello world, variables, if/else, for/while, functions, list/dict/str,
files, try/except, class, recursion, booleans (+ lookups for Big-O / compiler).

## Debug L1–L3 (project memory)

```bash
make roe_asi_debug_l3          # ROE_ASI_DEBUG_L3_PASS
make roe_asi_debug_cli

# After train, project-scoped recall (0 tokens):
./bin/roe_asi_debug_cli turn --project proj_webapi \
  --tb "ImportError: cannot import name 'helper' from partially initialized module"

# Learn a new project bug (shell verify):
./bin/roe_asi_debug_cli verify --project myapp \
  --tb "KeyError: 'timeout'" \
  --fix "cfg.setdefault('timeout', 30)" --tests-pass
```

| Level | Behavior |
|---|---|
| L1 | Global error recipes (NameError, TypeError, …) |
| L2 | Checklists (pytest, bisect, debug steps) |
| L3 | **project_id + error signature → certified fix** |

Measured soak: **L3 rate 100%, token save 100%** on recurring project bugs after verify.
Catalog: `artifacts/roe_debug_catalog/project_memory.jsonl`

## Goal map + microsplit (tidy cat/sub capsules)

```text
goal → microsplits (ordered)
     → each split → category/subcategory
     → HAVE knowledge → local capsule
     → MISS → learn → tidy slot cat/sub
```

```bash
make roe_asi_goal
make roe_asi_goal_cli
./bin/roe_asi_goal_cli "learn basic coding"
./bin/roe_asi_goal_cli "build web api with tests"
./bin/roe_asi_goal_cli "debug traceback with pytest"
```

First run: HAVE + LEARN. Second run: all HAVE, ~100% token save.  
Layout: `artifacts/roe_goal_catalog/capsules/<cat>/<sub>/...`

## OCR (vision capsule lane)

Hermetic **5×7 glyph OCR** (render → template match), tidy under `vision/ocr|pipeline|pdf`.
PDF text layer via `pdftotext` when available. Unlimited-OCR remains external teacher path.

```bash
make roe_asi_ocr   # ROE_ASI_OCR_PASS
# catalog → artifacts/roe_ocr_catalog/capsules/vision/...
./bin/roe_asi_goal_cli "ocr hermetic pipeline" --catalog artifacts/roe_ocr_catalog
```

Measured: curriculum **100% exact** on hermetic alphabet, phrases promoted, local serve 0 tokens.

## Surpass Unlimited (teacher → capsules → bench)

```bash
make roe_asi_ocr_surpass   # ROE_ASI_OCR_SURPASS_PASS
# teacher skills: python3 tools/roe_unlimited_teacher.py skills
# live hooks: ROE_UNLIMITED_URL=... or ROE_UNLIMITED_LOCAL=1 (needs torch)
```

Measured hybrid vs Unlimited-only on covered corpus:
- **cost save ~95%**, warm local **40/40**, quality **match 1.0**
- audit **CERT > opaque**
- OmniDoc external SOTA: **WITHHELD** until live GPU VLM run
- Catalog: `artifacts/roe_ocr_surpass/capsules/vision/{unlimited,layout,table,...}`

## OmniDoc live SOTA confirmation (Unlimited teacher on GPU)

Live Baidu Unlimited-OCR on AMD R9700 (ROCm), stratified OmniDocBench slice.

```bash
# requires .venv-unlimited-ocr (ROCm torch + transformers 4.57)
make roe_omnidoc_sota
# report → artifacts/omnidoc_sota_run/omnidoc_sota_report.json
```

Measured (12-page slice, live VLM):
- Unlimited text_acc ≈ 0.62 (vs GT reading-order; metric is rough SequenceMatcher)
- ROE hybrid **quality match 1.0** (teacher CERT cache)
- **cost beat** two-pass Unlimited-only (save 50%+ on cold+warm)
- **composite system beat** (quality + cost + audit)
- Full 1651 leaderboard OmniDoc table: still **WITHHELD_SLICE_ONLY** (not claimed as published SOTA number)

### Quality improve (metric + dual OCR)

```bash
./.venv-unlimited-ocr/bin/python tools/roe_omnidoc_quality_improve.py --rescore
# optional: --reocr  (gundam+base pick-best)
```

| Metric | Old naive | After improve |
|---|---:|---:|
| page_acc | 0.62 | **0.87** |
| block_acc (OmniDoc-ish) | — | **0.88** |
| text_block_acc | — | **0.88** |

Dual-pass OCR ≈ same quality as single (teacher ceiling). Further gains need official OmniDoc end2end (TEDS/CDM) + formula/table paths.

## OCR asset moat (highest leverage — not second brain)

```bash
make roe_asi_ocr_asset   # ROE_ASI_OCR_ASSET_PASS
```

| Piece | What |
|---|---|
| **Doc L3** | `(corpus_id + page_sig) → CERT body`; cross-corpus isolation; no silent CERT |
| **Pack ABI** | `ROE_OCR_PACK` v1 export/import; bad packs fail-closed |
| **$/page freeze** | 100 pages / 10 unique → **$0.20 → $0.02/page** (90% save) |

Artifacts:
- `artifacts/roe_ocr_asset/pack_export/{PACK.abi,doc_l3.jsonl,MANIFEST.txt}`
- `artifacts/roe_ocr_asset/dollar_per_page_freeze.json`

Claim: **better asset than bare Unlimited** — teacher on miss only, CERT packs, lower $/page, fail-closed import. `second_brain: false`.

## Mostly without teacher (local-first)

```bash
make roe_asi_ocr_local   # ROE_ASI_OCR_LOCAL_PASS
```

Router order: **pdftotext → L3 memory → classic OCR (if any) → teacher**

| KPI (100 pages, 5% novel) | Value |
|---|---:|
| teacher_rate | **5.0%** |
| kpi_teacher_under_10pct | **true** |
| holdout known templates | **20/20 local** |

Freeze: `artifacts/roe_ocr_local/teacher_rate_kpi.json`  
Stronger `page_sig`: text head + newlines/pipes/len bucket + FNV hash.

## Tables (CSV / TSV / XLSX)

```bash
make roe_asi_ocr_tables   # ROE_ASI_OCR_TABLES_PASS
python3 tools/roe_table_extract.py path/to/file.xlsx   # → TSV stdout
```

| Format | How |
|---|---|
| CSV/TSV | Native C parse |
| XLSX | `roe_table_extract.py` (stdlib zip+xml) |
| XLS | LibreOffice → CSV when available |

Output: markdown table + `SCHEMA: col:type,...` + L3 by header signature.  
Second open of same table = **L3 free**. Wired into `roe_doc_route`.

## Hard pages beat plain Unlimited

```bash
make roe_ocr_hard_beat   # or: .venv-unlimited-ocr/bin/python tools/roe_ocr_hard_beat.py
# → artifacts/roe_ocr_hard/hard_beat_report.json
```

Pipeline (same teacher, better packaging):
preprocess variants · multi-config (gundam/base) · `<|det|>` geometry reading-order
(column-aware for newspaper/multi-col) · formula cleanup · best-of ensemble

Measured hard subset (math / historical / double-col / newspaper):

| Metric | Plain Unlimited | ROE hard pipeline |
|---|---:|---:|
| mean block_acc | 0.813 | **0.848** (+3.5pp) |
| mean text_acc | 0.780 | **0.841** (+6.1pp) |
| math page example | 0.552 | **0.773** (+22pp) |

`second_brain: false` — still Unlimited underneath; selection + structure beats single-pass.

## Self-model (instrumented inventory)

Not consciousness — coverage + skills + health + gate-bound tree + goal HAVE/MISS.

```bash
make roe_asi_self          # ROE_ASI_SELF_PASS
make roe_asi_self_cli      # artifacts/roe_self_model/{SELF.abi,self_report.json}
./bin/roe_asi_cli ask "who are you"   # prints inventory_line
```

See `docs/ROE_SELF_MODEL.md`. Law: never self-CERT; `beat_quality` stays 0 without SOTA_CLAIM.

## Daily packs (separate — token reduction)

Ten isolated catalogs under `artifacts/roe_daily_packs/pack_*/`. Load **one domain pack** (+ optional always-on trio), not a monobrain soup.

```bash
make roe_daily_packs          # seed + ROE_DAILY_PACKS_PASS
# always-on: pack_roe_self, pack_goal_split, pack_toolcall_hermes
# on-demand: coding | debug | doc_ocr | git_pr | ops | pm | meta_gardener
```

Misses → append `miss_log.jsonl` (schema in tree). See `artifacts/roe_daily_packs/README.md`.

## Front door (selective load)

```bash
make roe_front_door
./bin/roe_front_door ask "who are you"
./bin/roe_front_door route "format-truncation"
./bin/roe_front_door bench
```

Loads always-on trio + at most one matched domain pack. MISSes append `artifacts/roe_daily_packs/miss_log.jsonl`.

## Ollama cloud teacher (`deepseek-v4-flash:cloud`)

No `DEEPSEEK_API_KEY`. Teacher goes through **local Ollama** cloud model.

```bash
# one-time: ensure Ollama sees the cloud model
ollama show deepseek-v4-flash:cloud

# env
set -a && . config/roe-teacher-ollama-cloud.env && set +a
make roe_teacher_cloud_smoke   # ROE_TEACHER_CLOUD_SMOKE_PASS

# live ask (miss → cloud teacher, untrusted until --accept verify)
./bin/roe_asi_cli ask "what is conformal abstention?" --live --catalog artifacts/roe_catalog
# promote only with gold / --accept (never self-CERT)
```

| Env | Value |
|-----|--------|
| `ROE_LLM_MODEL` | `deepseek-v4-flash:cloud` |
| `ROE_LLM_URL` | `http://127.0.0.1:11434/api/generate` |
| `ROE_LLM_THINK` | `0` (required — empty answers if think left on) |
| `ROE_LIVE` | `1` |

Law unchanged: LLM output is **untrusted** until shell verify / user_accept.
