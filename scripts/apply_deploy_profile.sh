#!/usr/bin/env bash
# Apply the CNET deploy profile into the current shell environment.
# Usage:
#   source scripts/apply_deploy_profile.sh
#   # or (prints export lines for eval):
#   eval "$(scripts/apply_deploy_profile.sh --print)"
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILE="${CNET_DEPLOY_ENV:-$ROOT/config/cnet-deploy.env}"
if [[ ! -f "$PROFILE" ]]; then
  echo "apply_deploy_profile: missing $PROFILE" >&2
  exit 1
fi
if [[ "${1:-}" == "--print" ]]; then
  # shellcheck disable=SC1090
  set -a
  # shellcheck source=/dev/null
  . "$PROFILE"
  set +a
  env | grep -E '^CNET_|^OMP_NUM_THREADS=' | sort | sed 's/^/export /'
  exit 0
fi
# When sourced:
if [[ "${BASH_SOURCE[0]}" != "${0}" ]]; then
  set -a
  # shellcheck source=/dev/null
  . "$PROFILE"
  set +a
  echo "CNET deploy profile applied from $PROFILE" >&2
  return 0 2>/dev/null || true
fi
# When executed: print a one-line status after loading in a subshell
set -a
# shellcheck source=/dev/null
. "$PROFILE"
set +a
echo "CNET_DEPLOY_PROFILE_OK file=$PROFILE train_fast=${CNET_TRAIN_FAST:-} int8=${CNET_ORACLE_INT8:-} stages=${CNET_ACQ_STAGES:-} max_closures=${CNET_LANE_MAX_CLOSURES:-} idle=${CNET_TEACHER_IDLE_SEC:-}"
