#!/usr/bin/env bash
# Deploy the CNET MCP server into the Hermes gateway, with the runtime
# health tick enabled. This is the recipe used for the 2026-07-11 rollout,
# kept executable so the deployment is reproducible instead of tribal.
#
#   1. dotnet-publish CnetMcpServer (framework-dependent, Release)
#   2. stop hermes-gateway (the running apphost otherwise holds the binary:
#      "Text file busy")
#   3. back up the deploy dir, copy the publish output, refresh cnet.so
#      (and its cnet.dll alias — both ELF, the interop binds "cnet")
#   4. write launch.sh with CNET_BASE_PATH and CNET_HEALTH_TICK_SECONDS
#   5. start the gateway and verify the tick registers
#
# Interval note: the gateway recycles its MCP children (observed lifetimes
# ~60-190 s), so the tick interval must fit inside a child lifetime — 300 s
# never fired in production; 60 s does. Override with TICK_SECONDS=N.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DEPLOY="${DEPLOY_DIR:-$HOME/.hermes/mcp_servers/cnet-mcp}"
BASE="${BASE_PATH:-$REPO/soul_gemma4v2_final.cnb}"
TICK="${TICK_SECONDS:-60}"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

[ -d "$DEPLOY" ] || { echo "deploy dir not found: $DEPLOY" >&2; exit 1; }
[ -f "$BASE" ] || { echo "base not found: $BASE" >&2; exit 1; }

# C: placement doctor — refuse missing base paths; dual-load UNSAFE is hard fail
# unless CNET_DEPLOY_ALLOW_UNSAFE_DUAL=1.
echo "== placement doctor"
if [ ! -x "$REPO/bin/cnet_plan" ]; then
  make -C "$REPO" cnet_plan_cli -j"$(nproc 2>/dev/null || echo 2)" || true
fi
if [ -x "$REPO/bin/cnet_plan" ]; then
  export CNET_BASE_PATH="$BASE"
  # shellcheck disable=SC1091
  if [ -f "$REPO/config/personal-ai.env" ]; then
    set -a
    # shellcheck source=/dev/null
    . "$REPO/config/personal-ai.env" || true
    set +a
  fi
  if ! "$REPO/bin/cnet_plan" doctor; then
    if [ "${CNET_DEPLOY_ALLOW_UNSAFE_DUAL:-0}" = "1" ]; then
      echo "deploy: doctor NEEDS_ATTENTION but CNET_DEPLOY_ALLOW_UNSAFE_DUAL=1 — continuing"
    else
      # Re-check: only hard-fail on missing residual when set, or missing cnb
      json=$("$REPO/bin/cnet_plan" json 2>/dev/null || echo '{}')
      if echo "$json" | grep -q '"dual_safe":0'; then
        echo "deploy: REFUSED dual residual+teacher over MemAvailable budget" >&2
        echo "  set CNET_DEPLOY_ALLOW_UNSAFE_DUAL=1 to override, or unset one model" >&2
        exit 1
      fi
      echo "deploy: doctor flagged issues — continuing if only soft warnings"
    fi
  fi
fi

echo "== publish"
dotnet publish "$REPO/dotnet/CnetMcpServer/CnetMcpServer.csproj" \
  -c Release --nologo -v:q -o "$STAGE/publish"

echo "== verify native exports"
nm -D "$REPO/cnet.so" | grep -q " soul_health_tick$"
nm -D "$REPO/cnet.so" | grep -q " soul_unit_axes$"

echo "== stop gateway + dashboard (both spawn cnet-mcp children)"
systemctl --user stop hermes-gateway.service hermes-dashboard.service
for _ in $(seq 1 20); do
  pgrep -x CnetMcpServer >/dev/null || break
  sleep 1
done
# A CnetMcpServer outside the hermes services (e.g. a manual terminal run)
# may still hold the old binary open; unlink-first replacement below makes
# that harmless — the survivor keeps its old inode, new spawns get the new.

echo "== deploy"
cp -a "$DEPLOY" "$DEPLOY.bak-$(date +%Y%m%d-%H%M%S)"
for f in "$STAGE/publish/"*; do
  rm -f "$DEPLOY/$(basename "$f")"
  cp "$f" "$DEPLOY/"
done
rm -f "$DEPLOY/cnet.so" "$DEPLOY/cnet.dll"
cp "$REPO/cnet.so" "$DEPLOY/cnet.so"
cp "$REPO/cnet.so" "$DEPLOY/cnet.dll"

# Residual Tier C: source from personal-ai.env when present (operator may
# override RESIDUAL= / CNET_RESIDUAL_GGUF= for this deploy).
RESIDUAL_GGUF="${RESIDUAL:-${CNET_RESIDUAL_GGUF:-}}"
RESIDUAL_WIN="${RESIDUAL_WINDOW:-${CNET_RESIDUAL_WINDOW:-$REPO/english_window_256.txt}}"
if [ -z "$RESIDUAL_GGUF" ] && [ -f "$REPO/config/personal-ai.env" ]; then
  RESIDUAL_GGUF=$(grep -E '^CNET_RESIDUAL_GGUF=' "$REPO/config/personal-ai.env" | head -1 | cut -d= -f2- || true)
  RESIDUAL_WIN=$(grep -E '^CNET_RESIDUAL_WINDOW=' "$REPO/config/personal-ai.env" | head -1 | cut -d= -f2- || true)
  RESIDUAL_WIN="${RESIDUAL_WIN:-$REPO/english_window_256.txt}"
fi

cat > "$DEPLOY/launch.sh" <<EOF
#!/bin/bash
export DOTNET_ROOT="\$HOME/dotnet"
export DOTNET_ROOT_X64="\$HOME/dotnet"
export LD_LIBRARY_PATH="$DEPLOY:$REPO:\${LD_LIBRARY_PATH}"
export CNET_MODEL_PATH="$BASE"
export CNET_BASE_PATH="$BASE"
# Serving misses become gap-lane work: soul_route/soul_request append no-plans
# here and the personal-ai learner ingests them.
export CNET_GAP_INBOX="$BASE.inbox"
# Runtime health optimizer: one specialist_health_pass over the live
# certified registry every ${TICK}s (0/absent = off). Keep the interval
# under the gateway's MCP child lifetime or it never fires.
# Also runs structure-mine on residual traces when ripe.
export CNET_HEALTH_TICK_SECONDS=$TICK
export CNET_ORACLE_INT8="\${CNET_ORACLE_INT8:-1}"
# Tier C residual (lazy-loaded on first certified miss when set)
EOF
if [ -n "$RESIDUAL_GGUF" ]; then
  cat >> "$DEPLOY/launch.sh" <<EOF
export CNET_RESIDUAL_GGUF="$RESIDUAL_GGUF"
export CNET_RESIDUAL_WINDOW="$RESIDUAL_WIN"
EOF
  echo "deploy: residual Tier C → $RESIDUAL_GGUF"
else
  echo "deploy: residual Tier C unset (certified-only serve until CNET_RESIDUAL_GGUF)"
fi
cat >> "$DEPLOY/launch.sh" <<EOF
exec "$DEPLOY/CnetMcpServer" "\$@"
EOF
chmod +x "$DEPLOY/launch.sh"

echo "== start gateway"
systemctl --user reset-failed hermes-gateway.service hermes-dashboard.service 2>/dev/null || true
systemctl --user start hermes-gateway.service hermes-dashboard.service
sleep 8
systemctl --user is-active hermes-gateway.service

echo "== verify tick registration"
for _ in $(seq 1 10); do
  if grep -aq "periodic health tick every ${TICK}s" "$HOME/.hermes/logs/mcp-stderr.log"; then
    echo "tick registered (every ${TICK}s); first pass lands within ${TICK}s of a child spawn"
    exit 0
  fi
  sleep 3
done
echo "tick registration not observed yet — check ~/.hermes/logs/mcp-stderr.log" >&2
exit 1
