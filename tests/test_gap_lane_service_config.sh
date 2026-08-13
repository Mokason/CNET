#!/usr/bin/env bash
# Fail-safe deployment tracer for the CNET gap-lane systemd unit.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
fail() {
    printf 'GAP_LANE_SERVICE_CONFIG_FAIL: %s\n' "$1" >&2
    exit 1
}

SERVICE=config/cnet-gap-lane.service
[[ -f "$SERVICE" ]] || fail "missing $SERVICE"
text=$(cat "$SERVICE")

unit_body=$(awk '
  /^\[Unit\]/ {grab=1; next}
  grab && /^\[/ {exit}
  grab {print}
' "$SERVICE")
[[ -n "$unit_body" ]] || fail "[Unit] section not found"

exec_bin=$(printf '%s\n' "$text" | grep -E '^ExecStart=\S+' | head -n1 | sed 's/^ExecStart=//' | awk '{print $1}')
[[ -n "$exec_bin" ]] || fail "ExecStart is missing or empty"

mapfile -t conds < <(printf '%s\n' "$text" | grep -E '^!?ConditionFileIsExecutable=\S+' || true)
[[ ${#conds[@]} -eq 1 ]] ||
    fail "unit must declare exactly one ConditionFileIsExecutable"
[[ "${conds[0]}" != '!'* ]] || fail "condition must not be negated"

cond_in_unit=$(printf '%s\n' "$unit_body" | grep -E '^ConditionFileIsExecutable=\S+' | head -n1 || true)
[[ -n "$cond_in_unit" ]] ||
    fail "[Unit] lacks ConditionFileIsExecutable — absent binary will cause 203/EXEC restart churn instead of a clean condition skip"
cond_path=${cond_in_unit#ConditionFileIsExecutable=}
[[ "$cond_path" == "$exec_bin" ]] ||
    fail "ConditionFileIsExecutable ($cond_path) does not match ExecStart binary ($exec_bin)"

if command -v systemd-analyze >/dev/null 2>&1; then
    systemd-analyze condition 'ConditionFileIsExecutable=/bin/sh' >/dev/null ||
        fail "systemd-analyze condition accepted /bin/sh unexpectedly failed"
    missing_path="$ROOT/bin/.cnet-condition-probe-missing"
    [[ ! -e "$missing_path" ]] || fail "semantic probe path unexpectedly exists"
    if systemd-analyze condition "ConditionFileIsExecutable=$missing_path" >/dev/null 2>&1; then
        fail "ConditionFileIsExecutable must fail for a missing path"
    fi
fi

printf 'GAP_LANE_SERVICE_CONFIG_PASS\n'
