#!/usr/bin/env bash
# N1: recycle CNET user services + smoke traffic + dashboard snapshot.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
ENVF="${CNET_PERSONAL_AI_ENV:-$ROOT/config/personal-ai.env}"
OUT="${1:-$ROOT/artifacts/janitor/LIVE_SMOKE.md}"
mkdir -p "$(dirname "$OUT")" logs artifacts/janitor

{
  echo "# CNET live smoke (N1)"
  echo "- when: $(date -Iseconds)"
  echo

  if [[ -f "$ENVF" ]]; then
    set -a
    # shellcheck disable=SC1090
    source "$ENVF"
    set +a
    echo "- env: \`$ENVF\` loaded"
  else
    echo "- WARN: no $ENVF"
  fi

  echo
  echo "## systemctl --user (pre)"
  systemctl --user list-units 'cnet*' --all --no-pager 2>/dev/null || true

  echo
  echo "## recycle"
  for u in cnet-personal-ai-lane.service cnet-ghost-orchestrator.service; do
    if systemctl --user cat "$u" &>/dev/null; then
      systemctl --user restart "$u" && echo "- restarted $u" || echo "- FAIL restart $u"
    else
      echo "- skip missing $u"
    fi
  done
  # ops oneshot if present
  if systemctl --user cat cnet-personal-ai-ops.service &>/dev/null; then
    systemctl --user start cnet-personal-ai-ops.service && echo "- started ops oneshot" || true
  fi
  sleep 2

  echo
  echo "## systemctl --user (post)"
  systemctl --user is-active cnet-personal-ai-lane.service 2>/dev/null | awk '{print "- lane:", $0}'
  systemctl --user is-active cnet-ghost-orchestrator.service 2>/dev/null | awk '{print "- ghost:", $0}'

  echo
  echo "## hermetic traffic (substitutes 24h load proof)"
  if [[ -x bin/cnet_a_grade ]]; then
    ./bin/cnet_a_grade | tail -5
  elif [[ -x bin/cnet_grade_up ]]; then
    ./bin/cnet_grade_up | tail -5
  else
    echo "- building cnet_a_grade..."
    make -s cnet_a_grade 2>&1 | tail -8
  fi

  echo
  echo "## dashboard"
  bash scripts/cnet_acct_dashboard.sh "$ROOT/artifacts/janitor/ACCT_DASHBOARD.md" | tail -20
  bash scripts/cnet_openlab_doctor.sh | tail -15

  echo
  echo "CNET_LIVE_SMOKE_PASS"
} | tee "$OUT"
