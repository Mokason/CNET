#!/usr/bin/env bash
# Full campaign: unit gates + benches + real-model quality when GGUF present.
set -uo pipefail
cd "$(dirname "$0")/.."
# Campaign validation is intentionally CPU-only: user-facing ROCm devices are
# reserved for serving and validation must not heat or evict them.
export CNET_GPU=0
export CNET_GPU_PAIR=0
export CNET_INFER_BACKEND=cpu
mkdir -p logs/campaign
REPORT="logs/campaign/report_$(date +%Y%m%d_%H%M%S).txt"
exec > >(tee "$REPORT") 2>&1

echo "============================================================"
echo " CNET campaign bench  $(date -Iseconds)"
echo "============================================================"
echo "host: $(uname -srm)"
echo "cpu:  $(nproc)  mem_avail_Gi: $(awk '/MemAvailable/{printf "%.1f",$2/1024/1024}' /proc/meminfo)"
echo

FAILS=0
pass() { echo "RESULT $1 PASS"; }
fail() { echo "RESULT $1 FAIL"; FAILS=$((FAILS+1)); }

run() {
  local name="$1"; shift
  echo "---- $name ----"
  local t0 t1 rc=0
  t0=$(date +%s)
  if "$@"; then
    t1=$(date +%s)
    pass "$name (wall=$((t1-t0))s)"
  else
    rc=$?
    t1=$(date +%s)
    fail "$name (wall=$((t1-t0))s)"
  fi
  echo
  return "$rc"
}

# Gates (already often built; re-run for fresh numbers)
run mtk make -s mtk
run mtk_eval make -s mtk_eval
run kv_page make -s kv_page
run mtp_bench make -s mtp_bench
run sparse_stack make -s sparse_stack
run gguf_stack make -s gguf_stack
run gguf_tok make -s gguf_tok
run resource_governor make -s resource_governor

echo "---- synthetic GGUF tok/s ----"
if run gguf_synth_build make -s cnet_gguf_bench; then
  run gguf_synth bash -o pipefail -c \
    './bin/cnet_gguf_bench 64 2>&1 | tee logs/campaign/gguf_synth.log'
fi

echo "---- MTP sweep (sim launch) ----"
if [[ -x bin/cnet_mtp_bench ]]; then
  run mtp_sweep bash -o pipefail -c \
    'CNET_MTP_SIM_LAUNCH=1000000 ./bin/cnet_mtp_bench 128 2>&1 | tee logs/campaign/mtp.log'
fi

echo "---- build quality_eval ----"
QUALITY_BUILD_OK=0
if run quality_eval_build make -s quality_eval; then QUALITY_BUILD_OK=1; fi

REAL=""
for p in \
  Models/gemma-4-12B-it-MTP-Q8_0.gguf \
  /home/marble/AI/Models/gemma4-v2-Q4_K_M.gguf \
  /home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf \
  hermes_wrappers/Qwythos-9B-Claude-Mythos-5-1M-MTP-Q4_K_S-recovery.gguf
do
  [[ -f "$p" ]] && REAL="$p" && break
done

if [[ "$QUALITY_BUILD_OK" == 1 && -n "$REAL" && -x bin/cnet_quality_eval ]]; then
  echo "---- real quality model=$REAL (English chat template) ----"
  for prof in eco balanced; do
    echo "---- quality profile=$prof ----"
    if CNET_GOV_PROFILE=$prof CNET_GOV_FORCE=1 CNET_MAX_CTX=256 \
         CNET_FOREST_NO_PERSIST=1 CNET_INFER_FP=1 \
         CNET_QUALITY_PROMPT='What is 2+2? Answer with just the number.' \
         CNET_QUALITY_EXPECTED='4' \
         CNET_QUALITY_NO_THINK=1 \
         timeout 600 bin/cnet_quality_eval "$REAL" 24 \
         2>&1 | tee "logs/campaign/quality_${prof}.log"; then
      pass "quality_$prof"
      # Prefer English detokenize evidence when present
      grep -E 'text_gen:|QUALITY_EVAL_' "logs/campaign/quality_${prof}.log" || true
    else
      echo "(open/gen failed for $prof — recording soft)"
      fail "quality_$prof"
    fi
  done
else
  echo "no real GGUF found for quality eval"
  fail quality_model_missing
fi

echo
echo "============================================================"
echo " CAMPAIGN SUMMARY  fails=$FAILS"
echo " report: $REPORT"
echo "============================================================"
echo "--- extracted metrics ---"
grep -E 'PASS|FAIL|tok_s|tok/s|speedup|needles|QUALITY|open_ms|gen_ms|GGUF_BENCH|MTP_BENCH|bench:' \
  logs/campaign/*.log logs/mtk_eval.log logs/mtp_bench.log logs/gguf_stack.log 2>/dev/null | tail -60 || true

exit "$FAILS"
