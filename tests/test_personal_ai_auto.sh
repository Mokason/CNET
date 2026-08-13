#!/usr/bin/env bash
# Static gate for Personal AI automation units + script.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
fail() { printf 'PERSONAL_AI_AUTO_FAIL: %s\n' "$1" >&2; exit 1; }

SCRIPT=scripts/personal_ai_auto.sh
LANE=config/cnet-personal-ai-lane.service
TARGET=config/cnet-personal-ai.target
ENV=config/personal-ai.env
OPS_ENV=config/personal-ai-ops.env
OPS_TICK=scripts/personal_ai_ops_tick.sh

lane=$(cat "$LANE")
printf '%s' "$lane" | grep -Fq 'ConditionFileIsExecutable=' || fail "lane missing ConditionFileIsExecutable"
printf '%s' "$lane" | grep -Fq 'gap_lane_run' || fail "lane missing gap_lane_run"
printf '%s' "$lane" | grep -Fq 'CNET_GAP_INBOX' || fail "lane missing CNET_GAP_INBOX"
printf '%s' "$lane" | grep -Fq 'CNET_TRAIN_FAST' || fail "lane missing CNET_TRAIN_FAST"
printf '%s' "$lane" | grep -Fq 'CNET_TEACHER_IDLE_SEC' || fail "lane missing CNET_TEACHER_IDLE_SEC"
cond=$(printf '%s\n' "$lane" | grep -E 'ConditionFileIsExecutable=\S+' | head -n1 | sed 's/.*=//')
[[ "$cond" == *gap_lane_run ]] || fail "Condition binary must end with gap_lane_run"

env_text=$(cat "$ENV")
printf '%s' "$env_text" | grep -Fq 'CNET_PERSONAL_ALLOW_TEACHER=1' || fail "env missing ALLOW_TEACHER=1"
printf '%s' "$env_text" | grep -Fq 'CNET_PERSONAL_TEACH_INLINE=0' || fail "env missing TEACH_INLINE=0"
printf '%s' "$env_text" | grep -Fq 'CNET_TEACHER_IDLE_SEC' || fail "env missing TEACHER_IDLE_SEC"
printf '%s' "$env_text" | grep -Fq 'CNET_RESIDUAL_GGUF=' || fail "env missing CNET_RESIDUAL_GGUF="
if printf '%s' "$env_text" | grep -Fq '# CNET_RESIDUAL_GGUF='; then
    fail "CNET_RESIDUAL_GGUF must not be commented out"
fi
printf '%s' "$env_text" | grep -Fq 'CNET_RESIDUAL_WINDOW=' || fail "env missing CNET_RESIDUAL_WINDOW="

set +e
help_out=$(bash "$SCRIPT" 2>&1)
help_rc=$?
set -e
[[ $help_rc -eq 2 ]] || fail "personal_ai_auto.sh without args must exit 2"
for tok in 'prepare|install|start' serve-proof loop jtc-seal ops-install ops-tick; do
    printf '%s' "$help_out" | grep -Fq "$tok" || fail "help missing $tok"
done

[[ -f "$TARGET" ]] || fail "missing $TARGET"
[[ -f "$SCRIPT" ]] || fail "missing $SCRIPT"
grep -Fq 'PERSONAL_AI_AUTO_PREPARE_OK' "$SCRIPT" || fail "script missing PREPARE_OK marker"
common=scripts/personal_ai_common.sh
[[ -f "$common" ]] || fail "missing $common"
grep -Fq 'cnet_count_lines' "$common" || fail "common missing cnet_count_lines"
grep -Fq 'cnet_default_base' "$common" || fail "common missing cnet_default_base"

command -v jq >/dev/null 2>&1 || fail "jq required for metrics JSON check"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
base="$TMP/fake.cnb"
printf 'x' >"$base"
: >"${base}.inbox"
metrics_out=$(bash scripts/personal_ai_metrics.sh "$base")
inbox_lines=$(printf '%s' "$metrics_out" | jq -r '.inbox_lines')
inbox_no_plan=$(printf '%s' "$metrics_out" | jq -r '.inbox_no_plan')
[[ "$inbox_lines" == "0" ]] || fail "inbox_lines expected 0 got $inbox_lines"
[[ "$inbox_no_plan" == "0" ]] || fail "inbox_no_plan expected 0 got $inbox_no_plan"

log="$TMP/pattern.log"
printf '%s\n' 'status: count=7' 'noise' 'status: promoted=3' 'trailing' >"$log"
for pair in promoted:3 count:7 missing:0; do
    key=${pair%%:*}
    expected=${pair##*:}
    got=$(bash "$OPS_TICK" --extract-metric "$log" "$key")
    [[ "$got" == "$expected" ]] || fail "ops metric $key expected $expected got $got"
done

ops=$(cat "$OPS_ENV")
printf '%s' "$ops" | grep -Fq 'OPS_CURRICULUM_ON_STALE=0' || fail "ops env missing CURRICULUM_ON_STALE=0"
printf '%s' "$ops" | grep -Fq 'OPS_ASK_HERMES=0' || fail "ops env missing ASK_HERMES=0"
printf '%s' "$ops" | grep -Fq 'OPS_RESTART_LEARNER=0' || fail "ops env missing RESTART_LEARNER=0"
if printf '%s' "$ops" | grep -Fq 'g4v2ref'; then
    fail "ops env must not target g4v2ref"
fi

printf 'PERSONAL_AI_AUTO_PASS\n'
