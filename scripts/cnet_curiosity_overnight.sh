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
    mkdir -p "$VAR_DIR" "$CNET_DIR/bin/capsules" "$CNET_DIR/docs" ~/.config/systemd/user
    cp -f "$CNET_DIR/scripts/cnet-curiosity-crawler.service" ~/.config/systemd/user/
    systemctl --user daemon-reload
    systemctl --user start cnet-curiosity-crawler.service

    echo "================================================================="
    echo " Started CNET Overnight Autonomous Curiosity Crawler (systemd)"
    echo " Service:        cnet-curiosity-crawler.service (Linger: active)"
    echo " Teacher Model:  http://127.0.0.1:8081 (27B Ternary Bonsai on ROCm)"
    echo " Active Working: $CNET_DIR"
    echo " Log File:       $LOG_FILE"
    echo " Dashboard:      $CNET_DIR/docs/CURIOSITY_NETWORK_STATUS.md"
    echo "================================================================="
    echo "[+] To monitor live: tail -f $LOG_FILE"
    ;;

  stop)
    echo "[*] Stopping CNET Curiosity crawler service..."
    systemctl --user stop cnet-curiosity-crawler.service || true
    echo "[✓] Curiosity crawler service stopped."
    ;;

  status)
    systemctl --user status cnet-curiosity-crawler.service --no-pager || true
    echo ""
    echo "--- Recent Activity (last 15 lines of log) ---"
    tail -n 15 "$LOG_FILE" 2>/dev/null || true
    echo "----------------------------------------------"
    echo "Check live dashboard: docs/CURIOSITY_NETWORK_STATUS.md"
    ;;

  *)
    echo "Usage: $0 {start|stop|status}"
    exit 1
    ;;
esac
