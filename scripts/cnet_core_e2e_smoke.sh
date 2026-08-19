#!/usr/bin/env bash
# CORE middle-ground E2E smoke against live cnetd + default Bonsai held.
# Cases: LOGIC→CERT, CREATIVE→OPEN_CHAT, LOGIC miss→abstain
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

SOCK="${CNET_SOCK:-/tmp/cnetd-core-e2e-$$.sock}"
LOG="${CNET_CORE_E2E_LOG:-$ROOT/logs/cnet_core_e2e.log}"
MISS="${CNET_MISS_LOG:-$ROOT/logs/cnet_core_e2e_miss.jsonl}"
ENVF="${CNET_HELD_ENV:-$ROOT/config/cnet-bonsai-held.env}"
PACKS="${CNET_PACKS_ROOT:-$ROOT/artifacts/roe_daily_packs}"
BIN="${CNETD_BIN:-$ROOT/bin/cnetd}"
export CNET_SOCK="$SOCK"
export CNET_PACKS_ROOT="$PACKS"
export CNET_MISS_LOG="$MISS"
export CNET_BRAIN_MIRROR_DIR="${CNET_BRAIN_MIRROR_DIR:-/tmp/cnet_core_e2e_mirror_$$}"

mkdir -p "$(dirname "$LOG")" "$CNET_BRAIN_MIRROR_DIR" logs
: >"$LOG"
: >"$MISS"

if [[ ! -x $BIN ]]; then
  echo "building cnetd..." | tee -a "$LOG"
  make cnetd
fi

if [[ -f $ENVF ]]; then
  # shellcheck disable=SC1090
  set -a && . "$ENVF" && set +a
fi

# Preflight Bonsai held
if ! curl -fsS --max-time 5 "${CNET_HELD_MODEL_ENDPOINT%/v1/chat/completions}/v1/models" >/dev/null 2>&1; then
  # try base without strip
  if ! curl -fsS --max-time 5 "http://127.0.0.1:8081/v1/models" >/dev/null; then
    echo "FAIL: Bonsai held not reachable on :8081" | tee -a "$LOG"
    exit 2
  fi
fi

# Start cnetd
rm -f "$SOCK"
"$BIN" >>"$LOG" 2>&1 &
PID=$!
cleanup() {
  # Bounded teardown: cnetd honours SIGTERM (tests/test_cnetd_sigterm.sh), but
  # never let this gate hang on `wait` if that ever regresses again — escalate
  # to SIGKILL rather than blocking for the whole CI timeout.
  kill "$PID" 2>/dev/null || true
  for _ in $(seq 1 50); do
    kill -0 "$PID" 2>/dev/null || break
    sleep 0.1
  done
  if kill -0 "$PID" 2>/dev/null; then
    echo "WARN: cnetd $PID ignored SIGTERM after 5s — escalating to SIGKILL" | tee -a "$LOG"
    kill -9 "$PID" 2>/dev/null || true
  fi
  wait "$PID" 2>/dev/null || true
  rm -f "$SOCK"
}
trap cleanup EXIT

# wait sock
for i in $(seq 1 50); do
  [[ -S $SOCK ]] && break
  sleep 0.1
done
[[ -S $SOCK ]] || { echo "FAIL: sock not ready $SOCK" | tee -a "$LOG"; exit 3; }

ask() {
  local q=$1
  python3 - "$SOCK" "$q" <<'PY'
import json, socket, sys
sock, q = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(120)
s.connect(sock)
payload = json.dumps({"op": "ask", "q": q}) + "\n"
s.sendall(payload.encode())
buf = b""
while not buf.endswith(b"\n") and len(buf) < 1_000_000:
    chunk = s.recv(65536)
    if not chunk:
        break
    buf += chunk
s.close()
print(buf.decode("utf-8", "replace").strip())
PY
}

pass_n=0
fail_n=0
check() {
  local name=$1 cond=$2 detail=$3
  if [[ $cond == 1 ]]; then
    echo "  ok   $name  $detail" | tee -a "$LOG"
    pass_n=$((pass_n + 1))
  else
    echo "  FAIL $name  $detail" | tee -a "$LOG"
    fail_n=$((fail_n + 1))
  fi
}

echo "cnet_core_e2e: sock=$SOCK held=$CNET_HELD_MODEL_ENDPOINT" | tee -a "$LOG"

# STATUS
st=$(python3 - "$SOCK" <<'PY'
import socket,sys
s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.settimeout(5); s.connect(sys.argv[1])
s.sendall(b"STATUS\n"); print(s.recv(4096).decode())
PY
)
check status "$([[ $st == OK* ]] && echo 1 || echo 0)" "$st"

# A) LOGIC → CERT
r1=$(ask "what is 2 plus 3")
echo "LOGIC_REPLY $r1" >>"$LOG"
src1=$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(d.get("source",""))' "$r1" 2>/dev/null || echo "")
ans1=$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(d.get("answer") or d.get("utterance") or "")' "$r1" 2>/dev/null || echo "")
ver1=$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(int(bool(d.get("verified"))))' "$r1" 2>/dev/null || echo 0)
miss1=$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(int(bool(d.get("miss"))))' "$r1" 2>/dev/null || echo 1)
check logic_source_local "$([[ $src1 == LOCAL ]] && echo 1 || echo 0)" "source=$src1"
check logic_has_five "$([[ $ans1 == *5* ]] && echo 1 || echo 0)" "answer=$ans1"
check logic_verified "$([[ $ver1 == 1 ]] && echo 1 || echo 0)" "verified=$ver1"
check logic_not_miss "$([[ $miss1 == 0 ]] && echo 1 || echo 0)" "miss=$miss1"

# B) CREATIVE → OPEN_CHAT (Bonsai)
r2=$(ask "write a short haiku about zz99 moons")
echo "CREATIVE_REPLY $r2" >>"$LOG"
src2=$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(d.get("source",""))' "$r2" 2>/dev/null || echo "")
ans2=$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(d.get("answer") or d.get("utterance") or "")' "$r2" 2>/dev/null || echo "")
ver2=$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(int(bool(d.get("verified"))))' "$r2" 2>/dev/null || echo 1)
miss2=$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(int(bool(d.get("miss"))))' "$r2" 2>/dev/null || echo 1)
sk2=$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(d.get("skill",""))' "$r2" 2>/dev/null || echo "")
# OPEN_CHAT leftover answers are killed — creative must not be OPEN_CHAT
check creative_not_open "$([[ $src2 != OPEN_CHAT && $src2 != RESIDUAL ]] && echo 1 || echo 0)" "source=$src2"
check creative_not_cert "$([[ $ver2 == 0 ]] && echo 1 || echo 0)" "verified=$ver2"
check creative_is_miss_or_local "$([[ $miss2 == 1 || $src2 == LOCAL || $src2 == CNET ]] && echo 1 || echo 0)" "miss=$miss2 source=$src2"
check creative_no_held_skill "$([[ $sk2 != held_model_v1 ]] && echo 1 || echo 0)" "skill=$sk2"

# C) LOGIC miss → abstain (no creative fill)
r3=$(ask "compute crc8 of unknown blob zz99nofill")
echo "LOGIC_MISS_REPLY $r3" >>"$LOG"
src3=$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(d.get("source",""))' "$r3" 2>/dev/null || echo "")
ver3=$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(int(bool(d.get("verified"))))' "$r3" 2>/dev/null || echo 1)
# After hemi miss, may fall through to ROE/teacher — accept miss or non-LOCAL unverified
# Prefer: not verified CERT claim
check logic_miss_no_cert "$([[ $ver3 == 0 ]] && echo 1 || echo 0)" "source=$src3 verified=$ver3"

echo "CNET_CORE_E2E_PASS pass=$pass_n fail=$fail_n held=bonsai:8081 core_middle=1" | tee -a "$LOG"
echo "log=$LOG miss=$MISS" | tee -a "$LOG"
[[ $fail_n -eq 0 ]]
