#!/usr/bin/env bash
# Hot-swap CnetMcpServer binaries into ~/.hermes/mcp_servers/cnet-mcp.
# Run this from a shell OUTSIDE Discord/gateway so children can be recycled.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
DEPLOY="${DEPLOY_DIR:-$HOME/.hermes/mcp_servers/cnet-mcp}"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
export PATH="${HOME}/dotnet:${PATH}"

echo "== publish"
dotnet publish "$REPO/dotnet/CnetMcpServer/CnetMcpServer.csproj" \
  -c Release --nologo -v:q -o "$STAGE/publish"

echo "== native cnet.so"
make -C "$REPO" --no-print-directory cnet_dll -j"$(nproc 2>/dev/null || echo 2)"

echo "== deploy files (unlink-first)"
cp -a "$DEPLOY" "$DEPLOY.bak-$(date +%Y%m%d-%H%M%S)"
for f in "$STAGE/publish/"*; do
  rm -f "$DEPLOY/$(basename "$f")"
  cp "$f" "$DEPLOY/"
done
rm -f "$DEPLOY/cnet.so" "$DEPLOY/cnet.dll"
cp "$REPO/cnet.so" "$DEPLOY/cnet.so"
cp "$REPO/cnet.so" "$DEPLOY/cnet.dll"

BASE="${BASE_PATH:-$REPO/soul_gemma4v2_final.cnb}"
cat > "$DEPLOY/launch.sh" <<EOS
#!/bin/bash
export DOTNET_ROOT="\$HOME/dotnet"
export DOTNET_ROOT_X64="\$HOME/dotnet"
export LD_LIBRARY_PATH="$DEPLOY:$REPO:\${LD_LIBRARY_PATH}"
export CNET_MODEL_PATH="$BASE"
export CNET_BASE_PATH="$BASE"
export CNET_GAP_INBOX="$BASE.inbox"
export CNET_HEALTH_TICK_SECONDS=60
export CNET_ORACLE_INT8="\${CNET_ORACLE_INT8:-1}"
export CNET_RESIDUAL_GGUF="/home/marble/AI/Models/gemma4-v2-Q4_K_M.gguf"
export CNET_RESIDUAL_WINDOW="$REPO/english_window_256.txt"
export CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE=1
export CNET_PERSONAL_STRUCTURE_MIN_HITS=2
export CNET_PERSONAL_ALLOW_RESIDUAL=1
exec "$DEPLOY/CnetMcpServer" "\$@"
EOS
chmod +x "$DEPLOY/launch.sh"

echo "== verify UnitName v2 in deployed CNET.Cce.dll"
python3 - <<'PY'
from pathlib import Path
import os
p = Path(os.path.expanduser("~/.hermes/mcp_servers/cnet-mcp/CNET.Cce.dll"))
b = p.read_bytes()
assert b.find("json_toolcall_v2".encode("utf-16le")) >= 0, "v2 missing"
print("CNET.Cce.dll has json_toolcall_v2")
PY

echo "HOTSWAP_FILES_OK"
echo "Next (outside this chat): recycle MCP children or the gateway so the new binary loads."
echo "  Example: systemctl --user kill CnetMcpServer  # if unitized"
echo "  Or your usual gateway recycle command."
