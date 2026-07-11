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

echo "== publish"
dotnet publish "$REPO/dotnet/CnetMcpServer/CnetMcpServer.csproj" \
  -c Release --nologo -v:q -o "$STAGE/publish"

echo "== verify native exports"
nm -D "$REPO/cnet.so" | grep -q " soul_health_tick$"
nm -D "$REPO/cnet.so" | grep -q " soul_unit_axes$"

echo "== stop gateway"
systemctl --user stop hermes-gateway.service
for _ in $(seq 1 20); do
  pgrep -x CnetMcpServer >/dev/null || break
  sleep 1
done
pgrep -x CnetMcpServer >/dev/null && {
  echo "CnetMcpServer still running; not deploying over a live binary" >&2
  systemctl --user start hermes-gateway.service
  exit 1
}

echo "== deploy"
cp -a "$DEPLOY" "$DEPLOY.bak-$(date +%Y%m%d-%H%M%S)"
cp "$STAGE/publish/"* "$DEPLOY/"
cp "$REPO/cnet.so" "$DEPLOY/cnet.so"
cp "$REPO/cnet.so" "$DEPLOY/cnet.dll"

cat > "$DEPLOY/launch.sh" <<EOF
#!/bin/bash
export DOTNET_ROOT="\$HOME/dotnet"
export DOTNET_ROOT_X64="\$HOME/dotnet"
export LD_LIBRARY_PATH="$DEPLOY:$REPO:\${LD_LIBRARY_PATH}"
export CNET_MODEL_PATH="$BASE"
export CNET_BASE_PATH="$BASE"
# Serving misses become gap-lane work: soul_route appends no-plans here
# and the cnet-gap-lane service ingests them.
export CNET_GAP_INBOX="$BASE.inbox"
# Runtime health optimizer: one specialist_health_pass over the live
# certified registry every ${TICK}s (0/absent = off). Keep the interval
# under the gateway's MCP child lifetime or it never fires.
export CNET_HEALTH_TICK_SECONDS=$TICK
exec "$DEPLOY/CnetMcpServer" "\$@"
EOF
chmod +x "$DEPLOY/launch.sh"

echo "== start gateway"
systemctl --user reset-failed hermes-gateway.service 2>/dev/null || true
systemctl --user start hermes-gateway.service
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
