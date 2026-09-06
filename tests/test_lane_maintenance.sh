#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d /tmp/cnet-lane-maintenance-XXXXXX)
pid=''
trap 'rc=$?; [[ -z $pid ]] || { kill "$pid" 2>/dev/null || :; wait "$pid" 2>/dev/null || :; }; if ((rc)); then echo LANE_MAINTENANCE_RED; fi; rm -rf -- "$root"' EXIT
touch "$root/base.cnb" "$root/other.cnb"
mkdir "$root/bin"
ln -s "$PWD/tests/fixtures/fake_lane_systemctl.sh" "$root/bin/systemctl"
env CNET_BASE_PATH="$root/base.cnb" sleep 60 & pid=$!
export TEST_LANE_PID=$pid TEST_LANE_STATE="$root/state" TEST_LANE_LOG="$root/log"
export PATH="$root/bin:$PATH"
if bash scripts/cnet_with_lane_paused.sh "$root/base.cnb" /bin/false; then exit 1; fi
test "$(tr '\n' ' ' < "$root/log")" = 'stop start '
if bash scripts/cnet_with_lane_paused.sh "$root/other.cnb" touch "$root/should_not_run"; then exit 1; fi
test ! -e "$root/should_not_run"
bash scripts/cnet_with_lane_paused.sh "$root/base.cnb" touch "$root/success"
test -e "$root/success"
echo LANE_MAINTENANCE_PASS
