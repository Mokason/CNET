#!/usr/bin/env bash
# Qwythos specialist conversion — ONE process, full schedule (no per-family reload).
#
# 1) Single load + capture + schedule (all families)
# 2) Import prior LDTR → skip already-packed specialists (resume-safe)
# 3) Lean STE defaults (do not inflate steps/calib unless quality needs it)
# 4) Checkpoint export after each schedule stage
#
# Usage:
#   nohup bash scripts/qwythos_ladder_campaign.sh \
#     > logs/qwythos_ladder_campaign.out 2>&1 &
#   bash scripts/qwythos_ladder_status.sh
#   tail -f logs/qwythos_ladder_campaign.log
#
# Env overrides:
#   MODEL  PACK  LOG  STEPS  N_CALIB  N_HOLDOUT  CAL_SEQS  CERT  MAX  MODE
set -uo pipefail
cd "$(dirname "$0")/.."
mkdir -p logs artifacts

MODEL="${MODEL:-hermes_wrappers/Qwythos-9B-Claude-Mythos-5-1M-MTP-Q4_K_S-recovery.gguf}"
PACK="${PACK:-artifacts/qwythos_ladder_campaign.ldtr}"
LOG="${LOG:-logs/qwythos_ladder_campaign.log}"

# Lean STE — only raise if quality requires it
STEPS="${STEPS:-32}"
N_CALIB="${N_CALIB:-48}"
N_HOLDOUT="${N_HOLDOUT:-16}"
CAL_SEQS="${CAL_SEQS:-3}"
CERT="${CERT:-0.56}"
MAX="${MAX:-512}"          # whole forest
MODE="${MODE:-ste}"

if [[ ! -x bin/cnet_spec_ladder ]]; then
  make -s spec_ladder_tool || { echo "build failed"; exit 1; }
fi
if [[ ! -f "$MODEL" ]]; then
  echo "MODEL not found: $MODEL" | tee -a "$LOG"
  exit 1
fi

{
  echo "============================================================"
  echo " Qwythos ladder campaign (single-process schedule)"
  echo " $(date -Iseconds)"
  echo " model=$MODEL"
  echo " pack=$PACK"
  echo " mode=$MODE steps=$STEPS cert=$CERT max=$MAX"
  echo " n_calib=$N_CALIB holdout=$N_HOLDOUT cal_seqs=$CAL_SEQS"
  echo " gpu=1 opencl  import=$( [[ -f "$PACK" ]] && echo yes || echo no )"
  echo "============================================================"
} | tee -a "$LOG"

t0=$(date +%s)
CNET_INFER_FP=1 \
CNET_FOREST_NO_PERSIST=1 \
CNET_MAX_CTX=128 \
CNET_GPU=1 \
CNET_GPU_BACKEND=opencl \
CNET_GPU_MIN_FLOPS=1 \
CNET_LADDER_THREADS="${CNET_LADDER_THREADS:-24}" \
CNET_LADDER_PARALLEL="${CNET_LADDER_PARALLEL:-2}" \
CNET_LADDER_MAX="$MAX" \
CNET_LADDER_STEPS="$STEPS" \
CNET_LADDER_N_CALIB="$N_CALIB" \
CNET_LADDER_N_HOLDOUT="$N_HOLDOUT" \
CNET_LADDER_CAL_SEQS="$CAL_SEQS" \
CNET_LADDER_CERT="$CERT" \
CNET_LADDER_EXPORT="$PACK" \
CNET_LADDER_IMPORT="$( [[ -f "$PACK" ]] && echo "$PACK" || true )" \
./bin/cnet_spec_ladder "$MODEL" schedule "$MODE" 2>&1 | tee -a "$LOG"
rc=${PIPESTATUS[0]}
t1=$(date +%s)

{
  echo ""
  echo "============================================================"
  echo " CAMPAIGN_DONE  $(date -Iseconds)  wall=$((t1 - t0))s  rc=$rc"
  echo " pack=$PACK"
  [[ -f "$PACK" ]] && ls -lh "$PACK"
  grep -E 'LADDER_SUMMARY|LADDER_PASS|LADDER_SOFT|checkpoint export|stage ' "$LOG" | tail -50
  echo "============================================================"
} | tee -a "$LOG"

exit "$rc"
