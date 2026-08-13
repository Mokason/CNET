#!/usr/bin/env bash
# Deploy profile static gate: free-win knobs present, quality floors intact.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
fail() { printf 'DEPLOY_PROFILE_FAIL: %s\n' "$1" >&2; exit 1; }

PROFILE=config/cnet-deploy.env
SERVICE=config/cnet-gap-lane.service
V2=config/qwythos_v2_campaign.env
APPLY=scripts/apply_deploy_profile.sh

[[ -f "$PROFILE" ]] || fail "missing $PROFILE"
text=$(cat "$PROFILE")
for key in \
    CNET_ORACLE_INT8=1 \
    CNET_TRAIN_FAST=1 \
    CNET_ACQ_STAGES=40 \
    CNET_LANE_MAX_CLOSURES=4 \
    CNET_TEACHER_IDLE_SEC=300 \
    CNET_HEALTH_TICK_SECONDS=300
do
    printf '%s' "$text" | grep -Fq "$key" || fail "missing deploy free-win $key"
done
if printf '%s\n' "$text" | grep -Eq '^export CNET_TOPK_SET=1'; then
    fail "TOPK_SET must not be silent deploy default"
fi

v2=$(cat "$V2")
printf '%s' "$v2" | grep -Fq 'CNET_TOPK_SET=1' || fail "v2 campaign missing CNET_TOPK_SET=1"
printf '%s' "$v2" | grep -Fq 'NEW goldens' || fail "v2 campaign missing NEW goldens note"

svc=$(cat "$SERVICE")
for key in CNET_ORACLE_INT8=1 CNET_TRAIN_FAST CNET_ACQ_STAGES CNET_LANE_MAX_CLOSURES CNET_TEACHER_IDLE_SEC; do
    printf '%s' "$svc" | grep -Fq "$key" || fail "gap-lane service missing $key"
done

out=$(bash "$APPLY") || fail "apply_deploy_profile.sh exited non-zero"
printf '%s' "$out" | grep -Fq 'CNET_DEPLOY_PROFILE_OK' ||
    fail "apply script did not print CNET_DEPLOY_PROFILE_OK"

printf 'DEPLOY_PROFILE_PASS\n'
