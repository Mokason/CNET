#!/usr/bin/env bash
# Off-hours Marble curiosity watchdog — silent when healthy (exit 0, empty stdout).
# Alert only when something is wrong (for hermes cron --no-agent deliver).
set -euo pipefail
ROOT="${CNET_ROOT:-/home/marble/AI/CNET}"
cd "$ROOT"
issues=()

for u in bonsai-server cnet-personal-ai-lane; do
  systemctl --user is-active --quiet "$u" 2>/dev/null || issues+=("service_down:$u")
done
for t in cnet-governor.timer cnet-autoteach.timer cnet-personal-ai-ops.timer; do
  systemctl --user is-active --quiet "$t" 2>/dev/null || issues+=("timer_down:$t")
done

if ! curl -sf -m 4 http://127.0.0.1:8080/v1/models >/dev/null 2>&1; then
  issues+=("bonsai_http_dead:8080")
fi

envf="$ROOT/config/personal-ai.env"
if [[ -f "$envf" ]]; then
  grep -qE '^CNET_CURIOSITY=1' "$envf" || issues+=("curiosity_off_in_env")
  grep -qE '^CNET_CURIOSITY_YIELD_OPEN=1$' "$envf" && issues+=("curiosity_yield_starved")
  grep -qE '^CNET_COVERAGE_ABSTAIN=0' "$envf" && issues+=("coverage_gate_forced_off")
  grep -qE '^CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE=1' "$envf" || issues+=("structure_mine_on_serve_off")
fi

# Last oneshot results (if failed)
for u in cnet-governor.service cnet-autoteach.service; do
  res=$(systemctl --user show "$u" -p Result --value 2>/dev/null || echo unknown)
  # After success Result=success; failure leaves failed
  if [[ "$res" == "failed" || "$res" == "timeout" || "$res" == "signal" ]]; then
    issues+=("last_${u}_result:$res")
  fi
done

if ((${#issues[@]})); then
  echo "MARBLE_OFFHOURS_ALERT ts=$(date -Iseconds)"
  printf '  - %s\n' "${issues[@]}"
  systemctl --user is-active bonsai-server cnet-personal-ai-lane 2>&1 | sed 's/^/  svc /' || true
  exit 1
fi
exit 0
