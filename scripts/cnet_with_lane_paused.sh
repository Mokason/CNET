#!/usr/bin/env bash
# Serialize legacy base maintenance and resume the exact learner afterwards.
# The command must also hold BASE.writer.lock (struct_mine_persist does so).
set -euo pipefail
base=${1:?absolute base required}; shift
[[ $base = /* && -f $base && $# -gt 0 ]] || exit 2
exec 9>"$base.maintenance.lock"
flock -n 9 || { echo LANE_MAINTENANCE_BUSY; exit 3; }
service=cnet-personal-ai-lane.service
resume=0
restore() {
    rc=$?
    trap - EXIT
    if (( resume )); then systemctl --user start "$service" || rc=1; fi
    exit "$rc"
}
trap restore EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
pid=$(systemctl --user show "$service" -p MainPID --value)
if [[ $pid =~ ^[1-9][0-9]*$ ]]; then
    [[ -r /proc/$pid/environ ]] || exit 4
    effective=''
    while IFS= read -r -d '' entry; do
        case "$entry" in CNET_BASE_PATH=*) effective=${entry#*=} ;; esac
    done < "/proc/$pid/environ"
    if [[ $effective != "$base" ]]; then echo LANE_MAINTENANCE_REFUSED base_mismatch; exit 4; fi
    resume=1
    systemctl --user stop "$service"
    [[ $(systemctl --user show "$service" -p MainPID --value) == 0 ]] || exit 4
fi
"$@"
