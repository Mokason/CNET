#!/usr/bin/env bash
# Static gate for multimodal campaign helper + plan.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
fail() { printf 'MULTIMODAL_PREPARE_FAIL: %s\n' "$1" >&2; exit 1; }

SCRIPT=tools/multimodal_campaign.sh
PLAN=plans/multimodal_external_teachers.md

plan=$(cat "$PLAN")
printf '%s' "$plan" | grep -Fq 'External Teachers' || fail "plan missing External Teachers"
printf '%s' "$plan" | grep -Fq 'Voice v0' || fail "plan missing Voice v0"
printf '%s' "$plan" | grep -Fq 'Vision v0' || fail "plan missing Vision v0"

out=$(bash "$SCRIPT" prepare) || fail "multimodal prepare failed"
printf '%s' "$out" | grep -Fq 'MULTIMODAL_PREPARE_PASS' ||
    fail "prepare did not print MULTIMODAL_PREPARE_PASS"
[[ -f artifacts/voice_v0_commands.txt ]] || fail "missing artifacts/voice_v0_commands.txt"
[[ -f artifacts/vision_v0_classes.txt ]] || fail "missing artifacts/vision_v0_classes.txt"

printf 'MULTIMODAL_PREPARE_PASS\n'
