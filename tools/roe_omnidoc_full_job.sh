#!/usr/bin/env bash
# Former torch OmniDoc full-infer job. Product Python / .venv-unlimited-ocr purged (1A+2A).
# Portable OCR proof is make roe_asi_ocr_* / C ROE paths. This job is WITHHELD, not a pass.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p logs
echo "ROE_OMNIDOC_FULL_JOB_WITHHELD reason=torch_harness_removed_use_roe_asi_ocr $(date)" | tee logs/roe_omnidoc_full_job.log
exit 1
