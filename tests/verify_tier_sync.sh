#!/bin/sh
# verify_tier_sync.sh -- CORE log rows must be produced by T1 deps.
#
# Catches the STALE class: verify_logs.sh CORE lists a suite that verify_impl
# never runs. Also refuses T2 soak targets leaking back into default T1.

set -u
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
TIERS="$ROOT/mk/verify_tiers.mk"
LOGS_SH="$ROOT/tests/verify_logs.sh"
fail=0

[ -f "$TIERS" ] && [ -f "$LOGS_SH" ] || {
    echo "VERIFY_TIER_SYNC_FAIL missing $TIERS or $LOGS_SH"
    exit 1
}

# Collect deps: lines after VERIFY_Tx_DEPS := until a non-continued line ends the block.
collect_deps() {
    var=$1
    awk -v var="$var" '
        BEGIN { grab=0 }
        index($0, var " :=") == 1 || index($0, var ":=") == 1 {
            grab=1
            line=$0
            sub(/^[^=]*=[ \t]*/, "", line)
            cont = (line ~ /\\[ \t]*$/)
            gsub(/\\[ \t]*$/, "", line)
            gsub(/[ \t]+/, " ", line)
            gsub(/^ /, "", line)
            gsub(/ $/, "", line)
            if (length(line)) print line
            if (!cont) exit
            next
        }
        grab {
            line=$0
            cont = (line ~ /\\[ \t]*$/)
            gsub(/\\[ \t]*$/, "", line)
            gsub(/[ \t]+/, " ", line)
            gsub(/^ /, "", line)
            gsub(/ $/, "", line)
            if (length(line)) print line
            if (!cont) exit
        }
    ' "$TIERS" | tr ' ' '\n' | sed '/^$/d'
}

T1=$(collect_deps VERIFY_T1_DEPS | tr '\n' ' ')
T2=$(collect_deps VERIFY_T2_DEPS | tr '\n' ' ')

CORE_LOGS=$(sed -n "/^CORE='/,/^'/p" "$LOGS_SH" | grep '|||' | sed 's/|||.*//' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')

log_to_target() {
    case "$1" in
        makefile_budget.log|orphan_tools.log|orphan_tests.log|layering_guard.log|\
        curl_guard.log|platform_sweep.log|residual_http_nocurl.log)
            printf '%s\n' build_integrity ;;
        decimal_demo.log|circuit_demo.log)
            printf '%s\n' demos ;;
        *.log)
            printf '%s\n' "${1%.log}" ;;
        *)
            printf '\n' ;;
    esac
}

in_list() {
    needle=$1
    list=$2
    case " $list " in
        *" $needle "*) return 0 ;;
        *) return 1 ;;
    esac
}

echo "T1: $T1"
echo "T2: $T2"

for t in $T2; do
    if in_list "$t" "$T1"; then
        echo "VERIFY_TIER_SYNC_FAIL T2 target '$t' is also in T1"
        fail=$((fail + 1))
    fi
done

for log in $CORE_LOGS; do
    tgt=$(log_to_target "$log")
    if [ -z "$tgt" ]; then
        echo "VERIFY_TIER_SYNC_FAIL no target mapping for $log"
        fail=$((fail + 1))
        continue
    fi
    if ! in_list "$tgt" "$T1"; then
        echo "VERIFY_TIER_SYNC_FAIL CORE log $log needs T1 dep '$tgt' (missing)"
        fail=$((fail + 1))
    fi
done

n1=0; for _ in $T1; do n1=$((n1 + 1)); done
n2=0; for _ in $T2; do n2=$((n2 + 1)); done
ncore=0; for _ in $CORE_LOGS; do ncore=$((ncore + 1)); done

# Floors/ceilings — membership size bands, not exact snapshots.
if [ "$n1" -lt 20 ] || [ "$n1" -gt 40 ]; then
    echo "VERIFY_TIER_SYNC_FAIL T1 count $n1 outside [20,40]"
    fail=$((fail + 1))
fi
if [ "$n2" -lt 10 ] || [ "$n2" -gt 30 ]; then
    echo "VERIFY_TIER_SYNC_FAIL T2 count $n2 outside [10,30]"
    fail=$((fail + 1))
fi
if [ "$ncore" -lt 20 ] || [ "$ncore" -gt 35 ]; then
    echo "VERIFY_TIER_SYNC_FAIL CORE log count $ncore outside [20,35]"
    fail=$((fail + 1))
fi

if [ "$fail" -ne 0 ]; then
    echo "VERIFY_TIER_SYNC_FAIL failed=$fail"
    exit 1
fi
echo "VERIFY_TIER_SYNC_PASS t1=$n1 t2=$n2 core_logs=$ncore"
exit 0
