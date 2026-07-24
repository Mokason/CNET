#!/usr/bin/env bash
# Doctor: open-lab import deploy knobs (P0/P2).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENVF="${CNET_PERSONAL_AI_ENV:-$ROOT/config/personal-ai.env}"
ok=0
warn=0

echo "cnet_openlab_doctor | root=$ROOT"

check_kv() {
  local k="$1"
  local v
  v="$(grep -E "^${k}=" "$ENVF" 2>/dev/null | tail -1 | cut -d= -f2- || true)"
  if [[ -z "${v}" ]]; then
    echo "WARN  $k unset in $ENVF"
    warn=$((warn+1))
  else
    echo "OK    $k=$v"
    ok=$((ok+1))
  fi
}

if [[ ! -f "$ENVF" ]]; then
  echo "FAIL  missing $ENVF"
  exit 1
fi

check_kv CNET_FAULT_LOG
check_kv CNET_LORA_STORE_DIR
check_kv CNET_AUTO_LEARN_FREEFORM
check_kv CNET_LORA_STORE_AUTOSAVE
check_kv CNET_FAULT_MIRROR

for k in CNET_SPARSE_KV CNET_FOREST_LFRU CNET_ACCT_LOG CNET_ADAPTER_BANK; do
  v="$(grep -E "^${k}=" "$ENVF" 2>/dev/null | tail -1 || true)"
  if [[ -n "${v}" ]]; then
    echo "OK    $v"
    ok=$((ok+1))
  else
    echo "INFO  $k unset (optional)"
  fi
done

flog="$(grep -E '^CNET_FAULT_LOG=' "$ENVF" | tail -1 | cut -d= -f2- || true)"
if [[ -n "${flog}" ]]; then
  mkdir -p "$(dirname "$flog")" 2>/dev/null || true
  if touch "$flog" 2>/dev/null; then
    echo "OK    fault log writable: $flog"
    ok=$((ok+1))
  else
    echo "WARN  cannot write $flog"
    warn=$((warn+1))
  fi
fi

echo "CNET_OPENLAB_DOCTOR_PASS ok=$ok warn=$warn"
exit 0
