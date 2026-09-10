#!/usr/bin/env bash
# scripts/cnet_curiosity_overnight.sh — Run CNET Curiosity Crawler in the background
set -euo pipefail

CNET_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VAR_DIR="$CNET_DIR/var/curiosity_crawler"
PID_FILE="$VAR_DIR/crawler.pid"
LOG_FILE="$VAR_DIR/crawler.log"
CMD="${1:-start}"

mkdir -p "$VAR_DIR" "$CNET_DIR/bin/capsules" "$CNET_DIR/docs"

case "$CMD" in
  start)
    if [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
      echo "[!] Curiosity crawler is already running with PID $(cat "$PID_FILE")."
      echo "    Log file: $LOG_FILE"
      echo "    Status:   cat docs/CURIOSITY_NETWORK_STATUS.md"
      exit 0
    fi

    echo "================================================================="
    echo " Starting CNET Overnight Autonomous Curiosity Crawler..."
    echo " Teacher Model:  http://127.0.0.1:8081 (27B Ternary Bonsai on ROCm)"
    echo " Active Working: $CNET_DIR"
    echo " Log File:       $LOG_FILE"
    echo " Dashboard:      $CNET_DIR/docs/CURIOSITY_NETWORK_STATUS.md"
    echo "================================================================="

    cd "$CNET_DIR"
    export PYTHONUNBUFFERED=1
    nohup python3 tools/cnet_curiosity_crawler.py >> "$LOG_FILE" 2>&1 &
    CRAWLER_PID=$!
    echo "$CRAWLER_PID" > "$PID_FILE"
    echo "[+] Curiosity crawler started successfully with PID $CRAWLER_PID."
    echo "[+] To monitor live: tail -f $LOG_FILE"
    ;;

  stop)
    if [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
      PID=$(cat "$PID_FILE")
      echo "[*] Sending SIGTERM to Curiosity crawler PID $PID for graceful shutdown..."
      kill -TERM "$PID"
      for i in {1..15}; do
        if ! kill -0 "$PID" 2>/dev/null; then
          echo "[✓] Curiosity crawler stopped cleanly."
          rm -f "$PID_FILE"
          exit 0
        fi
        sleep 1
      done
      echo "[!] Process did not terminate within 15s. Sending SIGKILL..."
      kill -9 "$PID" 2>/dev/null || true
      rm -f "$PID_FILE"
    else
      echo "[-] Curiosity crawler is not running."
      rm -f "$PID_FILE" 2>/dev/null || true
    fi
    ;;

  status)
    if [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
      echo "[✓] Curiosity crawler is RUNNING (PID $(cat "$PID_FILE"))."
      echo "--- Recent Activity (last 15 lines of log) ---"
      tail -n 15 "$LOG_FILE" 2>/dev/null || true
      echo "----------------------------------------------"
      echo "Check live dashboard: docs/CURIOSITY_NETWORK_STATUS.md"
    else
      echo "[-] Curiosity crawler is STOPPED."
      if [[ -f "$CNET_DIR/docs/CURIOSITY_NETWORK_STATUS.md" ]]; then
        echo "Last saved dashboard available at docs/CURIOSITY_NETWORK_STATUS.md"
      fi
    fi
    ;;

  *)
    echo "Usage: $0 {start|stop|status}"
    exit 1
    ;;
esac
