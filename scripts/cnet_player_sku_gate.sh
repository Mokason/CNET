#!/usr/bin/env bash
# Freeze gate for the thin CNET player SKU. Uses one already-running cnetd;
# never starts or restarts a daemon and never invokes a residual/LLM.
set -uo pipefail

ROOT="${CNET_ROOT:-$HOME/AI/CNET}"
BRICKS="${CNET_CORE_BUS_BRICKS_DIR:-$HOME/.local/share/cnet-bricks}"
ASK="${CNET_PLAYER_ASK:-$ROOT/scripts/cnet_sock_ask.sh}"
SOCK="${CNET_SOCK:-}"
ASK_TIMEOUT_SECONDS="${CNET_PLAYER_ASK_TIMEOUT_SECONDS:-10}"
BANK_LIMIT_BYTES=$((1024 * 1024))
GGUF_REFERENCE_BYTES=6700000000
failures=0
ASK_OUTPUT=""

pass() {
  echo "PASS $1"
}

fail() {
  echo "FAIL $1${2:+: $2}" >&2
  failures=$((failures + 1))
}

field() {
  local key="$1" payload="$2" line value=""
  while IFS= read -r line; do
    case "$line" in
      "$key "*) value="${line#"$key "}" ;;
    esac
  done <<< "$payload"
  printf '%s' "$value"
}

check_eq() {
  local label="$1" expected="$2" actual="$3"
  if [[ "$actual" == "$expected" ]]; then
    pass "$label"
  else
    fail "$label" "expected=$(printf '%q' "$expected") actual=$(printf '%q' "$actual")"
  fi
}

ask_existing() {
  local query="$1"
  ASK_OUTPUT=""
  if ! ASK_OUTPUT="$(CNET_SOCK="$SOCK" timeout "${ASK_TIMEOUT_SECONDS}s" "$ASK" "$query" 2>&1)"; then
    fail "ask" "query=$(printf '%q' "$query") output=$(printf '%q' "$ASK_OUTPUT")"
    return 1
  fi
}

if [[ -z "$SOCK" ]]; then
  if [[ -n "${XDG_RUNTIME_DIR:-}" && -S "$XDG_RUNTIME_DIR/cnet/cnet.sock" ]]; then
    SOCK="$XDG_RUNTIME_DIR/cnet/cnet.sock"
  else
    SOCK="$HOME/.local/share/cnet-minimal/run/cnet.sock"
  fi
fi

if [[ ! "$ASK_TIMEOUT_SECONDS" =~ ^([1-9]|[1-5][0-9]|60)$ ]]; then
  fail "ask_timeout" "expected integer 1..60, got $(printf '%q' "$ASK_TIMEOUT_SECONDS")"
elif [[ ! -S "$SOCK" ]]; then
  fail "existing_cnetd_socket" "$SOCK"
elif [[ ! -x "$ASK" ]]; then
  fail "socket_client" "$ASK"
else
  pass "existing_cnetd_socket=$SOCK"

  if ask_existing '10+11'; then
    add_out="$ASK_OUTPUT"
    check_eq "10+11 source" "LOCAL" "$(field SOURCE "$add_out")"
    check_eq "10+11 CERT" "1" "$(field CLAIMED_CERT "$add_out")"
    check_eq "10+11 answer" "21" "$(field ANSWER "$add_out")"
    check_eq "10+11 no residual" "0" "$(field STAGE_DRAFT "$add_out")"
  fi

  if ask_existing 'q1_27b13 3'; then
    brick_out="$ASK_OUTPUT"
    check_eq "q1_27b13 source" "LOCAL" "$(field SOURCE "$brick_out")"
    check_eq "q1_27b13 CERT" "1" "$(field CLAIMED_CERT "$brick_out")"
    check_eq "q1_27b13 answer" "3" "$(field ANSWER "$brick_out")"
    check_eq "q1_27b13 no residual" "0" "$(field STAGE_DRAFT "$brick_out")"
  fi

  if ask_existing 'Do you know json?'; then
    json_out="$ASK_OUTPUT"
    check_eq "json CLAIMED_CERT" "0" "$(field CLAIMED_CERT "$json_out")"
    check_eq "json answer" "Not sealed." "$(field ANSWER "$json_out")"
    check_eq "json no residual" "0" "$(field STAGE_DRAFT "$json_out")"
    check_eq "json no action" "-" "$(field ACTION "$json_out")"
    if [[ "$json_out" == *$'\nTEACH '* || "$json_out" == *$'\nTAG '* ]]; then
      fail "json no teach tag"
    else
      pass "json no teach tag"
    fi
  fi
fi

# Latency + no-network: leftover/soul must not wait on residual/MCP.
python3 - "$SOCK" <<'PY'
import socket, sys, time
sock = sys.argv[1]

def ask(q, timeout=2.0):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    t0 = time.perf_counter()
    s.connect(sock)
    s.sendall(("ASK " + q + "\n").encode())
    data = b""
    while True:
        c = s.recv(65536)
        if not c:
            break
        data += c
        if b"\nEND\n" in data:
            break
    s.close()
    ms = (time.perf_counter() - t0) * 1000.0
    rec = {"ms": ms, "raw": data.decode(errors="replace")}
    return rec

def field(raw, key):
    for line in raw.splitlines():
        if line.startswith(key + " "):
            return line[len(key) + 1 :]
    return ""

fails = 0
for q, max_ms, cert, forbid in [
    ("Do you know json?", 5.0, "0", ("wttr", "JSON is", "I'll speak")),
    ("who are you", 5.0, "1", ()),
    ("tell marble to fetch the weather", 5.0, "0", ("wttr", "Sunny", "Live weather")),
]:
    try:
        r = ask(q)
    except Exception as e:
        print(f"FAIL sku_latency {q!r}: {type(e).__name__}")
        fails += 1
        continue
    ans = field(r["raw"], "ANSWER")
    c = field(r["raw"], "CLAIMED_CERT")
    ok = r["ms"] <= max_ms and c == cert and not any(x.lower() in ans.lower() for x in forbid)
    if ok:
        print(f"PASS sku_latency {q!r} ms={r['ms']:.2f} cert={c}")
    else:
        print(f"FAIL sku_latency {q!r} ms={r['ms']:.2f} cert={c} ans={ans[:80]!r}")
        fails += 1
sys.exit(1 if fails else 0)
PY
lat_rc=$?
if [[ "$lat_rc" -ne 0 ]]; then
  fail "sku_latency"
fi

if [[ ! -d "$BRICKS" ]]; then
  fail "brick_bank" "$BRICKS"
else
  bank_du="$(du -b -- "$BRICKS" 2>&1)"
  du_rc=$?
  bank_bytes="$(printf '%s\n' "$bank_du" | sed -n '$s/[[:space:]].*//p')"
  if [[ "$du_rc" -ne 0 || ! "$bank_bytes" =~ ^[0-9]+$ ]]; then
    fail "brick_bank_size" "$bank_du"
  elif (( bank_bytes >= BANK_LIMIT_BYTES )); then
    fail "brick_bank_size" "bytes=$bank_bytes limit=$BANK_LIMIT_BYTES"
  else
    pass "brick_bank_size bytes=$bank_bytes limit=$BANK_LIMIT_BYTES gguf_reference=$GGUF_REFERENCE_BYTES"
  fi
fi

if (( failures == 0 )); then
  echo "PLAYER_SKU_PASS"
  exit 0
fi
echo "PLAYER_SKU_FAIL failures=$failures" >&2
exit 1
