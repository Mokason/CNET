#!/usr/bin/bash
# Resolve Tailscale/localhost bind and exec cnet-web.
# Prefer tailscale0 100.x; fallback 127.0.0.1. Never 0.0.0.0 unless forced.
set -euo pipefail
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:${PATH:-}"

ROOT="$(cd "$(/usr/bin/dirname "$0")/.." && pwd)"
cd "$ROOT"

ENVF="${HOME}/.local/share/cnet-minimal/cnet-minimal.env"
if [[ -f "$ENVF" ]]; then
  set -a
  # shellcheck disable=SC1090
  . "$ENVF"
  set +a
fi
# re-assert PATH after env (in case file was broken)
export PATH="/home/marble/.local/share/cnet-minimal/current/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

export CNET_MINIMAL_ROOT="${CNET_MINIMAL_ROOT:-$HOME/.local/share/cnet-minimal/current}"
export CNET_PACKS_ROOT="${CNET_PACKS_ROOT:-$CNET_MINIMAL_ROOT/data/roe_daily_packs}"
export CNET_WEB_PORT="${CNET_WEB_PORT:-8642}"

if [[ -z "${CNET_WEB_HOST:-}" ]]; then
  host=""
  if [[ -x /usr/bin/tailscale ]]; then
    host="$(/usr/bin/tailscale ip -4 2>/dev/null | /usr/bin/awk '/^100\./{print; exit}')"
  fi
  if [[ -z "$host" ]]; then
    host="$(/usr/bin/ip -4 -o addr show dev tailscale0 2>/dev/null | /usr/bin/awk '{print $4}' | /usr/bin/cut -d/ -f1 | /usr/bin/awk '/^100\./{print; exit}')"
  fi
  if [[ -z "$host" ]]; then
    host="127.0.0.1"
  fi
  export CNET_WEB_HOST="$host"
fi

if [[ "${CNET_WEB_HOST}" == "0.0.0.0" || "${CNET_WEB_HOST}" == "::" ]]; then
  if [[ "${CNET_WEB_ALLOW_PUBLIC:-}" != "1" ]]; then
    echo "cnet-web-run: refusing public bind ${CNET_WEB_HOST}" >&2
    export CNET_WEB_HOST="127.0.0.1"
  fi
fi

sock="${CNET_SOCK:-}"
if [[ -z "$sock" ]]; then
  if [[ -n "${XDG_RUNTIME_DIR:-}" && -S "${XDG_RUNTIME_DIR}/cnet/cnet.sock" ]]; then
    sock="${XDG_RUNTIME_DIR}/cnet/cnet.sock"
  else
    sock="${HOME}/.local/share/cnet-minimal/run/cnet.sock"
  fi
fi
for _ in 1 2 3 4 5 6 7 8 9 10; do
  if [[ -S "$sock" ]]; then
    break
  fi
  /usr/bin/sleep 0.3
done
if [[ ! -S "$sock" ]]; then
  echo "cnet-web-run: warning: cnetd socket not ready at $sock (will still start)" >&2
fi

echo "cnet-web-run: bind=${CNET_WEB_HOST}:${CNET_WEB_PORT} sock=$sock" >&2
exec /usr/bin/python3 "$ROOT/tools/cnet_web.py"
