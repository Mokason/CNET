#!/usr/bin/env bash
# Durable OmniDoc full infer + CDM eval. Exit 0 only when report written.
set -euo pipefail
cd /home/marble/AI/CNET
export HIP_VISIBLE_DEVICES=0
export CUDA_VISIBLE_DEVICES=0
export PYTHONUNBUFFERED=1
export HF_HUB_DISABLE_PROGRESS_BARS=1
export PATH="/usr/local/bin:$PATH"

echo "[job] start $(date) preds=$(ls artifacts/omnidoc_full/predictions_unlimited 2>/dev/null | wc -l)"

.venv-unlimited-ocr/bin/python -u tools/roe_omnidoc_full_simple.py \
  --shard 0 --shards 1 --skip-existing \
  >> logs/roe_simple_single_full.log 2>&1

N=$(ls artifacts/omnidoc_full/predictions_unlimited | wc -l)
echo "[job] infer exit preds=$N $(date)"

if [ "$N" -lt 1600 ]; then
  echo "[job] BLOCKED low preds=$N"
  exit 2
fi

export PYTHONPATH=/home/marble/AI/CNET/third_party/OmniDocBench
export CDM_SAVE_VIS=0
.venv-unlimited-ocr/bin/python third_party/OmniDocBench/pdf_validation.py \
  --config artifacts/omnidoc_full/end2end_unlimited_full_cdm.yaml \
  2>&1 | tee logs/roe_omnidoc_full_cdm_eval.log

.venv-unlimited-ocr/bin/python - << 'PY'
import json
from pathlib import Path
src=Path('result/predictions_unlimited_quick_match_metric_result.json')
m=json.loads(src.read_text())
text_ed=m['text_block']['all']['Edit_dist']['ALL_page_avg']
cdm=m['display_formula']['all']['CDM']['all']
teds=m['table']['all']['TEDS']['all']
text_s=(1-text_ed)*100
teds_s=teds*100
cdm_s=cdm*100
overall=(text_s+teds_s+cdm_s)/3
n=len(list(Path('artifacts/omnidoc_full/predictions_unlimited').glob('*.md')))
rep={
  "n_preds": n,
  "text_score": text_s,
  "table_TEDS_x100": teds_s,
  "formula_CDM_x100": cdm_s,
  "Overall": overall,
  "sota_claim": "WITHHELD" if n < 1645 else "COMPUTED_NOT_LEADERBOARD_SUBMITTED",
}
Path('artifacts/omnidoc_full/OFFICIAL_FULL_CDM_REPORT.json').write_text(json.dumps(rep, indent=2))
print('OMNIDOC_FULL_JOB_DONE')
print(json.dumps(rep, indent=2))
PY
