#!/usr/bin/env bash
# Install pack_english_basic into runtime packs + live deploy path.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/packs/pack_english_basic"
DEST_ART="$ROOT/artifacts/roe_daily_packs/pack_english_basic"
DEST_LIVE="${CNET_PACKS_ROOT:-$HOME/.local/share/cnet-minimal/current/data/roe_daily_packs}/pack_english_basic"

if [[ ! -d "$SRC" ]]; then
  echo "missing $SRC" >&2
  exit 1
fi
mkdir -p "$(dirname "$DEST_ART")" "$(dirname "$DEST_LIVE")"
rm -rf "$DEST_ART" "$DEST_LIVE"
cp -a "$SRC" "$DEST_ART"
cp -a "$SRC" "$DEST_LIVE"

# Ensure routes include english (from repo ROUTES if present)
if [[ -f "$ROOT/artifacts/roe_daily_packs/ROUTES.jsonl" ]]; then
  cp -a "$ROOT/artifacts/roe_daily_packs/ROUTES.jsonl" \
    "${CNET_PACKS_ROOT:-$HOME/.local/share/cnet-minimal/current/data/roe_daily_packs}/ROUTES.jsonl" 2>/dev/null || true
fi
echo "pack_english_basic installed → $DEST_LIVE"
echo "restart cnetd to reload: systemctl --user restart cnetd.service"
