#!/usr/bin/env bash
# Fail if in-scope product Python remains, or Makefile/scripts still invoke it.
# Scope: tools/ scripts/ tests/ mcp_servers/ dotnet/ root *.py — not .claude/ or third_party/.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

fail=0

echo "== tracked in-scope .py =="
mapfile -t PYS < <(git ls-files '*.py' | grep -vE '^\.claude/|^third_party/' || true)
if ((${#PYS[@]})); then
  printf '%s\n' "${PYS[@]}"
  fail=1
else
  echo "(none)"
fi

echo "== python3 / .venv-*python in Makefile + scripts/*.sh + tools/*.sh =="
hits=$(grep -nE 'python3|\.py\b|\.venv-[^/]+/bin/python' Makefile scripts/*.sh tools/*.sh 2>/dev/null \
  | grep -vE 'no_python_audit|WITHHELD|formerly|was python|not python|gate_evidence\.py|voice_teacher\.py|margin_sweep' || true)
if [[ -n "$hits" ]]; then
  printf '%s\n' "$hits"
  fail=1
else
  echo "(none)"
fi

if [[ "$fail" -ne 0 ]]; then
  echo "NO_PYTHON_AUDIT_FAIL"
  exit 1
fi
echo "NO_PYTHON_AUDIT_PASS"
exit 0
