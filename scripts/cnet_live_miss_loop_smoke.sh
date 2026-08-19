#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
DIR=/tmp/cnet_live_loop_bricks
MISS=/tmp/cnet_live_loop_miss.jsonl
SOCK=/tmp/cnetd-live-loop.sock
rm -rf "$DIR"
mkdir -p "$DIR" result
: > "$MISS"
rm -f "$SOCK"

export CNET_GGUF_MMAP=1
export CNET_CORE_BUS_BRICKS_DIR=$DIR
export CNET_CORE_AUTO_EVOLVE=1
export CNET_CORE_EVOLVE_EVERY=1
export CNET_CORE_EVOLVE_FACTORY=0
export CNET_CORE_EVOLVE_BIN=$ROOT/bin/cnet_core_evolve
export CNET_BONSAI_GGUF=/home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf
export CNET_SOCK=$SOCK
export CNET_PACKS_ROOT=$ROOT/artifacts/roe_daily_packs
export CNET_MISS_LOG=$MISS

./bin/test_cnet_live_miss_loop | tee /tmp/live_miss_unit.out
grep -q 'fails=0' /tmp/live_miss_unit.out

"$ROOT/bin/cnetd" > /tmp/cnetd_live_loop.log 2>&1 &
PID=$!
cleanup() { kill "$PID" 2>/dev/null || true; sleep 0.2; kill -9 "$PID" 2>/dev/null || true; }
trap cleanup EXIT
for i in $(seq 1 50); do [[ -S $SOCK ]] && break; sleep 0.1; done
[[ -S $SOCK ]]

python3 - <<'PY'
import socket, json, os
sock=os.environ["CNET_SOCK"]

def ask(q):
    s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
    s.settimeout(120)
    s.connect(sock)
    s.sendall((json.dumps({"op":"ask","q":q})+"\n").encode())
    buf=b""
    while not buf.endswith(b"\n") and len(buf)<1000000:
        c=s.recv(65536)
        if not c: break
        buf+=c
    s.close()
    return json.loads(buf.decode())

for i in range(16):
    r=ask(f"teach live_dom {i} {(i*2+1)&15}")
print("last_teach", r.get("source"), r.get("verified"), r.get("answer"), r.get("skill"))

r2=ask("live_dom 3")
print("QUERY", r2.get("source"), r2.get("verified"), r2.get("answer"), r2.get("skill"))

r3=ask("goal: prove live_dom at 3")
print("GOAL", r3.get("source"), r3.get("verified"), r3.get("answer"), r3.get("skill"))

ok = (r2.get("verified") is True and r2.get("source")=="LOCAL"
      and str(r2.get("answer","")).strip().isdigit())
ok = ok and (r3.get("verified") is True or str(r3.get("answer","")).strip().isdigit()
             or r3.get("skill") in ("goal_cert","live_dom","live_live_dom","core_brick"))
print("CNET_LIVE_MISS_LOOP_PASS" if ok else "CNET_LIVE_MISS_LOOP_FAIL")
open("result/bench_live_miss_loop.txt","w").write(
    f"last_teach={r}\nQUERY={r2}\nGOAL={r3}\nPASS={ok}\n")
if not ok:
    raise SystemExit(1)
PY
cleanup
trap - EXIT
exit 0
