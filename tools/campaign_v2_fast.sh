#!/bin/bash
# Quality-preserving FAST path for a TOPK_SET (v2) mining campaign.
#
# Stacks only measured free wins + sweep screening + optional sharding:
#   1. deploy free wins (int8, train_fast, stages=40, adaptive, OMP=4)
#   2. CNET_TOPK_SET=1 (set-valued claims — requires NEW goldens)
#   3. CNET_UNIT_ALLOWLIST from margin-sweep predicted-minable set
#   4. optional CNET_UNIT_START / CNET_UNIT_END for multi-worker shards
#
# Does NOT lower margin / Wilson / certify bars.
#
# Usage:
#   tools/campaign_v2_fast.sh prepare \
#       --sweep qwythos_english_v1.margin_sweep.tsv \
#       --out-allowlist artifacts/qwythos_v2_minable.ids \
#       [--eps 0.02] [--semantics set]
#
#   tools/campaign_v2_fast.sh estimate \
#       --allowlist artifacts/qwythos_v2_minable.ids \
#       [--sec-per-unit 70] [--workers 1]
#
#   # then (operator starts the real mine; this only prints the env recipe):
#   tools/campaign_v2_fast.sh env \
#       --allowlist artifacts/qwythos_v2_minable.ids \
#       [--start 0] [--end 128]
#
#   # drive existing mining_campaign.sh with the fast env preloaded:
#   eval "$(tools/campaign_v2_fast.sh env --allowlist ...)"
#   tools/mining_campaign.sh <manifest.json> [max_passes] [seed0]
set -euo pipefail
CNET_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CMD="${1:-}"
shift || true

EPS=0.02
SEM=set
SWEEP=""
ALLOW=""
START=""
END=""
SEC_PER=70
WORKERS=1

while [ $# -gt 0 ]; do
    case "$1" in
        --sweep) SWEEP="$2"; shift 2 ;;
        --out-allowlist|--allowlist) ALLOW="$2"; shift 2 ;;
        --eps) EPS="$2"; shift 2 ;;
        --semantics) SEM="$2"; shift 2 ;;
        --start) START="$2"; shift 2 ;;
        --end) END="$2"; shift 2 ;;
        --sec-per-unit) SEC_PER="$2"; shift 2 ;;
        --workers) WORKERS="$2"; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

prepare() {
    if [ -z "$SWEEP" ] || [ -z "$ALLOW" ]; then
        echo "prepare requires --sweep and --out-allowlist" >&2
        exit 2
    fi
    mkdir -p "$(dirname "$ALLOW")"
    python3 "$CNET_DIR/tools/margin_sweep_analyze.py" "$SWEEP" \
        --eps "$EPS" --semantics "$SEM" \
        --export-minable "$ALLOW" \
        --export-unfit "${ALLOW%.ids}_unfit.ids" \
        > "${ALLOW%.ids}_prepare.log"
    n=$(grep -v '^#' "$ALLOW" | grep -c . || true)
    echo "CAMPAIGN_V2_FAST_PREPARE_PASS minable=$n allowlist=$ALLOW"
    echo "  log: ${ALLOW%.ids}_prepare.log"
}

emit_env() {
    if [ -z "$ALLOW" ] || [ ! -f "$ALLOW" ]; then
        echo "env requires an existing --allowlist file" >&2
        exit 2
    fi
    # shellcheck disable=SC1091
    set -a
    # deploy free wins (do not override already-set operator vars)
    # shellcheck source=/dev/null
    . "$CNET_DIR/config/cnet-deploy.env"
    # shellcheck source=/dev/null
    . "$CNET_DIR/config/qwythos_v2_campaign.env"
    export CNET_UNIT_ALLOWLIST="$(cd "$(dirname "$ALLOW")" && pwd)/$(basename "$ALLOW")"
    [ -n "$START" ] && export CNET_UNIT_START="$START"
    [ -n "$END" ] && export CNET_UNIT_END="$END"
    set +a
    cat <<EOF
# campaign v2-fast env (source or eval)
export CNET_ORACLE_INT8=${CNET_ORACLE_INT8}
export CNET_TRAIN_FAST=${CNET_TRAIN_FAST}
export CNET_ACQ_STAGES=${CNET_ACQ_STAGES}
export CNET_ACQ_ADAPTIVE=${CNET_ACQ_ADAPTIVE}
export CNET_LANE_PROBE_BATCH=${CNET_LANE_PROBE_BATCH:-32}
export OMP_NUM_THREADS=${OMP_NUM_THREADS:-4}
export CNET_TOPK_SET=${CNET_TOPK_SET}
export CNET_UNIT_ALLOWLIST=${CNET_UNIT_ALLOWLIST}
${START:+export CNET_UNIT_START=$START}
${END:+export CNET_UNIT_END=$END}
EOF
}

estimate() {
    if [ -z "$ALLOW" ] || [ ! -f "$ALLOW" ]; then
        echo "estimate requires --allowlist" >&2
        exit 2
    fi
    n=$(grep -v '^#' "$ALLOW" | grep -c . || true)
    # sec_per_unit defaults from deploy stack (~70s mid unit with free wins)
    # workers shard independent index ranges
    total_sec=$(awk -v n="$n" -v s="$SEC_PER" -v w="$WORKERS" \
        'BEGIN{ if(w<1)w=1; printf "%.0f", (n*s)/w }')
    h=$(awk -v t="$total_sec" 'BEGIN{printf "%.1f", t/3600}')
    # add ~15% overhead for load/goldens/retries/pass2 residue
    h_wall=$(awk -v h="$h" 'BEGIN{printf "%.1f", h*1.15}')
    cat <<EOF
CAMPAIGN_V2_FAST_ESTIMATE
  minable_units:     $n  (from allowlist)
  sec_per_unit:      $SEC_PER  (deploy free-wins teach budget)
  workers:           $WORKERS
  pure_teach_hours:  $h
  wall_hours_~1.15x: $h_wall   (load + goldens + retries)
  notes:
    - screen skips predicted-unfit: no full train on them
    - TOPK_SET changes unit claims: NEW goldens required
    - dual-GPU mining (if identity-green) can cut teacher portion further
EOF
}

case "$CMD" in
    prepare) prepare ;;
    env) emit_env ;;
    estimate) estimate ;;
    *)
        echo "usage: $0 prepare|env|estimate [options]" >&2
        exit 2
        ;;
esac
