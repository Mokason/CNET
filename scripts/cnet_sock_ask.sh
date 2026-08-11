#!/usr/bin/env bash
# Query warm cnetd over UNIX socket.
set -euo pipefail
SOCK="${CNET_SOCK:-}"
if [[ -z "$SOCK" ]]; then
  if [[ -n "${XDG_RUNTIME_DIR:-}" && -S "$XDG_RUNTIME_DIR/cnet/cnet.sock" ]]; then
    SOCK="$XDG_RUNTIME_DIR/cnet/cnet.sock"
  else
    SOCK="${HOME}/.local/share/cnet-minimal/run/cnet.sock"
  fi
fi
Q="${*:-}"
if [[ -z "$Q" ]]; then
  echo "usage: cnet-sock-ask <query>" >&2
  exit 2
fi
if [[ ! -S "$SOCK" ]]; then
  echo "cnetd not running (no socket $SOCK). Start: make cnetd-run" >&2
  exit 1
fi
# Prefer python for reliable AF_UNIX
python3 - "$SOCK" "$Q" <<'PY'
import socket, sys
sock, q = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock)
s.sendall(("ASK " + q + "\n").encode())
data = b""
while True:
    chunk = s.recv(65536)
    if not chunk:
        break
    data += chunk
    if b"\nEND\n" in data or data.endswith(b"\n"):
        # text mode ends with END\n; json ends with one line
        if b"\nEND\n" in data or (data.startswith(b"{") and data.endswith(b"\n")):
            break
s.close()
sys.stdout.write(data.decode(errors="replace"))
PY
