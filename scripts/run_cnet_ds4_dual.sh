#!/usr/bin/env bash
set -euo pipefail

ACTION=${1:-status}
DS4_ROOT=${CNET_DS4_ROOT:-/home/marble/AI/ds4}
DS4_SERVER=${CNET_DS4_SERVER:-$DS4_ROOT/ds4-server}
MODEL=${CNET_DS4_MODEL:-$DS4_ROOT/gguf/DeepSeek-V4-Flash-IQ2XXS-w2Q2K-AProjQ8-SExpQ8-OutQ8-chat-v2-imatrix.gguf}
API_HOST=${CNET_DS4_API_HOST:-127.0.0.1}
API_PORT=${CNET_DS4_API_PORT:-8082}
DIST_HOST=${CNET_DS4_DIST_HOST:-127.0.0.1}
DIST_PORT=${CNET_DS4_DIST_PORT:-9411}
LAYER_SPLIT=${CNET_DS4_LAYER_SPLIT:-20}
LAYER_AHEAD=${CNET_DS4_LAYER_AHEAD:-2}
PREFILL_WINDOW=${CNET_DS4_PREFILL_WINDOW:-4}
CTX=${CNET_DS4_CTX:-32768}
CACHE_EXPERTS=${CNET_DS4_CACHE_EXPERTS:-1GB}
RESERVE_GB=${CNET_DS4_RESERVE_GB:-1}
ARENA_CHUNK_MB=${CNET_DS4_ARENA_CHUNK_MB:-512}
NO_Q8_F16_CACHE=${CNET_DS4_NO_Q8_F16_CACHE:-1}
BOUNDED_LAYER_MAP=${CNET_DS4_BOUNDED_LAYER_MAP:-1}
READY_TIMEOUT=${CNET_DS4_READY_TIMEOUT_SEC:-1800}
STATE_DIR=${CNET_DS4_STATE_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/cnet-ds4-dual}
UNIT_PREFIX=${CNET_DS4_UNIT_PREFIX:-cnet-ds4-dual}
WORKER_UNIT=${UNIT_PREFIX}-worker.service
COORD_UNIT=${UNIT_PREFIX}-coordinator.service
READY_FILE=$STATE_DIR/ready.json
VERIFY_FILE=$STATE_DIR/inference-verification.json
IDENTITY_FILE=${CNET_DS4_IDENTITY_FILE:-$MODEL.cnet-identity}
SCRIPT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)

mkdir -p "$STATE_DIR"

worker_command=(
    "$DS4_SERVER"
    --role worker
    --layers "$((LAYER_SPLIT + 1)):output"
    --coordinator "$DIST_HOST" "$DIST_PORT"
    -m "$MODEL"
    --ssd-streaming
    --ssd-streaming-cache-experts "$CACHE_EXPERTS"
    -c "$CTX"
)
coordinator_command=(
    "$DS4_SERVER"
    --role coordinator
    --layers "0:$LAYER_SPLIT"
    --listen "$DIST_HOST" "$DIST_PORT"
    --dist-prefill-window "$PREFILL_WINDOW"
    --host "$API_HOST"
    --port "$API_PORT"
    -m "$MODEL"
    --ssd-streaming
    --ssd-streaming-cache-experts "$CACHE_EXPERTS"
    -c "$CTX"
)

print_shell_command() {
    local name=$1 gpu=$2 lock=$3
    shift 3
    printf '%s: HIP_VISIBLE_DEVICES=%q DS4_LOCK_FILE=%q ' "$name" "$gpu" "$lock"
    printf 'DS4_DIST_SOCKET_TIMEOUT_SEC=%q ' 600
    printf 'DS4_CUDA_STREAMING_EXPERT_CACHE_RESERVE_GB=%q ' "$RESERVE_GB"
    printf 'DS4_ROCM_MODEL_ARENA_CHUNK_MB=%q ' "$ARENA_CHUNK_MB"
    if [[ $NO_Q8_F16_CACHE == 1 ]]; then printf 'DS4_CUDA_NO_Q8_F16_CACHE=1 '; fi
    if [[ $BOUNDED_LAYER_MAP == 1 ]]; then printf 'DS4_METAL_DISABLE_STREAMING_STATIC_DECODE_MAP=1 '; fi
    printf 'DS4_ROCM_STREAM_PREFILL_LAYER_AHEAD=%q ' "$LAYER_AHEAD"
    printf '%q ' "$@"
    printf '\n'
}

unit_state() {
    local unit=$1
    if ! command -v systemctl >/dev/null 2>&1; then
        printf 'unavailable'
        return
    fi
    systemctl --user is-active "$unit" 2>/dev/null || true
}

health_ready() {
    curl -fsS --max-time 3 "http://$API_HOST:$API_PORT/v1/models" >/dev/null 2>&1
}

write_status() {
    local worker coordinator ready=false api_ready=false inference_verified=false
    worker=$(unit_state "$WORKER_UNIT")
    coordinator=$(unit_state "$COORD_UNIT")
    if [[ $worker == active && $coordinator == active ]] && health_ready; then
        api_ready=true
    fi
    if [[ $api_ready == true && -f $VERIFY_FILE ]] &&
       grep -q '"ok":true' "$VERIFY_FILE"; then
        inference_verified=true
        ready=true
    fi
    printf '{"ready":%s,"api_ready":%s,"inference_verified":%s,"worker":"%s","coordinator":"%s","endpoint":"http://%s:%s","resource_mask":3}\n' \
        "$ready" "$api_ready" "$inference_verified" "$worker" "$coordinator" "$API_HOST" "$API_PORT"
}

port_available() {
    local port=$1
    ! ss -ltnH "sport = :$port" 2>/dev/null | grep -q .
}

stop_units() {
    systemctl --user stop "$COORD_UNIT" >/dev/null 2>&1 || true
    systemctl --user stop "$WORKER_UNIT" >/dev/null 2>&1 || true
    systemctl --user reset-failed "$COORD_UNIT" "$WORKER_UNIT" >/dev/null 2>&1 || true
    rm -f "$READY_FILE" "$VERIFY_FILE"
}

start_unit() {
    local unit=$1 gpu=$2 lock=$3 logfile=$4
    local q8_cache_env=()
    local bounded_map_env=()
    shift 4
    if [[ $NO_Q8_F16_CACHE == 1 ]]; then
        q8_cache_env+=(--setenv=DS4_CUDA_NO_Q8_F16_CACHE=1)
    fi
    if [[ $BOUNDED_LAYER_MAP == 1 ]]; then
        bounded_map_env+=(--setenv=DS4_METAL_DISABLE_STREAMING_STATIC_DECODE_MAP=1)
    fi
    systemd-run --user --quiet --unit "$unit" --collect \
        --property=Restart=on-failure \
        --property=RestartSec=2 \
        --property=TimeoutStopSec=60 \
        --property="StandardOutput=append:$logfile" \
        --property="StandardError=append:$logfile" \
        --working-directory="$DS4_ROOT" \
        --setenv="HIP_VISIBLE_DEVICES=$gpu" \
        --setenv="DS4_LOCK_FILE=$lock" \
        --setenv=DS4_DIST_SOCKET_TIMEOUT_SEC=600 \
        --setenv="DS4_CUDA_STREAMING_EXPERT_CACHE_RESERVE_GB=$RESERVE_GB" \
        --setenv="DS4_ROCM_MODEL_ARENA_CHUNK_MB=$ARENA_CHUNK_MB" \
        "${q8_cache_env[@]}" \
        "${bounded_map_env[@]}" \
        --setenv="DS4_ROCM_STREAM_PREFILL_LAYER_AHEAD=$LAYER_AHEAD" \
        -- "$@"
}

case "$ACTION" in
    print-plan)
        print_shell_command worker 1 /tmp/cnet-ds4-worker-gpu1.lock \
            "${worker_command[@]}"
        print_shell_command coordinator 0 /tmp/cnet-ds4-coordinator-gpu0.lock \
            "${coordinator_command[@]}"
        ;;

    status)
        write_status
        ;;

    stop)
        stop_units
        write_status
        ;;

    logs)
        journalctl --user -u "$WORKER_UNIT" -u "$COORD_UNIT" -n 200 --no-pager
        ;;

    identity)
        if [[ ! -f $MODEL ]]; then
            echo "model not found: $MODEL" >&2
            exit 1
        fi
        python3 "$SCRIPT_ROOT/scripts/cnet_chunk_hash.py" \
            "$MODEL" "$IDENTITY_FILE"
        ;;

    verify)
        if [[ $(unit_state "$WORKER_UNIT") != active ||
              $(unit_state "$COORD_UNIT") != active ]] || ! health_ready; then
            echo "DS4 API is not ready; start the supervised dual service first" >&2
            exit 1
        fi
        python3 "$SCRIPT_ROOT/scripts/verify_ds4_endpoint.py" \
            "http://$API_HOST:$API_PORT" "$READY_FILE" "$VERIFY_FILE"
        write_status
        ;;

    start)
        if [[ ! -x $DS4_SERVER ]]; then
            echo "DS4 server binary not executable: $DS4_SERVER" >&2
            exit 1
        fi
        if [[ ! -f $MODEL ]]; then
            echo "DS4 model not found: $MODEL" >&2
            exit 1
        fi
        if ! command -v systemd-run >/dev/null 2>&1 ||
           ! systemctl --user show-environment >/dev/null 2>&1; then
            echo "user systemd is required for supervised, orphan-free launch" >&2
            exit 1
        fi
        if [[ $(unit_state "$WORKER_UNIT") == active ||
              $(unit_state "$COORD_UNIT") == active ]]; then
            echo "DS4 dual service is already active; use status or stop" >&2
            exit 1
        fi
        if ! port_available "$API_PORT" || ! port_available "$DIST_PORT"; then
            echo "API port $API_PORT or distributed port $DIST_PORT is already in use" >&2
            exit 1
        fi
        if [[ ${CNET_DS4_ALLOW_SHARED_GPU:-0} != 1 ]] &&
           rocm-smi --showpids 2>/dev/null | grep -E 'llama-server|GPU\[1\].*[1-9]' >/dev/null; then
            echo "GPU1 is occupied; refusing to oversubscribe. Stop the staged predecessor or set CNET_DS4_ALLOW_SHARED_GPU=1 explicitly." >&2
            exit 1
        fi

        rm -f "$READY_FILE" "$VERIFY_FILE"
        start_unit "$WORKER_UNIT" 1 /tmp/cnet-ds4-worker-gpu1.lock \
            "$STATE_DIR/worker.log" "${worker_command[@]}"
        sleep 2
        if [[ $(unit_state "$WORKER_UNIT") != active ]]; then
            echo "DS4 worker failed during startup; see $STATE_DIR/worker.log" >&2
            stop_units
            exit 1
        fi
        start_unit "$COORD_UNIT" 0 /tmp/cnet-ds4-coordinator-gpu0.lock \
            "$STATE_DIR/coordinator.log" "${coordinator_command[@]}"

        deadline=$((SECONDS + READY_TIMEOUT))
        until health_ready; do
            if [[ $(unit_state "$WORKER_UNIT") != active ||
                  $(unit_state "$COORD_UNIT") != active ]]; then
                echo "DS4 dual service exited before readiness; inspect logs action" >&2
                stop_units
                exit 1
            fi
            if (( SECONDS >= deadline )); then
                echo "DS4 readiness timed out after ${READY_TIMEOUT}s; stopping both supervised units" >&2
                stop_units
                exit 1
            fi
            sleep 2
        done

        binary_hash=$(sha256sum "$DS4_SERVER" | cut -d' ' -f1)
        if [[ -f $IDENTITY_FILE ]]; then
            model_identity=$(tr -d '\r\n' < "$IDENTITY_FILE")
        else
            model_identity="unverified:size=$(stat -c %s "$MODEL"):mtime=$(stat -c %Y "$MODEL")"
        fi
        tmp=$READY_FILE.tmp.$$
        printf '{"ready":false,"api_ready":true,"inference_verified":false,"endpoint":"http://%s:%s","model":"%s","model_identity":"%s","binary_sha256":"%s","resource_mask":3,"prefill_window":%s}\n' \
            "$API_HOST" "$API_PORT" "$MODEL" "$model_identity" \
            "$binary_hash" "$PREFILL_WINDOW" > "$tmp"
        mv "$tmp" "$READY_FILE"
        write_status
        ;;

    *)
        echo "usage: $0 {print-plan|start|status|stop|logs|identity|verify}" >&2
        exit 2
        ;;
esac
