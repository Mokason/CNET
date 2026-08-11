#!/usr/bin/env bash
# CNET + Marble 24/7 outside Hermes.
#
# Hermes is an optional client. This stack keeps learning / ROE evolve /
# personal-ai lane / bonsai teacher alive across Hermes upgrades.
#
# Usage:
#   scripts/cnet_marble_24_7.sh install   # copy units, linger, daemon-reload
#   scripts/cnet_marble_24_7.sh start     # enable --now cnet-marble.target + timers
#   scripts/cnet_marble_24_7.sh stop      # stop target (does NOT stop Hermes)
#   scripts/cnet_marble_24_7.sh status
#   scripts/cnet_marble_24_7.sh doctor
#   scripts/cnet_marble_24_7.sh health    # one health snap now
#
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
UNIT_SRC="$REPO/scripts/systemd"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
TARGET="cnet-marble.target"

# Units owned/managed by this stack (Hermes deliberately excluded)
COPY_UNITS=(
  cnet-marble.target
  cnet-marble-health.service
  cnet-marble-health.timer
  roe-evolve-tick.service
  roe-evolve-tick.timer
  cnet-autonomous-cycle.service
  cnet-autonomous-cycle.timer
)

# Existing user units we enable into the target (already installed elsewhere)
ENABLE_TIMERS=(
  cnet-autoteach.timer
  cnet-governor.timer
  cnet-janitor.timer
  cnet-personal-ai-ops.timer
  roe-evolve-tick.timer
  cnet-marble-health.timer
  cnet-autonomous-cycle.timer
)
ENABLE_SERVICES=(
  cnet-personal-ai-lane.service
  bonsai-server.service
)

info() { echo "cnet_marble_24_7: $*"; }
warn() { echo "cnet_marble_24_7: WARN: $*" >&2; }
die() { echo "cnet_marble_24_7: $*" >&2; exit 1; }

ensure_linger() {
  if loginctl show-user "$USER" -p Linger 2>/dev/null | grep -q 'Linger=yes'; then
    info "linger already yes (survives logout)"
  else
    info "enabling linger for $USER (needs root once if not set)"
    if loginctl enable-linger "$USER" 2>/dev/null; then
      info "linger enabled"
    else
      warn "could not enable linger — run: sudo loginctl enable-linger $USER"
    fi
  fi
}

install_units() {
  mkdir -p "$UNIT_DIR"
  for u in "${COPY_UNITS[@]}"; do
    if [[ -f "$UNIT_SRC/$u" ]]; then
      cp -f "$UNIT_SRC/$u" "$UNIT_DIR/$u"
      info "installed $u"
    else
      warn "missing $UNIT_SRC/$u"
    fi
  done
  # Drop-in: make key units also WantedBy cnet-marble.target without rewriting them
  for u in cnet-autoteach.timer cnet-governor.timer cnet-janitor.timer \
           cnet-personal-ai-ops.timer cnet-personal-ai-lane.service bonsai-server.service; do
    d="$UNIT_DIR/${u}.d"
    mkdir -p "$d"
    cat >"$d/cnet-marble.conf" <<EOF
[Install]
WantedBy=cnet-marble.target
EOF
    info "drop-in $u → cnet-marble.target"
  done
  systemctl --user daemon-reload
  ensure_linger
  info "INSTALL_OK"
}

start_stack() {
  install_units
  # never touch hermes units
  for t in "${ENABLE_TIMERS[@]}"; do
    if systemctl --user cat "$t" &>/dev/null; then
      systemctl --user enable --now "$t" && info "enabled $t" || warn "failed $t"
    else
      warn "unit missing: $t"
    fi
  done
  for s in "${ENABLE_SERVICES[@]}"; do
    if systemctl --user cat "$s" &>/dev/null; then
      systemctl --user enable --now "$s" && info "enabled $s" || warn "failed $s"
    else
      warn "unit missing: $s"
    fi
  done
  systemctl --user enable --now "$TARGET" && info "enabled $TARGET"
  # optional marble helpers
  for s in marble-heartbeat.service marble-embeddings.service; do
    if systemctl --user cat "$s" &>/dev/null; then
      systemctl --user enable --now "$s" 2>/dev/null && info "enabled optional $s" || true
    fi
  done
  python3 "$REPO/scripts/cnet_marble_health_snap.py" || true
  info "START_OK — Hermes not required"
}

stop_stack() {
  systemctl --user stop "$TARGET" 2>/dev/null || true
  for t in roe-evolve-tick.timer cnet-marble-health.timer; do
    systemctl --user stop "$t" 2>/dev/null || true
  done
  info "stopped $TARGET (+ local roe/health timers). Hermes left alone."
  info "STOP_OK"
}

status_stack() {
  echo "=== CNET+Marble 24/7 (Hermes-independent) ==="
  echo "repo=$REPO"
  loginctl show-user "$USER" -p Linger 2>/dev/null || true
  echo
  printf '%-40s %-10s %-12s\n' UNIT ACTIVE ENABLED
  for u in "$TARGET" "${ENABLE_SERVICES[@]}" "${ENABLE_TIMERS[@]}" \
           marble-heartbeat.service marble-embeddings.service \
           hermes-gateway.service; do
    a=$(systemctl --user is-active "$u" 2>/dev/null || echo missing)
    e=$(systemctl --user is-enabled "$u" 2>/dev/null || echo missing)
    printf '%-40s %-10s %-12s\n' "$u" "$a" "$e"
  done
  echo
  if [[ -f logs/marble_24_7/status.json ]]; then
    echo "---- last health ----"
    python3 - <<'PY'
import json
from pathlib import Path
p=Path("logs/marble_24_7/status.json")
d=json.loads(p.read_text())
print("ts", d.get("ts"))
print("core_active", d.get("core_active_count"), d.get("core_names"))
print("hermes_gateway", d.get("hermes_gateway"))
print("ports", d.get("ports"))
print("paths", d.get("paths"))
print("hermes_required", d.get("hermes_required"))
PY
  fi
  echo
  systemctl --user list-timers --all 2>/dev/null | rg -i 'cnet|roe-evolve|marble-health|governor|janitor|autoteach' || true
}

doctor() {
  status_stack
  echo
  echo "=== doctor ==="
  # Hermes must not be a hard dependency of target
  if systemctl --user cat "$TARGET" 2>/dev/null | rg -q '^(Wants|Requires|BindsTo)=.*hermes'; then
    warn "TARGET hard-depends on hermes — remove it"
  else
    info "target has no hermes hard-dependency OK"
  fi
  # WorkingDirectory must be CNET repo
  for u in roe-evolve-tick.service cnet-autoteach.service; do
    if systemctl --user cat "$u" 2>/dev/null | rg -q 'AI/CNET'; then
      info "$u rooted under AI/CNET OK"
    else
      warn "$u may not be CNET-rooted"
    fi
  done
  # ollama for cloud teacher/reviewer
  if curl -sf --max-time 2 http://127.0.0.1:11434/api/tags >/dev/null; then
    info "ollama :11434 up (teacher/reviewer transport)"
  else
    warn "ollama not reachable — reviewer/teacher cloud offline until ollama up"
  fi
  if curl -sf --max-time 2 http://127.0.0.1:8080/health >/dev/null 2>&1 || \
     curl -sf --max-time 2 http://127.0.0.1:8080/ >/dev/null 2>&1; then
    info "bonsai residual :8080 reachable"
  else
    warn "bonsai :8080 not answering (autoteach/governor may idle)"
  fi
  [[ -d artifacts/roe_daily_packs/pack_soul_marble ]] && info "pack_soul_marble present" || warn "seed soul pack: make roe_soul_pack"
  [[ -d artifacts/roe_daily_packs/pack_personal ]] && info "pack_personal present" || info "pack_personal created on first evolve tick"
  info "DOCTOR_DONE"
}

cmd="${1:-}"
case "$cmd" in
  install) install_units ;;
  start) start_stack ;;
  stop) stop_stack ;;
  status) status_stack ;;
  doctor) doctor ;;
  health) python3 "$REPO/scripts/cnet_marble_health_snap.py" ;;
  *)
    cat <<EOF
Usage: $0 {install|start|stop|status|doctor|health}

Hermes-independent 24/7 for CNET + Marble.
Does not start/stop hermes-gateway.
EOF
    exit 2
    ;;
esac
