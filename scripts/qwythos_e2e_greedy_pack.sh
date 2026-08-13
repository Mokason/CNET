#!/usr/bin/env bash
# Greedy e2e-safe ternary pack for Qwythos.
#
# One NEW specialist per convert (MAX=1 counts only new work).
# Quality gate must PASS on THIS run's log (not a shared history file).
# Markers from ladder: NEW_PACK / NO_PACK (authoritative).
#
# Does NOT auto-start long campaigns from make — invoke explicitly.
#
# Env: MODEL PACK SKIP LOG CERT STEPS N_CALIB N_HOLDOUT CAL_SEQS
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p logs artifacts

MODEL="${MODEL:-hermes_wrappers/Qwythos-9B-Claude-Mythos-5-1M-MTP-Q4_K_S-recovery.gguf}"
PACK="${PACK:-artifacts/qwythos_e2e.ldtr}"
SKIP="${SKIP:-artifacts/qwythos_e2e.skip}"
LOG="${LOG:-logs/qwythos_e2e_greedy.log}"
QLOG="${QLOG:-logs/qwythos_e2e_quality.log}"
CERT="${CERT:-0.53}"
STEPS="${STEPS:-64}"
N_CALIB="${N_CALIB:-64}"
N_HOLDOUT="${N_HOLDOUT:-24}"
CAL_SEQS="${CAL_SEQS:-3}"
FAMILIES=(gate_proj up_proj down_proj o_proj k_proj v_proj)
# 0 = full greedy (maximal coherent subset; slower).
# N>0 = stop family after N quality rejects (faster, non-maximal).
FAM_REJECT_LIMIT="${FAM_REJECT_LIMIT:-0}"

[[ -x bin/cnet_spec_ladder ]] || make -s spec_ladder_tool
[[ -x bin/cnet_quality_eval ]] || make -s quality_eval
[[ -f "$MODEL" ]] || { echo "MODEL missing: $MODEL"; exit 1; }

# LDTR v1: magic u32, ver u32, count u32 ('LDTR' LE = 0x5254444C)
pack_count() {
  if [[ ! -f "$PACK" ]]; then echo 0; return; fi
  # od: little-endian unsigned ints; fail closed on short/bad packs
  local hdr mag ver n
  hdr=$(od -An -t u4 -N 12 "$PACK" 2>/dev/null | tr -s ' ' | sed 's/^ //')
  # shellcheck disable=SC2086
  set -- $hdr
  mag=${1:-0}; ver=${2:-0}; n=${3:-0}
  if [[ "$mag" -eq 1381258316 && "$ver" -eq 1 ]]; then
    echo "$n"
  else
    echo 0
  fi
}

# Quality: isolated log for THIS invocation only.
# force_fp=1 → never import pack (true FP baseline).
quality_ok() {
  local tag=$1
  local force_fp=${2:-0}
  local qtmp rc=0
  qtmp=$(mktemp "${TMPDIR:-/tmp}/qe.XXXXXX")
  {
    echo "---- quality ($tag) $(date -Iseconds) force_fp=$force_fp ----"
  } | tee -a "$LOG" | tee -a "$QLOG"

  local imp=""
  if [[ "$force_fp" != "1" ]]; then
    local pc
    pc=$(pack_count)
    if [[ "$pc" -gt 0 ]]; then
      imp="$PACK"
    fi
  fi

  set +e
  CNET_GPU=1 CNET_GPU_BACKEND=opencl CNET_GOV_PROFILE=turbo \
  CNET_MAX_CTX=256 CNET_FOREST_NO_PERSIST=1 \
  CNET_QUALITY_NO_THINK=1 \
  CNET_QUALITY_PROMPT='What is 2+2? Answer with just the number.' \
  CNET_QUALITY_EXPECTED=4 \
  CNET_LADDER_IMPORT="$imp" \
  ./bin/cnet_quality_eval "$MODEL" 24 >"$qtmp" 2>&1
  rc=$?
  set -e

  cat "$qtmp" | tee -a "$QLOG" | tee -a "$LOG" >/dev/null
  # Also show on stdout for nohup.out
  cat "$qtmp"

  local ok=0
  # Authoritative: final line must be PASS and exit 0
  if [[ $rc -eq 0 ]] && grep -qE '^QUALITY_EVAL_PASS ' "$qtmp"; then
    if ! grep -qE '^QUALITY_EVAL_FAIL ' "$qtmp"; then
      # Must have passed the expected-token CHECK (ok line, not FAIL)
      if grep -qE '^\s*ok\s+generated answer contains the expected' "$qtmp"; then
        ok=1
      fi
    fi
  fi

  rm -f "$qtmp"
  if [[ $ok -eq 1 ]]; then
    echo "quality_ok PASS ($tag)" | tee -a "$LOG"
    return 0
  fi
  echo "quality_ok FAIL ($tag) rc=$rc" | tee -a "$LOG"
  return 1
}

# Parse markers from a convert capture file (not whole historical LOG).
parse_convert_markers() {
  local cap=$1
  # NEW_PACK name / NO_PACK name / none
  NEW_NAME=""
  NEW_KIND="" # pack|nopack|none
  if grep -q '\[ladder\] NEW_PACK ' "$cap"; then
    NEW_KIND=pack
    NEW_NAME=$(grep '\[ladder\] NEW_PACK ' "$cap" | tail -1 \
      | sed -n 's/.*NEW_PACK \([^ ]*\).*/\1/p')
  elif grep -q '\[ladder\] NO_PACK ' "$cap"; then
    NEW_KIND=nopack
    NEW_NAME=$(grep '\[ladder\] NO_PACK ' "$cap" | tail -1 \
      | sed -n 's/.*NO_PACK \([^ ]*\).*/\1/p')
  else
    NEW_KIND=none
    NEW_NAME=""
  fi
  EXPORT_N=$(grep -E 'export .* written=' "$cap" | tail -1 \
    | sed -n 's/.*written=\([0-9]*\).*/\1/p')
  EXPORT_N=${EXPORT_N:-0}
}

denylist_add() {
  local n=$1
  [[ -z "$n" ]] && return 0
  if ! grep -qxF "$n" "$SKIP" 2>/dev/null; then
    echo "$n" >> "$SKIP"
    echo "denylist + $n" | tee -a "$LOG"
  fi
}

{
  echo "============================================================"
  echo " Qwythos E2E greedy pack  $(date -Iseconds)"
  echo " model=$MODEL"
  echo " pack=$PACK skip=$SKIP"
  echo " cert=$CERT steps=$STEPS fam_reject_limit=$FAM_REJECT_LIMIT"
  echo " note: cert~0.53 ≈ posthoc floor on Q4 ternary (~0.51); e2e gate is the real bar"
  echo " note: FAM_REJECT_LIMIT=0 means try every specialist (maximal greedy)"
  echo "============================================================"
} | tee -a "$LOG"

touch "$SKIP"
kept=$(pack_count)
rejected=0
echo "seed_pack_count=$kept" | tee -a "$LOG"

# 1) Pure FP baseline (must work before we trust the gate)
if ! quality_ok baseline_fp 1; then
  echo "BASELINE_FAIL — abort" | tee -a "$LOG"
  exit 1
fi
echo "BASELINE_PASS" | tee -a "$LOG"

# 2) Seed pack (if any) must itself be coherent
if [[ $(pack_count) -gt 0 ]]; then
  if ! quality_ok seed_pack 0; then
    echo "SEED_PACK_FAIL — removing pack" | tee -a "$LOG"
    rm -f "$PACK"
    kept=0
  else
    kept=$(pack_count)
    echo "SEED_PACK_PASS count=$kept" | tee -a "$LOG"
  fi
fi

for fam in "${FAMILIES[@]}"; do
  fam_rejects=0
  idle=0
  while [[ $idle -lt 2 ]]; do
    before=$(pack_count)
    bak=""
    if [[ "$before" -gt 0 ]]; then
      bak="${PACK}.bak.$$"
      cp -f "$PACK" "$bak"
    fi

    echo "==== try family=$fam kept=$kept count=$before $(date -Iseconds) ====" \
      | tee -a "$LOG"

    cap=$(mktemp "${TMPDIR:-/tmp}/conv.XXXXXX")
    set +e
    CNET_INFER_FP=1 CNET_FOREST_NO_PERSIST=1 CNET_MAX_CTX=128 \
    CNET_GPU=1 CNET_GPU_BACKEND=opencl CNET_GPU_MIN_FLOPS=1 \
    CNET_LADDER_THREADS=24 CNET_LADDER_PARALLEL=1 \
    CNET_LADDER_MAX=1 CNET_LADDER_STEPS="$STEPS" \
    CNET_LADDER_N_CALIB="$N_CALIB" CNET_LADDER_N_HOLDOUT="$N_HOLDOUT" \
    CNET_LADDER_CAL_SEQS="$CAL_SEQS" CNET_LADDER_CERT="$CERT" \
    CNET_LADDER_EXPORT="$PACK" \
    CNET_LADDER_IMPORT="$( [[ $(pack_count) -gt 0 ]] && echo "$PACK" || true )" \
    CNET_LADDER_SKIP="$SKIP" \
    ./bin/cnet_spec_ladder "$MODEL" "$fam" ste >"$cap" 2>&1
    conv_rc=$?
    set -e
    cat "$cap" | tee -a "$LOG"

    parse_convert_markers "$cap"
    after=$(pack_count)
    rm -f "$cap"

    echo "markers kind=$NEW_KIND name=$NEW_NAME export_n=$EXPORT_N count $before->$after conv_rc=$conv_rc" \
      | tee -a "$LOG"

    case "$NEW_KIND" in
      none)
        # No convert attempted (family done or all denylisted/skipped)
        echo "family $fam: nothing to convert" | tee -a "$LOG"
        rm -f "$bak"
        break
        ;;
      nopack)
        denylist_add "$NEW_NAME"
        idle=0
        rm -f "$bak"
        # count unchanged expected
        if [[ "$after" -ne "$before" ]]; then
          echo "WARN: NO_PACK but count changed $before->$after — restoring bak" \
            | tee -a "$LOG"
          if [[ -n "$bak" && -f "$bak" ]]; then mv -f "$bak" "$PACK"; else true; fi
          after=$(pack_count)
        fi
        continue
        ;;
      pack)
        # Must see count increase by exactly 1 (or export_n == before+1)
        if [[ "$after" -ne $((before + 1)) ]]; then
          echo "WARN: NEW_PACK but count $before->$after (expected +1)" | tee -a "$LOG"
          # If count didn't grow, import/export bug — denylist and restore
          if [[ "$after" -le "$before" ]]; then
            denylist_add "$NEW_NAME"
            if [[ -n "$bak" && -f "$bak" ]]; then mv -f "$bak" "$PACK"; fi
            idle=$((idle + 1))
            continue
          fi
        fi
        idle=0
        if quality_ok "after_${fam}_$NEW_NAME" 0; then
          kept=$after
          echo "KEEP $NEW_NAME count=$after" | tee -a "$LOG"
          rm -f "$bak"
          fam_rejects=0
        else
          echo "REJECT $NEW_NAME — rollback" | tee -a "$LOG"
          if [[ -n "$bak" && -f "$bak" ]]; then
            mv -f "$bak" "$PACK"
          else
            rm -f "$PACK"
          fi
          denylist_add "$NEW_NAME"
          rejected=$((rejected + 1))
          fam_rejects=$((fam_rejects + 1))
          kept=$(pack_count)
          if [[ $fam_rejects -ge $FAM_REJECT_LIMIT ]]; then
            echo "family $fam: reject limit $FAM_REJECT_LIMIT — stop family" \
              | tee -a "$LOG"
            break
          fi
        fi
        ;;
    esac
  done
done

{
  echo ""
  echo "============================================================"
  echo " E2E_GREEDY_DONE $(date -Iseconds) kept=$(pack_count) rejected=$rejected"
  if [[ $(pack_count) -gt 0 ]]; then
    ls -lh "$PACK"
    echo "QWYTHOS_COHERENT_AND_CONVERTED=1 pack=$PACK kept=$(pack_count)"
  else
    echo "QWYTHOS_COHERENT_FP_ONLY=1 kept=0"
  fi
  echo " skip_list=$(wc -l < "$SKIP") "
  echo "============================================================"
} | tee -a "$LOG"

# Final gate with current pack (or FP if empty)
if [[ $(pack_count) -gt 0 ]]; then
  quality_ok final 0 || echo "FINAL_QUALITY_FAIL" | tee -a "$LOG"
else
  quality_ok final 1 || echo "FINAL_FP_FAIL" | tee -a "$LOG"
fi

exit 0
