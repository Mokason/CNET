#!/usr/bin/env bash
# Enforce KPI floors and law invariants on a scenario-layer log.
#
# WHY THIS EXISTS
# The scenario layers printed cert_rate / honesty / goal_rate / synth_prec /
# committee_rate and the law flag residual_auto_cert, but the gates grepped
# only the binary markers. Every rate was therefore a vanity metric in the
# exact sense AGENTS.md #3 forbids: a number reported to a human that nothing
# checks. synth_prec could fall 0.600 -> 0.100 and the gate stayed green.
# Worse, residual_auto_cert=0 -- a CORE law, not a quality score -- was printed
# and unenforced in all three layers.
#
# Floors are set at MEASURED values (the layers are deterministic: three
# consecutive runs produce byte-identical KPI lines, only ms varies). Setting a
# floor where none existed is not lowering one. Nothing here may be relaxed to
# make a gate pass -- AGENTS.md #1. If a floor legitimately moves, move it in
# the same commit that earns it and say so.
#
# Usage:
#   scenario_kpi_floor_check.sh <log> <name> key>=floor [key==exact ...]
#
#   key>=floor   numeric floor; FAIL if measured < floor
#   key==exact   exact match; FAIL if measured != exact (use for law flags)
#
# A key that is absent from the log is a FAIL, never a silent skip -- a renamed
# or dropped metric must break the gate rather than vanish from it.
set -euo pipefail

LOG="${1:?usage: scenario_kpi_floor_check.sh <log> <name> <checks...>}"
NAME="${2:?missing scenario name}"
shift 2

[[ -f $LOG ]] || { echo "SCENARIO_KPI_FLOOR_FAIL $NAME reason=no_log log=$LOG"; exit 1; }

fails=0
checked=0

# Metrics may appear on any line of the log (KPI summary or trailer line).
read_metric() { # key -> value on stdout, empty when absent
  # `|| true` is load-bearing: with `set -o pipefail`, a non-matching grep would
  # abort the calling assignment and the script would exit before printing which
  # key was missing -- failing closed, but silently. Fail loud instead.
  grep -oE "(^|[[:space:]])$1=[-0-9.]+" "$LOG" 2>/dev/null | tail -1 | sed "s/.*$1=//" || true
}

for spec in "$@"; do
  if [[ $spec == *"=="* ]]; then
    key="${spec%%==*}"; want="${spec##*==}"; mode=exact
  elif [[ $spec == *">="* ]]; then
    key="${spec%%>=*}"; want="${spec##*>=}"; mode=floor
  else
    echo "  FAIL malformed check spec: $spec"; fails=$((fails + 1)); continue
  fi

  got="$(read_metric "$key")"
  checked=$((checked + 1))

  if [[ -z $got ]]; then
    printf '  FAIL %-22s absent from log (expected %s%s)\n' "$key" \
      "$([[ $mode == exact ]] && echo '==' || echo '>=')" "$want"
    fails=$((fails + 1))
    continue
  fi

  if [[ $mode == exact ]]; then
    if awk "BEGIN{exit !($got == $want)}"; then
      printf '  ok   %-22s %s == %s\n' "$key" "$got" "$want"
    else
      printf '  FAIL %-22s %s != %s  (LAW)\n' "$key" "$got" "$want"
      fails=$((fails + 1))
    fi
  else
    if awk "BEGIN{exit !($got >= $want)}"; then
      printf '  ok   %-22s %s >= %s\n' "$key" "$got" "$want"
    else
      printf '  FAIL %-22s %s < %s  (REGRESSION)\n' "$key" "$got" "$want"
      fails=$((fails + 1))
    fi
  fi
done

if [[ $fails -eq 0 ]]; then
  echo "SCENARIO_KPI_FLOOR_PASS $NAME checked=$checked fails=0"
else
  echo "SCENARIO_KPI_FLOOR_FAIL $NAME checked=$checked fails=$fails"
  exit 1
fi
