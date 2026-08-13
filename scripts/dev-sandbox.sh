#!/usr/bin/env bash
# CNET governor development sandbox (Hermes-style isolation).
#
# Isolated CNET_ROOT worktree overlay + governor state so experiments
# never clobber production logs/governor or the live CNB.
#
# Usage:
#   scripts/dev-sandbox.sh                         # print env and shell
#   scripts/dev-sandbox.sh --persistent            # keep under .cnet-sandbox/
#   scripts/dev-sandbox.sh --from-prod             # seed charter/config from prod
#   scripts/dev-sandbox.sh ./bin/governor_autonomous --test
#   scripts/dev-sandbox.sh make governor_quality
#   scripts/dev-sandbox.sh --delete                # wipe persistent sandbox
#
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PERSISTENT=false
DELETE=false
FROM_PROD=false
NAME="${CNET_DEV_SANDBOX_NAME:-CnetSandbox}"
DIRNAME="${CNET_DEV_SANDBOX_DIR:-.cnet-sandbox}"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --persistent) PERSISTENT=true; shift ;;
    --from-prod|--from) FROM_PROD=true; shift ;;
    --delete) DELETE=true; shift ;;
    -h|--help)
      sed -n '2,20p' "$0"; exit 0 ;;
    --) shift; break ;;
    -*)
      echo "unknown option: $1" >&2; exit 2 ;;
    *) break ;;
  esac
done

if $PERSISTENT; then
  SB="$ROOT/$DIRNAME"
else
  SB="$(mktemp -d -t cnet-sandbox-XXXXXX)"
  cleanup() { rm -rf "$SB"; }
  trap cleanup EXIT
fi

if $DELETE; then
  rm -rf "$ROOT/$DIRNAME"
  echo "deleted $ROOT/$DIRNAME"
  exit 0
fi

mkdir -p "$SB"/{logs/governor,config,scripts,bin,tmp,web_notes}
export CNET_SANDBOX=1
export CNET_SANDBOX_ROOT="$SB"
export CNET_ROOT="$ROOT"   # code from real tree
export CNET_GOVERNOR_DIR="$SB/logs/governor"
export CNET_BASE_PATH="${CNET_BASE_PATH:-$ROOT/soul_gemma4v2_final.cnb}"
export CNET_GOVERNOR_CHARTER="$SB/config/cnet_governor_charter.yaml"
export CNET_FAULT_LOG="$SB/logs/cnet_faults.jsonl"
export CNET_LORA_STORE_DIR="$SB/logs/lora_store"
export CNET_RESIDUAL_HTTP="${CNET_RESIDUAL_HTTP:-http://127.0.0.1:8080}"
export CNET_RESIDUAL_WINDOW="${CNET_RESIDUAL_WINDOW:-$ROOT/english_window_256_bonsai.txt}"
export PATH="$ROOT/bin:$PATH"
export TMPDIR="$SB/tmp"
export CNET_DEV_SANDBOX_NAME="$NAME"

# seed config
if $FROM_PROD || [ ! -f "$SB/config/cnet_governor_charter.yaml" ]; then
  cp -a "$ROOT/config/cnet_governor_charter.yaml" "$SB/config/" 2>/dev/null || true
  cp -a "$ROOT/config/governor_pins.yaml" "$SB/config/" 2>/dev/null || true
  cp -a "$ROOT/config/governor_projects.json" "$SB/config/" 2>/dev/null || true
  cp -a "$ROOT/config/governor_verified_urls.txt" "$SB/config/" 2>/dev/null || true
  cp -a "$ROOT/config/governor_goal_graph.json" "$SB/config/" 2>/dev/null || true
fi
# always prefer sandbox-local config paths
export CNET_GOVERNOR_CHARTER="$SB/config/cnet_governor_charter.yaml"
export CNET_GOVERNOR_PINS="$SB/config/governor_pins.yaml"
export CNET_GOVERNOR_PROJECTS="$SB/config/governor_projects.json"
export CNET_GOVERNOR_URLS="$SB/config/governor_verified_urls.txt"
export CNET_GOVERNOR_GOAL_GRAPH="$SB/config/governor_goal_graph.json"

# empty fault log if missing
: > "$CNET_FAULT_LOG"
mkdir -p "$CNET_LORA_STORE_DIR"

echo "CNET sandbox ready: $SB"
echo "  CNET_GOVERNOR_DIR=$CNET_GOVERNOR_DIR"
echo "  bin=$ROOT/bin (C/C# tools; no python venv)"
echo "  persistent=$PERSISTENT name=$NAME"

if [ "$#" -eq 0 ]; then
  cd "$ROOT"
  exec bash --noprofile --norc
fi

cd "$ROOT"
exec "$@"
