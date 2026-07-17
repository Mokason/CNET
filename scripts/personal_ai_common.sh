#!/usr/bin/env bash
# Shared helpers for personal_ai_*.sh (source only — not executable entrypoint).
# shellcheck shell=bash
# Usage:  # shellcheck source=personal_ai_common.sh
#         . "$(dirname "$0")/personal_ai_common.sh"

# Repo root when this file lives in scripts/
if [ -z "${CNET_REPO:-}" ]; then
  CNET_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fi
REPO="${REPO:-$CNET_REPO}"

# Default sealed personal base (env wins).
cnet_default_base() {
  echo "${BASE_PATH:-${CNET_BASE_PATH:-$REPO/soul_gemma4v2_final.cnb}}"
}

# GNU grep -c exits 1 on zero matches but still prints 0; with `|| echo 0`
# that becomes "0\n0" and breaks JSON. Always use this.
cnet_count_lines() {
  local f="${1:-}"
  local pat="${2:-.}"
  if [ ! -f "$f" ]; then
    echo 0
    return 0
  fi
  local n
  n=$(grep -cE "$pat" "$f" 2>/dev/null || true)
  echo "${n:-0}"
}

cnet_file_bytes() {
  local f="${1:-}"
  if [ ! -f "$f" ]; then
    echo 0
    return 0
  fi
  wc -c <"$f" | tr -d ' '
}

cnet_learner_active() {
  systemctl --user is-active --quiet cnet-personal-ai-lane.service 2>/dev/null
}

cnet_serve_active() {
  pgrep -x CnetMcpServer >/dev/null 2>&1
}

# Best-effort unit count without full registry certify (fast).
cnet_unit_count_fast() {
  local base="${1:-}"
  if [ -z "$base" ] || [ ! -f "$base" ]; then
    echo ""
    return 0
  fi
  if [ -x "$REPO/bin/cnb_audit" ]; then
    # "base path: units=N blobs=..."
    local line
    line=$("$REPO/bin/cnb_audit" "$base" --no-registry 2>/dev/null | head -1 || true)
    if [[ "$line" =~ units=([0-9]+) ]]; then
      echo "${BASH_REMATCH[1]}"
      return 0
    fi
  fi
  echo ""
}

cnet_nproc() {
  nproc 2>/dev/null || echo 2
}

# Load config/personal-ai.env into the current shell (optional).
cnet_load_personal_env() {
  if [ -f "$REPO/config/personal-ai.env" ]; then
    set -a
    # shellcheck source=/dev/null
    . "$REPO/config/personal-ai.env" || true
    set +a
  fi
}
