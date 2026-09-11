#!/usr/bin/env bash
# scripts/cnet_isekai_crawler.sh — Control CNET Isekai & RPG Knowledge Crawler
set -euo pipefail

CNET_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VAR_DIR="$CNET_DIR/var/isekai_rpg_crawler"
LOG_FILE="$VAR_DIR/crawler.log"
CMD="${1:-start}"

mkdir -p "$VAR_DIR" "$CNET_DIR/bin/capsules" "$CNET_DIR/docs"

case "$CMD" in
  start)
    mkdir -p "$VAR_DIR" "$CNET_DIR/bin/capsules" "$CNET_DIR/docs" ~/.config/systemd/user
    cp -f "$CNET_DIR/scripts/cnet-isekai-crawler.service" ~/.config/systemd/user/
    systemctl --user daemon-reload
    systemctl --user start cnet-isekai-crawler.service

    echo "================================================================="
    echo " Started CNET Autonomous Isekai & RPG Knowledge Crawler (systemd)"
    echo " Service:        cnet-isekai-crawler.service (Cores 0-7)"
    echo " Teacher Model:  http://127.0.0.1:8081 (27B Ternary Bonsai on ROCm)"
    echo " Active Working: $CNET_DIR"
    echo " Log File:       $LOG_FILE"
    echo " Live Dashboard: $CNET_DIR/docs/ISEKAI_RPG_NETWORK_STATUS.md"
    echo "================================================================="
    echo "[+] To monitor live: tail -f $LOG_FILE"
    ;;

  stop)
    echo "[*] Stopping CNET Isekai crawler service..."
    systemctl --user stop cnet-isekai-crawler.service || true
    echo "[✓] Isekai crawler service stopped."
    ;;

  status)
    systemctl --user status cnet-isekai-crawler.service --no-pager || true
    echo ""
    echo "--- Recent Activity (last 15 lines of log) ---"
    tail -n 15 "$LOG_FILE" 2>/dev/null || true
    echo "----------------------------------------------"
    echo "Check live dashboard: docs/ISEKAI_RPG_NETWORK_STATUS.md"
    ;;

  *)
    echo "Usage: $0 {start|stop|status}"
    exit 1
    ;;
esac
