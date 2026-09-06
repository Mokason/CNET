#!/usr/bin/env bash
set -eu
case "$2" in
    show) if [[ -f $TEST_LANE_STATE ]]; then echo 0; else echo "$TEST_LANE_PID"; fi ;;
    stop) touch "$TEST_LANE_STATE"; echo stop >> "$TEST_LANE_LOG" ;;
    start) rm -f "$TEST_LANE_STATE"; echo start >> "$TEST_LANE_LOG" ;;
    *) exit 2 ;;
esac
