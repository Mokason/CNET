#!/usr/bin/env bash
# Full campaign: unit gates + benches + real-model quality when GGUF present.
set -uo pipefail
cd "$(dirname "$0")/.."
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
  local t0 t1
  t0=$(date +%s)
  if "$@"; then
    t1=$(date +%s)
    pass "$name (wall=$((t1-t0))s)"
  else
    t1=$(date +%s)
    fail "$name (wall=$((t1-t0))s)"
  fi
  echo
}

# Gates (already often built; re-run for fresh numbers)
run mtk make -s mtk
run mtk_eval make -s mtk_eval
run kv_page make -s kv_page
run mtp_bench make -s mtp_bench
run sparse_stack make -s sparse_stack
run gguf_stack make -s gguf_stack
run resource_governor make -s resource_governor

echo "---- synthetic GGUF tok/s ----"
make -s cnet_gguf_bench 2>/dev/null || make cnet_gguf_bench
./bin/cnet_gguf_bench 64 2>&1 | tee logs/campaign/gguf_synth.log || fail gguf_synth

echo "---- MTP sweep (sim launch) ----"
if [[ -x bin/cnet_mtp_bench ]]; then
  CNET_MTP_SIM_LAUNCH=1000000 ./bin/cnet_mtp_bench 128 2>&1 | tee logs/campaign/mtp.log || true
fi

echo "---- build quality_eval ----"
make -s quality_eval

REAL=""
for p in \
  Models/gemma-4-12B-it-MTP-Q8_0.gguf \
  /home/marble/AI/Models/gemma4-v2-Q4_K_M.gguf \
  /home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf \
  hermes_wrappers/Qwythos-9B-Claude-Mythos-5-1M-MTP-Q4_K_S-recovery.gguf
do
  [[ -f "$p" ]] && REAL="$p" && break
done

if [[ -n "$REAL" && -x bin/cnet_quality_eval ]]; then
  echo "---- real quality model=$REAL ----"
  for prof in eco balanced; do
    echo "---- quality profile=$prof ----"
    if CNET_GOV_PROFILE=$prof CNET_GOV_FORCE=1 CNET_MAX_CTX=128 \
         CNET_FOREST_NO_PERSIST=1 CNET_INFER_FP=1 \
         timeout 400 bin/cnet_quality_eval "$REAL" 12 1 198 271 11 \
         2>&1 | tee "logs/campaign/quality_${prof}.log"; then
      pass "quality_$prof"
    else
      echo "(open/gen failed for $prof — recording soft)"
      fail "quality_$prof"
    fi
  done
else
  echo "no real GGUF found for quality eval"
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
