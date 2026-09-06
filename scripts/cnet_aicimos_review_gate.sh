#!/usr/bin/env bash
# Off-hot-path gate for cnet_aicimos_review_export. Never CERT.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${ROOT}/bin/cnet_aicimos_review_export"
LOG="${ROOT}/logs/aicimos_review_export.log"
mkdir -p "${ROOT}/bin" "${ROOT}/logs"
if [[ ! -x "$BIN" ]]; then
  echo "missing $BIN — run make aicimos_review_export" >&2
  exit 1
fi
"$BIN" --test | tee "$LOG"
grep -q 'AICIMOS_REVIEW_EXPORT_PASS' "$LOG"
grep -q 'claimed_cert=0' "$LOG"

# sha256 of a live fixture must match packet
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/inbox/x"
cat > "$TMP/inbox/x/PROPOSE.json" <<'JSON'
{"kind":"ffi_convert","sent":"go","got":"went","auto_cert":false,"admitted":false}
JSON
"$BIN" --in "$TMP" --out "$TMP/packets.jsonl"
hex=$(sha256sum "$TMP/inbox/x/PROPOSE.json" | awk '{print $1}')
grep -q "$hex" "$TMP/packets.jsonl"
grep -q '"claimed_cert":0' "$TMP/packets.jsonl"
if grep -q '"claimed_cert":1' "$TMP/packets.jsonl"; then
  echo "claimed_cert=1 leaked" >&2
  exit 1
fi
if ls "$TMP"/*.lut >/dev/null 2>&1 || [[ -d "$TMP/gold" ]]; then
  echo "forbidden lut/gold created" >&2
  exit 1
fi
echo "AICIMOS_REVIEW_GATE_PASS sha=$hex"
