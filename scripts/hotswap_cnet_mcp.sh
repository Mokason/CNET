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
# All exports MUST precede exec. A prior bug left residual/PEFT env after exec
# (dead code) → residual_bound=0 and empty fault/LoRA/acct on live MCP.
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
# Bonsai 1-bit residual via llama-server (must be pre-exec).
export CNET_RESIDUAL_HTTP="\${CNET_RESIDUAL_HTTP:-http://127.0.0.1:8080}"
export CNET_RESIDUAL_WINDOW="\${CNET_RESIDUAL_WINDOW:-$REPO/english_window_256_bonsai_v2.txt}"
export CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE=1
export CNET_PERSONAL_STRUCTURE_MIN_HITS=2
export CNET_PERSONAL_ALLOW_RESIDUAL=1
# Hermetic last-resort; HTTP residual wins when URL set.
export CNET_SOUL_RESIDUAL_HERMETIC="\${CNET_SOUL_RESIDUAL_HERMETIC:-1}"
export CNET_SOUL_RESIDUAL_PREFER_HERMETIC="\${CNET_SOUL_RESIDUAL_PREFER_HERMETIC:-0}"
# PEFT spine: fault bus + durable adapters + acct (shared with personal-ai.env).
export CNET_FAULT_LOG="\${CNET_FAULT_LOG:-$REPO/logs/cnet_faults.jsonl}"
export CNET_FAULT_MIRROR="\${CNET_FAULT_MIRROR:-1}"
export CNET_LORA_STORE_DIR="\${CNET_LORA_STORE_DIR:-$REPO/logs/lora_store}"
export CNET_LORA_STORE_AUTOSAVE="\${CNET_LORA_STORE_AUTOSAVE:-1}"
export CNET_LORA_STORE_AUTOLOAD="\${CNET_LORA_STORE_AUTOLOAD:-1}"
export CNET_ACCT_LOG="\${CNET_ACCT_LOG:-$REPO/logs/cnet_acct.jsonl}"
export CNET_ADAPTER_BANK="\${CNET_ADAPTER_BANK:-1}"
export CNET_LORA_AUTO_ORCH="\${CNET_LORA_AUTO_ORCH:-1}"
export CNET_AUTO_LEARN_FREEFORM="\${CNET_AUTO_LEARN_FREEFORM:-0}"
export CNET_JTC_MIN_CONF="\${CNET_JTC_MIN_CONF:-0.20}"
exec "$DEPLOY/CnetMcpServer" "\$@"
EOS
chmod +x "$DEPLOY/launch.sh"
# Static order gate: residual/PEFT env must appear before exec (never after).
{
  launch="$DEPLOY/launch.sh"
  body=$(grep -v '^[[:space:]]*#' "$launch" || true)
  exec_line=$(printf '%s\n' "$body" | grep -n '^exec \|^exec[[:space:]]' | head -1 | cut -d: -f1)
  [[ -n "$exec_line" ]] || { echo "no exec in launch.sh" >&2; exit 1; }
  pre=$(printf '%s\n' "$body" | sed -n "1,$((exec_line - 1))p")
  post=$(printf '%s\n' "$body" | sed -n "${exec_line},\$p")
  for key in CNET_RESIDUAL_HTTP CNET_SOUL_RESIDUAL_HERMETIC CNET_FAULT_LOG CNET_LORA_STORE_DIR CNET_ACCT_LOG; do
    printf '%s\n' "$pre" | grep -q "$key" || { echo "$key missing or after exec in launch.sh" >&2; exit 1; }
    # key must not appear after the exec token on the exec line / following lines
    printf '%s\n' "$post" | sed '1s/^exec//' | grep -q "$key" && {
      echo "$key appears after exec" >&2; exit 1
    } || true
  done
  echo "LAUNCH_SH_ENV_ORDER_OK"
}

echo "== verify UnitName v2 in deployed CNET.Cce.dll"
{
  dll="$DEPLOY/CNET.Cce.dll"
  # utf-16le "json_toolcall_v2" → j\0s\0o\0n\0_...
  if grep -aob $'j\0s\0o\0n\0_\0t\0o\0o\0l\0c\0a\0l\0l\0_\0v\0\62' "$dll" >/dev/null 2>&1 \
     || grep -aF 'json_toolcall_v2' "$dll" >/dev/null 2>&1; then
    echo "CNET.Cce.dll has json_toolcall_v2"
  else
    echo "v2 missing" >&2
    exit 1
  fi
}

echo "HOTSWAP_FILES_OK"
echo "Next (outside this chat): recycle MCP children or the gateway so the new binary loads."
echo "  Example: systemctl --user kill CnetMcpServer  # if unitized"
echo "  Or your usual gateway recycle command."
