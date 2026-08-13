#!/usr/bin/env bash
# Offline verification of greedy-pack helpers (no 9B load).
# Exit 0 only if all checks pass. Does NOT start the campaign.
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0
check() {
  if "$@"; then echo "ok  $*"; else echo "FAIL $*"; fail=$((fail+1)); fi
}

# --- pack_count ---
source /dev/null
pack_count() {
  local PACK=$1
  if [[ ! -f "$PACK" ]]; then echo 0; return; fi
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

MICRO=artifacts/qwythos_e2e_micro.ldtr
if [[ -f "$MICRO" ]]; then
  n=$(pack_count "$MICRO")
  check test "$n" -eq 1
else
  echo "skip micro pack missing"
fi

# empty / garbage
n=$(pack_count /nonexistent)
check test "$n" -eq 0

# --- marker parsing ---
cap=$(mktemp)
cat >"$cap" <<'EOF'
[ladder] convert qwen35.blk.1.gate_proj#0 …
[ladder]   qwen35.blk.1.gate_proj#0 cert=1 packed=1 relerr=0.5148
[ladder] NEW_PACK qwen35.blk.1.gate_proj#0 relerr=0.5148
  export artifacts/x.ldtr written=2
LADDER_PASS
EOF
NEW_NAME=$(grep '\[ladder\] NEW_PACK ' "$cap" | tail -1 | sed -n 's/.*NEW_PACK \([^ ]*\).*/\1/p')
check test "$NEW_NAME" = "qwen35.blk.1.gate_proj#0"

cat >"$cap" <<'EOF'
[ladder] convert qwen35.blk.2.gate_proj#0 …
[ladder]   qwen35.blk.2.gate_proj#0 cert=0 packed=0 relerr=0.56
[ladder] NO_PACK qwen35.blk.2.gate_proj#0 relerr=0.56
  export artifacts/x.ldtr written=1
EOF
if grep -q '\[ladder\] NEW_PACK ' "$cap"; then
  echo "FAIL should not see NEW_PACK"; fail=$((fail+1))
else
  echo "ok  no NEW_PACK on cert fail"
fi
NO=$(grep '\[ladder\] NO_PACK ' "$cap" | sed -n 's/.*NO_PACK \([^ ]*\).*/\1/p')
check test "$NO" = "qwen35.blk.2.gate_proj#0"
rm -f "$cap"

# --- quality log logic (simulate) ---
qtmp=$(mktemp)
cat >"$qtmp" <<'EOF'
  ok  generated answer contains the expected bounded token
QUALITY_EVAL_PASS checks=11 fails=0 open_ms=1.0 decode_tok_s=1.0 wall_tok_s=1.0 chat=1 gpu=1
EOF
ok=0
if grep -qE '^QUALITY_EVAL_PASS ' "$qtmp" && ! grep -qE '^QUALITY_EVAL_FAIL ' "$qtmp" \
   && grep -qE '^\s*ok\s+generated answer contains the expected' "$qtmp"; then
  ok=1
fi
check test "$ok" -eq 1

cat >"$qtmp" <<'EOF'
  FAIL generated answer contains the expected bounded token
QUALITY_EVAL_FAIL checks=12 fails=1 open_ms=1.0 decode_tok_s=1.0 wall_tok_s=1.0 chat=1 gpu=1
EOF
# Old bug: grepping shared log with a prior PASS would false-positive.
# Isolated log must FAIL:
ok=1
if ! grep -qE '^QUALITY_EVAL_PASS ' "$qtmp"; then ok=0; fi
if grep -qE '^QUALITY_EVAL_FAIL ' "$qtmp"; then ok=0; fi
check test "$ok" -eq 0
rm -f "$qtmp"

# --- binary has NEW_PACK string ---
check grep -q NEW_PACK bin/cnet_spec_ladder

# --- hermetic unit test if present ---
if [[ -x bin/test_spec_ladder ]]; then
  if ./bin/test_spec_ladder >/tmp/tsl.out 2>&1 && grep -q SPEC_LADDER_PASS /tmp/tsl.out; then
    echo "ok  test_spec_ladder"
  else
    echo "FAIL test_spec_ladder"; fail=$((fail+1)); tail -20 /tmp/tsl.out || true
  fi
else
  echo "note: bin/test_spec_ladder missing — run make spec_ladder"
fi

echo "========================================"
if [[ $fail -eq 0 ]]; then
  echo "VERIFY_E2E_LOGIC_PASS"
  exit 0
fi
echo "VERIFY_E2E_LOGIC_FAIL fails=$fail"
exit 1
