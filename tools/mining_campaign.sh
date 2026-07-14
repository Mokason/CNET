#!/bin/bash
# CNET mining campaign: a manifest in, a finished base out.
#
# Drives flagship_run to convergence without a human babysitter:
#   1. launches the run as a memory-capped transient systemd unit
#      (MemoryHigh/MemoryMax — flagship must NEVER run uncapped, it has
#      OOM'd this box twice),
#   2. waits, watching the ledger; on completion checks the deferred set,
#   3. chains retry passes with fresh student seeds until a pass converts
#      nothing new (loop-until-dry), each pass resuming the same base,
#   4. writes <base>.campaign.txt with the final tally.
#
# Usage: mining_campaign.sh <manifest.json> [max_passes] [seed0]
# Requires: passwordless systemd --user; the manifest's model/base paths.
# Stop everything cleanly: touch <base>.stop  (flagship honors it) or
#   systemctl --user stop cnet-campaign.
set -u

MANIFEST="${1:?usage: mining_campaign.sh <manifest.json> [max_passes] [seed0]}"
MAX_PASSES="${2:-4}"
SEED0="${3:-42}"
UNIT="cnet-campaign"
CNET_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BASE="${MANIFEST%.manifest.json}"
LOG="${BASE}.campaign.log"
REPORT="${BASE}.campaign.txt"

# Ledger-version-robust unit counting: v1 acquired rows end with "-" (the
# defer-reason column empty); v4 rows append the bound unit name ("acq_*")
# plus counters for acquired units, while deferred rows carry the reason and
# never a unit name. Counting only $NF=="-" (the v1 rule) reads every v4
# acquisition as a deferral and dry-stops the retry loop after pass 1.
acquired_count() {
    tail -n +3 "${BASE}.gaps.txt" 2>/dev/null | \
        awk '($NF=="-" || index($0, " acq_") > 0) {n++} END{print n+0}'
}
deferred_count() {
    tail -n +3 "${BASE}.gaps.txt" 2>/dev/null | \
        awk '!($NF=="-" || index($0, " acq_") > 0) {n++} END{print n+0}'
}

run_pass() {
    local seed="$1"
    systemctl --user reset-failed "$UNIT" 2>/dev/null
    systemd-run --user --unit="$UNIT" \
        -p MemoryHigh=72G -p MemoryMax=80G \
        -p WorkingDirectory="$CNET_DIR" \
        -p StandardOutput=append:"$LOG" \
        -p StandardError=append:"$LOG" \
        -E CNET_MANIFEST="$MANIFEST" \
        -E CNET_ACQ_SEED="$seed" \
        "$CNET_DIR/bin/flagship_run" || return 1
    # wait for the unit to finish (flagship exits on sweep completion)
    while systemctl --user is-active --quiet "$UNIT"; do
        sleep 60
        # safety: if system RAM ever hits the floor, stop the campaign
        local avail
        avail=$(free -g | awk 'NR==2{print $7}')
        if [ "${avail:-99}" -le 2 ]; then
            echo "$(date -Is) campaign: ${avail}GB free — stopping" >> "$LOG"
            touch "${BASE}.stop"
            sleep 120
            systemctl --user stop "$UNIT" 2>/dev/null
            rm -f "${BASE}.stop"
            return 2
        fi
    done
    return 0
}

echo "campaign start $(date -Is): manifest=$MANIFEST passes<=$MAX_PASSES" >> "$LOG"
prev_acquired=$(acquired_count)
pass=0
while [ "$pass" -lt "$MAX_PASSES" ]; do
    seed=$((SEED0 + pass))
    echo "campaign pass $((pass + 1)) seed=$seed ($(date -Is))" >> "$LOG"
    run_pass "$seed" || { echo "campaign: pass failed/stopped" >> "$LOG"; break; }
    now_acquired=$(acquired_count)
    echo "campaign pass $((pass + 1)) done: acquired=$now_acquired (+$((now_acquired - prev_acquired))), deferred=$(deferred_count)" >> "$LOG"
    if [ "$now_acquired" -le "$prev_acquired" ]; then
        echo "campaign: dry pass — converged" >> "$LOG"
        break
    fi
    prev_acquired=$now_acquired
    pass=$((pass + 1))
done

{
    echo "campaign report $(date -Is)"
    echo "manifest: $MANIFEST"
    echo "passes run: $((pass + 1))"
    echo "acquired: $(acquired_count)"
    echo "deferred: $(deferred_count)"
    grep -a "flagship:" "$LOG" | tail -1
} > "$REPORT"
echo "campaign complete: $(cat "$REPORT" | tr '\n' ' | ')"
