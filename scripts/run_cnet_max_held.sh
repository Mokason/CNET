#!/usr/bin/env bash
# Lifecycle for Modular MAX as CNET held_model_v1 residual teacher.
# Does not touch Bonsai baseline on :8081.
set -euo pipefail

ACTION=${1:-status}
SCRIPT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
ENV_FILE=${CNET_MAX_ENV_FILE:-$SCRIPT_ROOT/config/cnet-max-held.env}

if [[ -f $ENV_FILE ]]; then
  # shellcheck disable=SC1090
  set -a && . "$ENV_FILE" && set +a
fi

HOST=${CNET_MAX_HOST:-127.0.0.1}
PORT=${CNET_MAX_PORT:-8000}
MODEL=${CNET_MAX_MODEL:-Qwen/Qwen2.5-7B-Instruct}
DEVICES=${CNET_MAX_DEVICES:-gpu:0}
QUANT=${CNET_MAX_QUANT:-float32}
MAX_LENGTH=${CNET_MAX_MAX_LENGTH:-4096}
MAX_BATCH=${CNET_MAX_MAX_BATCH:-4}
NO_DGC=${CNET_MAX_NO_DEVICE_GRAPH:-1}
STATE_DIR=${CNET_MAX_STATE_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}/cnet-max-held}
PIXI_PROJECT=${CNET_MAX_PIXI_PROJECT:-$HOME/modular-max-nightly}
if [[ ! -d $PIXI_PROJECT/.pixi/envs/default && -d $HOME/modular-max/.pixi/envs/default ]]; then
  PIXI_PROJECT=$HOME/modular-max
fi
MAX_BIN=${CNET_MAX_BIN:-}
LOG_FILE=$STATE_DIR/max-serve.log
PID_FILE=$STATE_DIR/max-serve.pid
READY_FILE=$STATE_DIR/ready.json
ENDPOINT="http://${HOST}:${PORT}/v1/chat/completions"
MODELS_URL="http://${HOST}:${PORT}/v1/models"

mkdir -p "$STATE_DIR"

resolve_max() {
  # Prefer pixi env so MODULAR_HOME / kernel package paths resolve.
  if [[ -d $PIXI_PROJECT/.pixi/envs/default ]]; then
    export MODULAR_HOME=${MODULAR_HOME:-$PIXI_PROJECT/.pixi/envs/default/share/max}
    export CONDA_PREFIX=${CONDA_PREFIX:-$PIXI_PROJECT/.pixi/envs/default}
    export PATH="$PIXI_PROJECT/.pixi/envs/default/bin:$PATH"
  fi
  if [[ -n $MAX_BIN && -x $MAX_BIN ]]; then
    return 0
  fi
  if [[ -x $PIXI_PROJECT/.pixi/envs/default/bin/max ]]; then
    MAX_BIN=$PIXI_PROJECT/.pixi/envs/default/bin/max
    return 0
  fi
  if [[ -x $HOME/.local/bin/max ]]; then
    MAX_BIN=$HOME/.local/bin/max
    return 0
  fi
  if command -v max >/dev/null 2>&1; then
    MAX_BIN=$(command -v max)
    return 0
  fi
  echo "max CLI not found (install Modular max-all / wrappers)" >&2
  return 1
}

pid_alive() {
  local pid=${1:-}
  [[ -n $pid ]] && kill -0 "$pid" 2>/dev/null
}

read_pid() {
  if [[ -f $PID_FILE ]]; then
    tr -d ' \n' <"$PID_FILE" || true
  fi
}

health_ready() {
  curl -fsS --max-time 3 "$MODELS_URL" >/dev/null 2>&1
}

write_ready() {
  local ready=$1 pid=${2:-0} model_id=${3:-}
  printf '{"ready":%s,"pid":%s,"endpoint":"%s","models":"%s","model":"%s","devices":"%s","held_env":"CNET_HELD_MODEL_ENDPOINT","contract":"held_model_v1","replaces_baseline_8081":false}\n' \
    "$ready" "$pid" "$ENDPOINT" "$MODELS_URL" "$model_id" "$DEVICES" >"$READY_FILE"
}

cmd_status() {
  local pid ready=false
  pid=$(read_pid)
  if pid_alive "$pid" && health_ready; then
    ready=true
  elif health_ready; then
    ready=true
    pid=$(ss -lntp 2>/dev/null | rg -o "pid=[0-9]+" -m1 | cut -d= -f2 || echo 0)
  fi
  write_ready "$ready" "${pid:-0}" "$MODEL"
  cat "$READY_FILE"
  if [[ $ready == true ]]; then
    echo "CNET_HELD_MODEL_ENDPOINT=$ENDPOINT"
    return 0
  fi
  return 1
}

cmd_stop() {
  local pid
  pid=$(read_pid)
  if pid_alive "$pid"; then
    kill "$pid" 2>/dev/null || true
    for _ in $(seq 1 30); do
      pid_alive "$pid" || break
      sleep 0.5
    done
    if pid_alive "$pid"; then
      kill -9 "$pid" 2>/dev/null || true
    fi
  fi
  rm -f "$PID_FILE"
  write_ready false 0 "$MODEL"
  echo "stopped"
}

cmd_start() {
  local pid
  if health_ready; then
    echo "already_ready endpoint=$ENDPOINT"
    cmd_status || true
    return 0
  fi
  pid=$(read_pid)
  if pid_alive "$pid"; then
    echo "process_up waiting_ready pid=$pid"
  else
    resolve_max
    : >"$LOG_FILE"
    extra=()
    extra+=(--quantization-encoding "$QUANT")
    extra+=(--max-length "$MAX_LENGTH")
    extra+=(--max-batch-size "$MAX_BATCH")
    if [[ $NO_DGC == 1 || $NO_DGC == true ]]; then
      extra+=(--no-device-graph-capture)
    fi
    # shellcheck disable=SC2086
    nohup env \
      MODULAR_HOME="$MODULAR_HOME" \
      CONDA_PREFIX="${CONDA_PREFIX:-}" \
      PATH="$PATH" \
      HF_HUB_OFFLINE=${HF_HUB_OFFLINE:-0} \
      "$MAX_BIN" serve \
      --model "$MODEL" \
      --port "$PORT" \
      --devices "$DEVICES" \
      --allow-cold-interpreter-cache \
      "${extra[@]}" \
      >>"$LOG_FILE" 2>&1 &
    pid=$!
    echo "$pid" >"$PID_FILE"
    echo "started pid=$pid log=$LOG_FILE modular_home=$MODULAR_HOME quant=$QUANT batch=$MAX_BATCH len=$MAX_LENGTH"
  fi

  local i
  for i in $(seq 1 "${CNET_MAX_READY_TIMEOUT_SEC:-600}"); do
    if ! pid_alive "$(read_pid)"; then
      echo "max_serve_died; tail log:" >&2
      tail -n 40 "$LOG_FILE" >&2 || true
      write_ready false 0 "$MODEL"
      return 1
    fi
    if health_ready; then
      write_ready true "$(read_pid)" "$MODEL"
      echo "ready endpoint=$ENDPOINT"
      echo "export CNET_HELD_MODEL_ENDPOINT=$ENDPOINT"
      return 0
    fi
    sleep 1
  done
  echo "timeout waiting for $MODELS_URL" >&2
  tail -n 40 "$LOG_FILE" >&2 || true
  return 1
}

cmd_smoke() {
  local body
  if ! health_ready; then
    echo "not_ready" >&2
    return 1
  fi
  # Match cnet_held_model.c payload shape (model id may be ignored by single-model serve)
  body=$(curl -fsS --max-time 120 "$ENDPOINT" \
    -H 'Content-Type: application/json' \
    -d "{\"model\":\"${CNET_HELD_MODEL_NAME:-$MODEL}\",\"messages\":[{\"role\":\"system\",\"content\":\"Answer the user request.\"},{\"role\":\"user\",\"content\":\"Reply with exactly: pong\"}],\"temperature\":0,\"max_tokens\":16,\"stream\":false}")
  printf '%s\n' "$body" | head -c 800
  echo
  content=$(printf '%s' "$body" | python3 -c 'import sys,json; d=json.load(sys.stdin); print(d["choices"][0]["message"].get("content") or "")' 2>/dev/null || true)
  if [[ -z $content ]]; then
    echo "CNET_MAX_HELD_SMOKE_FAIL no content" >&2
    return 1
  fi
  if printf '%s' "$content" | rg -qi 'pong'; then
    echo "CNET_MAX_HELD_SMOKE_PASS content_ok"
    return 0
  fi
  # Coherent English without exact pong still counts as quality fail for this smoke.
  if printf '%s' "$content" | rg -q "strugg|'icon|filtro|xamarin|折折"; then
    echo "CNET_MAX_HELD_SMOKE_FAIL quality_garbage content=$(printf '%s' "$content" | head -c 80)" >&2
    return 2
  fi
  echo "CNET_MAX_HELD_SMOKE_PASS http_ok loose_content=$(printf '%s' "$content" | head -c 80)"
  return 0
}

case "$ACTION" in
  start) cmd_start ;;
  stop) cmd_stop ;;
  status) cmd_status ;;
  smoke) cmd_smoke ;;
  restart) cmd_stop; cmd_start ;;
  *)
    echo "usage: $0 {start|stop|restart|status|smoke}" >&2
    exit 2
    ;;
esac
