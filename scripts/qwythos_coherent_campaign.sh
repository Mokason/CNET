#!/usr/bin/env bash
# Quality-gated Qwythos conversion: grow LDTR only while e2e 2+2 stays coherent.
#
# After each schedule stage that packs specialists, run GPU quality_eval.
# On FAIL: restore previous pack and skip remaining families for that stage's
# export (rollback). On PASS: keep pack and continue.
#
# Env:
#   MODEL PACK QUALITY_LOG STEPS CERT MAX CAL_SEQS N_CALIB N_HOLDOUT
set -uo pipefail
cd "$(dirname "$0")/.."
mkdir -p logs artifacts

MODEL="${MODEL:-hermes_wrappers/Qwythos-9B-Claude-Mythos-5-1M-MTP-Q4_K_S-recovery.gguf}"
PACK="${PACK:-artifacts/qwythos_coherent.ldtr}"
LOG="${LOG:-logs/qwythos_coherent_campaign.log}"
QLOG="${QLOG:-logs/qwythos_coherent_quality.log}"
STEPS="${STEPS:-96}"
CERT="${CERT:-0.42}"
MAX="${MAX:-512}"
N_CALIB="${N_CALIB:-64}"
N_HOLDOUT="${N_HOLDOUT:-24}"
CAL_SEQS="${CAL_SEQS:-4}"
MODE="${MODE:-ste}"

# Families in order (real qwen35 names only — aliases empty)
FAMILIES=(gate_proj up_proj down_proj o_proj k_proj v_proj q_proj)

if [[ ! -x bin/cnet_spec_ladder ]]; then make -s spec_ladder_tool || exit 1; fi
if [[ ! -x bin/cnet_quality_eval ]]; then make -s quality_eval || exit 1; fi
if [[ ! -f "$MODEL" ]]; then echo "MODEL missing: $MODEL" | tee -a "$LOG"; exit 1; fi

quality_gate() {
  local tag=$1
  {
    echo ""
    echo "---- quality gate ($tag) $(date -Iseconds) ----"
  } | tee -a "$LOG" | tee -a "$QLOG"
  CNET_GPU=1 CNET_GPU_BACKEND=opencl CNET_GOV_PROFILE=turbo \
  CNET_MAX_CTX=256 CNET_FOREST_NO_PERSIST=1 \
  CNET_QUALITY_NO_THINK=1 \
  CNET_QUALITY_PROMPT='What is 2+2? Answer with just the number.' \
  CNET_QUALITY_EXPECTED=4 \
  CNET_LADDER_IMPORT="$( [[ -f "$PACK" ]] && echo "$PACK" || true )" \
  ./bin/cnet_quality_eval "$MODEL" 24 2>&1 | tee -a "$QLOG" | tee -a "$LOG"
  grep -q 'QUALITY_EVAL_PASS' <(tail -5 "$QLOG")
}

{
  echo "============================================================"
  echo " Qwythos COHERENT conversion campaign  $(date -Iseconds)"
  echo " model=$MODEL"
  echo " pack=$PACK  cert=$CERT steps=$STEPS"
  echo " n_calib=$N_CALIB holdout=$N_HOLDOUT cal_seqs=$CAL_SEQS"
  echo " families=${FAMILIES[*]}"
  echo "============================================================"
} | tee "$LOG"

# Baseline without pack must pass
rm -f "$PACK"
if ! quality_gate baseline_fp; then
  echo "BASELINE_FP quality FAIL — cannot gate conversion" | tee -a "$LOG"
  exit 1
fi
echo "BASELINE_FP PASS" | tee -a "$LOG"

prev_bak=""
for fam in "${FAMILIES[@]}"; do
  {
    echo ""
    echo "==== family=$fam $(date -Iseconds) ===="
  } | tee -a "$LOG"

  # Snapshot pack before stage
  if [[ -f "$PACK" ]]; then
    prev_bak="${PACK}.bak"
    cp -f "$PACK" "$prev_bak"
  else
    prev_bak=""
  fi

  CNET_INFER_FP=1 CNET_FOREST_NO_PERSIST=1 CNET_MAX_CTX=128 \
  CNET_GPU=1 CNET_GPU_BACKEND=opencl CNET_GPU_MIN_FLOPS=1 \
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
  ./bin/cnet_spec_ladder "$MODEL" "$fam" "$MODE" 2>&1 | tee -a "$LOG"
  rc=${PIPESTATUS[0]}

  packed_now=0
  [[ -f "$PACK" ]] && packed_now=$(wc -c < "$PACK" || echo 0)

  if ! quality_gate "after_$fam"; then
    echo "QUALITY_FAIL after $fam — rollback" | tee -a "$LOG"
    if [[ -n "$prev_bak" && -f "$prev_bak" ]]; then
      mv -f "$prev_bak" "$PACK"
    else
      rm -f "$PACK"
    fi
    # continue other families? yes — try remaining (this family won't be in pack)
    continue
  fi
  echo "QUALITY_PASS after $fam (pack_bytes=$packed_now)" | tee -a "$LOG"
  rm -f "$prev_bak"
done

{
  echo ""
  echo "============================================================"
  echo " COHERENT_CAMPAIGN_DONE $(date -Iseconds)"
  if [[ -f "$PACK" ]]; then
    ls -lh "$PACK"
    # count written specialists via ladder re-import is heavy; report size
    echo " pack=$PACK present — quality gates passed for kept stages"
  else
    echo " pack=NONE (no specialists met cert+quality)"
  fi
  echo "============================================================"
} | tee -a "$LOG"

# Final dual prompts for notification evidence
CNET_GPU=1 CNET_GPU_BACKEND=opencl CNET_GOV_PROFILE=turbo \
CNET_MAX_CTX=256 CNET_FOREST_NO_PERSIST=1 \
CNET_QUALITY_NO_THINK=1 \
CNET_QUALITY_PROMPT='What is 2+2? Answer with just the number.' \
CNET_QUALITY_EXPECTED=4 \
CNET_LADDER_IMPORT="$( [[ -f "$PACK" ]] && echo "$PACK" || true )" \
./bin/cnet_quality_eval "$MODEL" 24 2>&1 | tee -a "$QLOG" | tee -a "$LOG"

if grep -q 'QUALITY_EVAL_PASS' <(tail -3 "$QLOG") && [[ -f "$PACK" ]]; then
  echo "QWYTHOS_COHERENT_AND_CONVERTED=1 pack=$PACK" | tee -a "$LOG"
  exit 0
elif grep -q 'QUALITY_EVAL_PASS' <(tail -3 "$QLOG"); then
  echo "QWYTHOS_COHERENT_FP_ONLY=1 (no pack survived cert+quality)" | tee -a "$LOG"
  exit 2
else
  echo "QWYTHOS_COHERENT_FAIL" | tee -a "$LOG"
  exit 1
fi
