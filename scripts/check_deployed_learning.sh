#!/usr/bin/env bash
# Inspect the running lane's effective environment, not a guessed base path or
# a profile that may have changed since startup. No secrets are printed.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
health=${CNET_HEALTH_BIN:-$root/bin/cnet_own_learning_health}
service=${CNET_HEALTH_SERVICE:-cnet-personal-ai-lane.service}
pid=$(systemctl --user show "$service" -p MainPID --value)
if [[ ! $pid =~ ^[1-9][0-9]*$ || ! -r /proc/$pid/environ ]]; then
    echo 'OWN_LEARNING_HEALTH_BLOCKED reason=no_inspectable_running_lane'
    exit 4
fi
generation=$(stat -Lc '%d:%i' "/proc/$pid/exe")
effective=()
base=''
while IFS= read -r -d '' entry; do
    case "$entry" in
        CNET_BASE_PATH=*) base=${entry#*=}; effective+=("$entry") ;;
        CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE=*|CNET_COVERAGE_ABSTAIN=*|CNET_RESIDUAL_HTTP=*|CNET_RESIDUAL_GGUF=*) effective+=("$entry") ;;
    esac
done < "/proc/$pid/environ"
if [[ -z $base || ! -f $base ]]; then
    echo 'OWN_LEARNING_HEALTH_BLOCKED reason=running_lane_base_missing'
    exit 4
fi
printf 'DEPLOYED_LEARNING_INSPECT pid=%s executable_generation=%s\n' "$pid" "$generation"
status=0
env -i PATH="$PATH" "${effective[@]}" "$health" --base "$base" || status=$?
if [[ $(systemctl --user show "$service" -p MainPID --value) != "$pid" ||
      $(stat -Lc '%d:%i' "/proc/$pid/exe") != "$generation" ]]; then
    echo 'OWN_LEARNING_HEALTH_FAIL reason=deployment_changed_during_inspection'
    exit 1
fi
exit "$status"
