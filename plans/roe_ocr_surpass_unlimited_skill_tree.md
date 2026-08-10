# Skill tree: ROE vs Unlimited-OCR (2026-08-08)

Opponent: **Baidu Unlimited-OCR** (~3B-A0.5B MoE), OmniDocBench v1.5 **~93.23%** SOTA
(+6 pts vs DeepSeek-OCR), long-doc one-pass, flat long-output speed (R-SWA).

Run: `make roe_asi_ocr_tree` → `ROE_ASI_OCR_TREE_PASS`  
Artifact: `artifacts/roe_ocr_skill_tree/`

## Axes of "surpass"

| Axis | Need to win | ROE now | Realistic? |
|---|---|---|---|
| **Quality** (OmniDoc) | layout+table+VLM+harness | 🔒 0 | Hard moonshot |
| **Long-doc speed** | R-SWA / streaming state | 🔒 0 | Hard |
| **Cost / offline** | local CERT + miss-only teacher | 🟡 1 | **Yes — ROE path** |
| **Audit / law** | CERT, taint, fail-closed | 🟡 2 | **Yes — ROE ahead** |
| **Product system** | goal map + packs + hybrid | 🟡 1 | **Yes — ROE path** |

## Tree snapshot (now)

- ✅ `rec_hermetic` 3/3 — hermetic OCR floor (`ROE_ASI_OCR_PASS`)
- 🟡 shell/tidy/capsule/audit started
- 🔒 layout, tables, multi-page, OmniDoc harness, quality beat

## Critical path

1. `perc_render` — PDF→image  
2. `rec_vlm` — live Unlimited teacher (weights already under `~/AI/Models/baidu-Unlimited-OCR`)  
3. `ver_gold` — verify before CERT  
4. `perc_layout` / `perc_table`  
5. `bench_omni`  
6. `ver_proj` — corpus templates → free local hits  
7. `beat_cost` / `beat_system`  
8. `beat_quality` only if chasing SOTA

## Doctrine

**Don't clone Unlimited. Use it as teacher. Beat it as a system.**
