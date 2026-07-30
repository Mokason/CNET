#!/bin/sh
# A trained artifact that FAILED must leave no artifact behind.
#
# WHY THIS EXISTS. src/legacy/main.c trains nine artifacts and used to write
# every one of them unconditionally. A first pass gated four; a review found the
# other five still writing whatever training produced. Two of those five --
# `hex_char` and `word` -- were at that moment shipping weights for runs that
# had reported +inf, the value meaning THIS NET IS NOT FIT TO CERTIFY:
#
#     hex character final loss: inf      hex_char_weights.txt   3530 bytes
#     word final loss: inf               word_weights.txt       6136 bytes
#
# The gate can only be trusted if a genuine failure is observable, so each run
# here fails ONE artifact for real: CNET_DEMO_FAIL_ARTIFACT gives that
# artifact's training run a zero-epoch budget. It cannot reach its target
# because it was never trained -- the guard is not bypassed, the target is not
# moved, and no value of the variable can make a run succeed.
#
# Per artifact this asserts:
#   * the demo exits non-zero;
#   * it prints persist=REFUSED for that artifact and never persist=allowed;
#   * that artifact's weights AND contract are ABSENT;
#   * every artifact written before it is PRESENT -- the refusal is specific,
#     not a run that died early;
#   * every artifact written after it is absent.
# Then one unchanged run must persist all nine.
#
# Everything runs in a mkdtemp CWD. The repository is never written to.
#
# Usage: sh tests/test_legacy_persist_guard.sh
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEMO=${CNET_DEMO_BIN:-$ROOT/bin/nn_demo}
TIMEOUT=${CNET_DEMO_TIMEOUT:-300}

if [ ! -x "$DEMO" ]; then
    echo "LEGACY_PERSIST_GUARD_FAIL reason=no_demo_binary path=$DEMO"
    exit 1
fi

checks=0
failures=0

check() {
    checks=$((checks + 1))
    if [ "$1" != "0" ]; then
        failures=$((failures + 1))
        echo "FAIL: $2"
    fi
}

# Artifact -> the files it writes. Artifacts are listed in the order the demo
# writes them; `hex_char` and `word` share one write block, so they share a
# group index.
artifacts="nibble increment combine split conditional_increment hex_value hex_char word raw_word"

group_of() {
    case "$1" in
        nibble) echo 0 ;;
        increment) echo 1 ;;
        combine) echo 2 ;;
        split) echo 3 ;;
        conditional_increment) echo 4 ;;
        hex_value) echo 5 ;;
        hex_char) echo 6 ;;
        word) echo 6 ;;
        raw_word) echo 7 ;;
    esac
}

files_of() {
    case "$1" in
        nibble) echo "nibble_weights.txt" ;;
        increment) echo "increment_weights.txt increment_contract.txt" ;;
        combine) echo "combine_weights.txt combine_contract.txt" ;;
        split) echo "split_weights.txt split_contract.txt" ;;
        conditional_increment)
            echo "cond_increment_weights.txt cond_increment_contract.txt" ;;
        hex_value) echo "hex_value_weights.txt hex_value_contract.txt" ;;
        hex_char) echo "hex_char_weights.txt hex_char_contract.txt" ;;
        word) echo "word_weights.txt word_contract.txt" ;;
        raw_word) echo "raw_word_weights.txt raw_word_contract.txt" ;;
    esac
}

for forced in $artifacts; do
    before=$failures
    work=$(mktemp -d "${TMPDIR:-/tmp}/cnet-persist-guard-XXXXXX") || exit 1
    (
        cd "$work" || exit 1
        CNET_DEMO_FAIL_ARTIFACT="$forced" timeout "$TIMEOUT" "$DEMO" \
            > run.log 2>&1
        echo $? > rc
    )
    rc=$(cat "$work/rc" 2>/dev/null || echo "no-rc")
    log="$work/run.log"

    check "$([ "$rc" != "0" ] && echo 0 || echo 1)" \
        "$forced: a demo whose training failed must exit non-zero (rc=$rc)"
    if grep -q "TRAIN_STATUS name=$forced .*persist=REFUSED" "$log" 2>/dev/null
    then
        check 0 ""
    else
        check 1 "$forced: the run must print persist=REFUSED for it"
    fi
    if grep -q "TRAIN_STATUS name=$forced .*persist=allowed" "$log" 2>/dev/null
    then
        check 1 "$forced: a failed run must never report persist=allowed"
    else
        check 0 ""
    fi

    forced_group=$(group_of "$forced")
    for other in $artifacts; do
        other_group=$(group_of "$other")
        for file in $(files_of "$other"); do
            if [ "$other_group" -lt "$forced_group" ]; then
                if [ -f "$work/$file" ]; then
                    check 0 ""
                else
                    check 1 "$forced: $file was written before the failure and \
must still be there -- the refusal is specific, not an early death"
                fi
            else
                if [ -f "$work/$file" ]; then
                    check 1 "$forced: $file must be ABSENT -- \
a failed or not-yet-reached artifact must not be on disk"
                else
                    check 0 ""
                fi
            fi
        done
    done
    # Report what happened, never a fixed reassuring string: a summary line
    # that says `refused=yes` under a wall of FAILs is the exact defect this
    # whole remediation is about.
    echo "PERSIST_GUARD forced=$forced rc=$rc failures=$((failures - before))"
    rm -rf "$work"
done

# ---- the unchanged run: every artifact still persists -------------------
work=$(mktemp -d "${TMPDIR:-/tmp}/cnet-persist-guard-clean-XXXXXX") || exit 1
(
    cd "$work" || exit 1
    timeout "$TIMEOUT" "$DEMO" > run.log 2>&1
    echo $? > rc
)
rc=$(cat "$work/rc" 2>/dev/null || echo "no-rc")
log="$work/run.log"
check "$([ "$rc" = "0" ] && echo 0 || echo 1)" \
    "the unchanged run still succeeds (rc=$rc)"
allowed=0
for name in $artifacts; do
    if grep -q "TRAIN_STATUS name=$name .*persist=allowed" "$log" 2>/dev/null
    then
        allowed=$((allowed + 1))
        check 0 ""
    else
        check 1 "$name: the unchanged run must report persist=allowed"
    fi
    for file in $(files_of "$name"); do
        if [ -f "$work/$file" ]; then
            check 0 ""
        else
            check 1 "$name: the unchanged run must still write $file"
        fi
    done
done
if grep -q "persist=REFUSED" "$log" 2>/dev/null; then
    check 1 "the unchanged run must not refuse anything"
else
    check 0 ""
fi
echo "PERSIST_GUARD_CLEAN rc=$rc persist_allowed=$allowed/9"
rm -rf "$work"

if [ "$failures" != "0" ]; then
    echo "LEGACY_PERSIST_GUARD_FAIL checks=$checks failures=$failures"
    exit 1
fi
echo "LEGACY_PERSIST_GUARD_PASS checks=$checks artifacts=9 \
forced_failures=9 unchanged_run=persists_all"
exit 0
