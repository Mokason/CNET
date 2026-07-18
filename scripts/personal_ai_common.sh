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

# Best-effort unit count: cnb_audit --count (load only, no tag/overlap dump).
# Rebuilds cnb_audit once if missing (ops path after fresh clone).
cnet_ensure_cnb_audit() {
  if [ -x "$REPO/bin/cnb_audit" ]; then
    return 0
  fi
  make -C "$REPO" cnb_audit -j"$(cnet_nproc)" >/dev/null 2>&1 || true
}

cnet_unit_count_fast() {
  local base="${1:-}"
  if [ -z "$base" ] || [ ! -f "$base" ]; then
    echo ""
    return 0
  fi
  cnet_ensure_cnb_audit
  if [ -x "$REPO/bin/cnb_audit" ]; then
    local line
    # Prefer --count (fast). Fall back for older binaries.
    line=$("$REPO/bin/cnb_audit" "$base" --count 2>/dev/null | head -1 || true)
    if [ -z "$line" ]; then
      line=$("$REPO/bin/cnb_audit" "$base" --no-registry 2>/dev/null | head -1 || true)
    fi
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

# 1 if sealed base lists a json_toolcall unit (v1 preferred, v0 legacy).
# Cheap heuristic: unit name appears as ASCII in the CNB blob.
cnet_jtc_present() {
  local base="${1:-}"
  if [ -z "$base" ] || [ ! -f "$base" ]; then
    echo 0
    return 0
  fi
  # Binary CNB often contains unit name as ASCII
  if command -v strings >/dev/null 2>&1; then
    if strings "$base" 2>/dev/null | grep -Eqx 'json_toolcall_v[01]'; then
      echo 1
      return 0
    fi
  fi
  if grep -aobE $'json_toolcall_v[01]\0' "$base" >/dev/null 2>&1 || \
     grep -aobE 'json_toolcall_v[01]' "$base" >/dev/null 2>&1; then
    echo 1
    return 0
  fi
  echo 0
}

# Print current JTC unit name found in base (json_toolcall_v1 | v0 | none).
cnet_jtc_unit_name() {
  local base="${1:-}"
  if [ -z "$base" ] || [ ! -f "$base" ]; then
    echo none
    return 0
  fi
  if command -v strings >/dev/null 2>&1; then
    if strings "$base" 2>/dev/null | grep -qx 'json_toolcall_v1'; then
      echo json_toolcall_v1
      return 0
    fi
    if strings "$base" 2>/dev/null | grep -qx 'json_toolcall_v0'; then
      echo json_toolcall_v0
      return 0
    fi
  fi
  if grep -aob 'json_toolcall_v1' "$base" >/dev/null 2>&1; then
    echo json_toolcall_v1
    return 0
  fi
  if grep -aob 'json_toolcall_v0' "$base" >/dev/null 2>&1; then
    echo json_toolcall_v0
    return 0
  fi
  echo none
}

# Path mark without if/else ladders: cnet_path_mark /path ok MISSING
# Usage: echo "base: $BASE$(cnet_path_mark "$BASE" ' [ok]' ' [MISSING]')"
cnet_path_mark() {
  local p="${1:-}" ok="${2:- [ok]}" bad="${3:- [MISSING]}"
  if [ -n "$p" ] && [ -e "$p" ]; then
    printf '%s' "$ok"
  else
    printf '%s' "$bad"
  fi
}

cnet_x_mark() {
  local p="${1:-}" ok="${2:- [ok]}" bad="${3:- [MISSING]}"
  if [ -n "$p" ] && [ -x "$p" ]; then
    printf '%s' "$ok"
  else
    printf '%s' "$bad"
  fi
}
