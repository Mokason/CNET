#!/usr/bin/env bash
# cnetd must exit promptly on SIGTERM.
#
# RED marker: CNETD_SIGTERM_RED — cnetd installs its handlers with signal(),
# which carries SA_RESTART on glibc, so the blocked accept() is restarted
# instead of returning EINTR. g_stop is set but the loop condition is never
# re-tested and the daemon sleeps in accept() forever. Anything that does
# "kill $PID; wait $PID" (scripts/cnet_core_e2e_smoke.sh cleanup) then blocks
# for its whole timeout.
#
# This gate fails closed: a daemon that ignores SIGTERM is a hang, not a pass.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

DEADLINE="${CNETD_SIGTERM_DEADLINE:-5}"   # seconds cnetd is allowed to take
SOCK="/tmp/cnetd-sigterm-$$.sock"
LOG="${CNETD_SIGTERM_LOG:-$ROOT/logs/cnetd_sigterm.log}"
BIN="${CNETD_BIN:-$ROOT/bin/cnetd}"

export CNET_SOCK="$SOCK"
export CNET_PACKS_ROOT="${CNET_PACKS_ROOT:-$ROOT/artifacts/roe_daily_packs}"
export CNET_MISS_LOG="/tmp/cnetd-sigterm-miss-$$.jsonl"
export CNET_BRAIN_MIRROR_DIR="/tmp/cnetd-sigterm-mirror-$$"

mkdir -p "$(dirname "$LOG")" "$CNET_BRAIN_MIRROR_DIR"
: >"$LOG"

fails=0
check() { # name ok detail
  if [[ $2 == 1 ]]; then
    printf '  ok   %-28s %s\n' "$1" "${3:-}" | tee -a "$LOG"
  else
    printf '  FAIL %-28s %s\n' "$1" "${3:-}" | tee -a "$LOG"
    fails=$((fails + 1))
  fi
}

[[ -x $BIN ]] || make cnetd >/dev/null

rm -f "$SOCK"
"$BIN" >>"$LOG" 2>&1 &
PID=$!
cleanup() {
  kill -9 "$PID" 2>/dev/null || true
  rm -f "$SOCK" "$CNET_MISS_LOG"
  rm -rf "$CNET_BRAIN_MIRROR_DIR"
}
trap cleanup EXIT

for _ in $(seq 1 50); do [[ -S $SOCK ]] && break; sleep 0.1; done
check sock_ready "$([[ -S $SOCK ]] && echo 1 || echo 0)" "sock=$SOCK"

# A) plain SIGTERM on an IDLE daemon must be honoured within DEADLINE.
#    Idle is the case that matters: it is exactly when accept() is blocked.
kill -TERM "$PID" 2>/dev/null || true
waited=0
while kill -0 "$PID" 2>/dev/null; do
  if [[ $waited -ge $((DEADLINE * 10)) ]]; then break; fi
  sleep 0.1
  waited=$((waited + 1))
done
alive=0
kill -0 "$PID" 2>/dev/null && alive=1
check sigterm_exits_idle "$([[ $alive == 0 ]] && echo 1 || echo 0)" \
  "waited=$(awk "BEGIN{printf \"%.1f\", $waited/10}")s deadline=${DEADLINE}s"

# B) the socket must be cleaned up on the way out (the loop runs to completion
#    rather than being killed), proving an orderly shutdown not a hard kill.
if [[ $alive == 0 ]]; then
  check sock_unlinked "$([[ ! -S $SOCK ]] && echo 1 || echo 0)" "sock=$SOCK"
else
  check sock_unlinked 0 "daemon still alive"
fi

# C) `wait` must not block — this is the exact call the e2e smoke cleanup makes.
if [[ $alive == 0 ]]; then
  check wait_returns 1 "reaped"
else
  check wait_returns 0 "wait would block forever"
fi

if [[ $fails -eq 0 ]]; then
  echo "CNETD_SIGTERM_PASS checks=3 fails=0 deadline=${DEADLINE}s" | tee -a "$LOG"
else
  echo "CNETD_SIGTERM_RED checks=3 fails=$fails" | tee -a "$LOG"
  exit 1
fi
