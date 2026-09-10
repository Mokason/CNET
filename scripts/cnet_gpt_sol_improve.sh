#!/usr/bin/env bash
# gpt-sol background improver for CNET.
#
# When cnetd is stuck on an *operator-shaped* hole, spawn Codex (gpt-5.6-sol)
# to propose ONE brick / C organ / capsule. Never FAQ-seal leftover English.
# Never gold. Never `make cnetd-run`. auto_cert=false.
#
# Usage:
#   scripts/cnet_gpt_sol_improve.sh --dry-run
#   scripts/cnet_gpt_sol_improve.sh --once
#
# systemd: cnet-gpt-sol-improve.timer (2h). Do not reuse improvement-daemon.service
# (missing Python venv; 203/EXEC).
set -eu
CNET="${CNET:-$HOME/AI/CNET}"
MIN="${CNET_MINIMAL_ROOT:-$HOME/.local/share/cnet-minimal/current}"
JOBS="${CNET_GPT_SOL_JOBS:-$MIN/var/gpt_sol_jobs.jsonl}"
MEM="${CNET_MEMORY_MD:-$MIN/var/marble_memory.md}"
MISS="${CNET_MISS_LOG:-$MIN/data/roe_daily_packs/miss_log.jsonl}"
STATE="${CNET_GPT_SOL_STATE:-$MIN/var/gpt_sol_improve_state}"
BRICKS="${CNET_CORE_BUS_BRICKS_DIR:-$HOME/.local/share/cnet-bricks}"
LOCK="${CNET_GPT_SOL_LOCK:-${XDG_RUNTIME_DIR:-/tmp}/cnet-gpt-sol-improve.lock}"
WITNESS_DIR="${CNET_GPT_SOL_WITNESS_DIR:-/tmp}"
PICK_FILE="${CNET_GPT_SOL_PICK:-/tmp/cnet_gpt_sol_pick.txt}"
CODEX="${CODEX:-$HOME/.npm-global/bin/codex}"
PATH="$HOME/.npm-global/bin:$HOME/.local/bin:/usr/local/bin:/usr/bin:/bin:$PATH"
export PATH CNET_GPT_SOL_PICK="$PICK_FILE"
MODE=once
for a in "$@"; do
  case "$a" in
    --dry-run) MODE=dry ;;
    --once) MODE=once ;;
    --force) export CNET_GPT_SOL_FORCE=1 ;;
    -h|--help)
      echo "usage: $0 [--dry-run|--once|--force]"
      exit 0
      ;;
  esac
done

# python3 for harvest only. Create a venv only if this unit has no interpreter.
PY=python3
if ! command -v python3 >/dev/null 2>&1; then
  VENV="${CNET_GPT_SOL_VENV:-$MIN/var/gpt-sol-venv}"
  if [[ ! -x "$VENV/bin/python" ]]; then
    echo "python3 missing; creating venv $VENV"
    /usr/bin/python3 -m venv "$VENV"
  fi
  PY="$VENV/bin/python"
fi

mkdir -p "$MIN/var" "$WITNESS_DIR"
exec 9>"$LOCK"
if ! flock -n 9; then
  echo "SKIPPED locked=$LOCK"
  exit 0
fi

# classifier: leftover encyclopedia / presence / probes are NOT improver jobs.
"$PY" - "$JOBS" "$MEM" "$MISS" "$STATE" "$BRICKS" "$WITNESS_DIR" <<'PY'
import hashlib, json, os, re, sys, time
from pathlib import Path

jobs_p, mem_p, miss_p, state_p, bricks_p, witness_dir = map(Path, sys.argv[1:])
SKIP_RE = re.compile(
    r"(how are you|how'?s it going|what would you like|what do you want|"
    r"yeah i know|good (morning|evening)|are you there|due reminders|"
    r"remind in|hello world|capital of|tailscale|c#|csharp|"
    r"why is json|do you know json|json used|tokyo|weather|"
    r"autonomous cycle probe|novel fact|curriculum_probe|"
    r"who are you|who is marble|what is an llm|what can you do|"
    r"minutes in seconds|plus 5 then min|look up json|crc8 |"
    r"not bored|still bored|feeling better|how are you now|"
    r"brick by brick|language model)",
    re.I,
)
CLONE_MINT_RE = re.compile(
    r"\bmint\s+(?:the\s+)?next\s+unused\s+27b\s+tensor\s+brick\b",
    re.I,
)
ATTN_QKV_CLONE_RE = re.compile(
    r"(?:\battn[_ -]?qkv\b.*\b(?:brick|nibble|tensor)\b|"
    r"\b(?:brick|nibble|tensor)\b.*\battn[_ -]?qkv\b)",
    re.I,
)
NEW_MAP_RE = re.compile(
    r"\b(?:compose|composition|split)\b|"
    r"\bdomain(?:[ _-]+to)?[ _-]+table\b",
    re.I,
)
TAKE_RE = re.compile(
    r"\b(lut|tensor|brick|capsule|compose|organ|q1_|crc8?|bitwise|"
    r"clamp|minutes|gpu|host[_ ]load|wiki|domain to table|"
    r"infix|plus then|then min|then max|attn_qkv|ffn_down)\b",
    re.I,
)

def load_lines(p, n=80):
    if not p.is_file():
        return []
    try:
        rows = p.read_text(errors="replace").splitlines()
    except OSError:
        return []
    return rows[-n:]

def load_text(p):
    if not p.is_file():
        return ""
    try:
        return p.read_text(errors="replace")
    except OSError:
        return ""

def state_value(text, key):
    m = re.search(rf"(?m)^{re.escape(key)}=(.*)$", text)
    return m.group(1).strip() if m else ""

state_text = load_text(state_p)
previous_q = state_value(state_text, "q")
evidence = [state_text]
witness_paths = []
state_witness = state_value(state_text, "witness")
if state_witness:
    witness_paths.append(Path(state_witness))
witness_paths.append(witness_dir / "cnet-gpt-sol-improve-latest.md")
for witness_p in witness_paths:
    evidence.append(load_text(witness_p))

live_witness_tags = set()
for tag in re.findall(r"\b(q1_27b\d+)\.lut\b", "\n".join(evidence), re.I):
    tag = tag.lower()
    if (bricks_p / f"{tag}.lut").is_file():
        live_witness_tags.add(tag)

def tag_number(tag):
    m = re.search(r"(\d+)$", tag)
    return int(m.group(1)) if m else -1

newest_live_witness_tag = (
    max(live_witness_tags, key=tag_number) if live_witness_tags else ""
)
live_27b_count = sum(1 for p in bricks_p.glob("q1_27b*.lut") if p.is_file())

def clone_nibble_reason(q):
    normalized = q.strip().casefold()
    if (
        previous_q
        and normalized == previous_q.strip().casefold()
        and newest_live_witness_tag
    ):
        return f"landed={newest_live_witness_tag}"
    asks_for_clone = bool(CLONE_MINT_RE.search(q) or ATTN_QKV_CLONE_RE.search(q))
    if asks_for_clone and not NEW_MAP_RE.search(q) and live_27b_count >= 22:
        return f"bank_q1_27b={live_27b_count}"
    return ""

cands = []
for line in load_lines(jobs_p, 40):
    line = line.strip()
    if not line:
        continue
    q = None
    if line.startswith("{"):
        try:
            o = json.loads(line)
            q = (o.get("query") or o.get("q")) if isinstance(o, dict) else None
        except json.JSONDecodeError:
            q = None
    if q is not None and not isinstance(q, str):
        q = None
    if not q:
        q = line
    cands.append(("job", q.strip()))

for line in load_lines(mem_p, 200):
    if line.startswith("GAP:"):
        q = line[4:].split("|", 1)[0].strip()
        if q:
            cands.append(("gap", q))

for line in load_lines(miss_p, 30):
    if not line.startswith("{"):
        continue
    try:
        o = json.loads(line)
    except json.JSONDecodeError:
        continue
    if not isinstance(o, dict):
        continue
    if o.get("probe_pat") or o.get("via") == "autonomous_cycle":
        continue
    q = o.get("query") or ""
    if not isinstance(q, str):
        continue
    q = q.strip()
    if q:
        cands.append(("miss", q))

picked = None
src = None
seen = set()
clone_skips = []
for src0, q in reversed(cands):
    key = q.lower()
    if key in seen:
        continue
    seen.add(key)
    if SKIP_RE.search(q):
        continue
    clone_reason = clone_nibble_reason(q)
    if clone_reason:
        clone_skips.append((q, clone_reason))
        continue
    if TAKE_RE.search(q):
        picked, src = q, src0
        break

# Leftover jobs (cnetd nanny queue) that are not encyclopedia/presence.
if not picked:
    for src0, q in reversed(cands):
        if src0 != "job":
            continue
        if SKIP_RE.search(q):
            continue
        clone_reason = clone_nibble_reason(q)
        if clone_reason:
            clone_skips.append((q, clone_reason))
            continue
        picked, src = q, src0
        break

out = Path(os.environ.get("CNET_GPT_SOL_PICK", "/tmp/cnet_gpt_sol_pick.txt"))
if not picked:
    if clone_skips:
        skipped_q, reason = clone_skips[0]
        print(f"SKIPPED clone_nibble {reason} q={skipped_q!r}")
    else:
        print("SKIPPED no_operator_gap")
    out.write_text("")
    raise SystemExit(0)

h = hashlib.sha1(picked.encode()).hexdigest()[:16]
prev = ""
if state_p.is_file():
    prev = state_p.read_text(errors="replace")
if f"hash={h}" in prev and os.environ.get("CNET_GPT_SOL_FORCE") != "1":
    # same hole already handed to gpt-sol
    age = 0
    m = re.search(r"ts=(\d+)", prev)
    if m:
        age = int(time.time()) - int(m.group(1))
    if age < 20 * 3600:
        print(f"SKIPPED already_queued hash={h} age={age}s q={picked!r}")
        out.write_text("")
        raise SystemExit(0)

print(f"PICK src={src} hash={h} q={picked}")
out.write_text(picked + "\n")
PY
if [[ ! -s "$PICK_FILE" ]]; then
  exit 0
fi
Q="$(tr -d '\r' < "$PICK_FILE" | head -n 1)"
H="$(printf '%s' "$Q" | sha1sum | awk '{print substr($1,1,16)}')"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
WITNESS="${CNET_GPT_SOL_WITNESS:-$WITNESS_DIR/cnet-gpt-sol-improve-$STAMP.md}"
PROMPT="/tmp/cnet-gpt-sol-improve.prompt.txt"

if [[ "$MODE" == dry ]]; then
  echo "DRY q=$Q witness_would=$WITNESS"
  exit 0
fi

cat > "$PROMPT" <<EOF
You are gpt-sol (Codex) as CNET's background improver. One stuck hole, one artifact.

STUCK QUERY:
$Q

Repo: $CNET
Live bank: $BRICKS
Capsule inbox: $MIN/var/capsule_inbox
Witness (must write): $WITNESS
First line of witness: # gpt-sol CNET improver
Last line of witness: END_GPT_SOL

LAW (hard):
- CERT the operator / LUT-from-weights, never a Q→A FAQ row.
- Do NOT gold leftover English (JSON, C#, Tailscale, capitals, hello world).
- Do NOT grow pack_english_basic. Do NOT gold 10+11=21 or 20/4=5.
- Capsule propose auto_cert=false. Never self-CERT. Never make cnetd-run (it pkills cnetd).
- No MCP (no codebase-memory, graphify, context7). Local rg/sed/cat only.
- Do not touch identity slots (soul_who / soul_operator / soul_ack).
- Do not edit ~/.hermes memory. Do not FAQ-seal personality ("I like X").

DO exactly ONE of, in this order of fit:
1. BRICK: if the hole is tensor/domain/LUT, mint the next unused 27B tensor with
   CNET_GGUF_MMAP=1 ./bin/cnet_pq2_brick_mint (fused attn_qkv, else SSM split attn_q;
   never steal blk.0). cp the new .lut into ~/.local/share/cnet-bricks/. Do not
   exceed CNET_CORE_BUS_MAX_BRICKS. Compose TABLE only if two new luts exist.
2. ORGAN: if the hole is an operator CNET should compute (minutes, crc, compose,
   host sensor, dialog), add a C operator + gate. make the relevant target
   (showrunner / cnet_ood / query_alias / cnetd). Never make cnetd-run.
   Restart is the operator's job: print RESTART_NEEDED=1 in the witness.
3. CAPSULE: ./bin/cnet_capsule_propose --unit <name> --out $MIN/var/capsule_inbox
   (auto_cert=false). Do not admit.

If the stuck query is leftover encyclopedia (JSON/C#/facts), write the witness
with SKIP encyclopedia leftover and stop. Do not implement a FAQ.

Witness must include: STUCK, KIND=brick|organ|capsule|skip, PATHS created,
GATES run, CERT=0 or existing, RESTART_NEEDED=0|1,
SPOKEN=<one line CERT 0: what landed and how to ask it>.
EOF

if [[ ! -x "$CODEX" ]]; then
  echo "codex missing; herdr needed"
  NEED_HERDR=1
else
  NEED_HERDR=0
fi
if [[ "${CNET_GPT_SOL_HERDR:-0}" == "1" ]]; then
  NEED_HERDR=1
fi

herdr_spawn() {
  # Start the GUI server only if we actually need a pane.
  export DISPLAY="${DISPLAY:-:0}"
  if ! herdr status 2>/dev/null | grep -q 'status: running'; then
    echo "starting herdr server (needed)"
    nohup herdr >/tmp/cnet-gpt-sol-herdr.log 2>&1 &
    for _ in 1 2 3 4 5 6 7 8 9 10; do
      herdr status 2>/dev/null | grep -q 'status: running' && break
      sleep 1
    done
  fi
  local ws pane name
  ws="$(herdr workspace create --label cnet-gpt-sol-improve 2>/dev/null | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d.get("workspace_id") or d.get("id") or "")' || true)"
  pane="$(herdr workspace list 2>/dev/null | python3 -c 'import json,sys,re
raw=sys.stdin.read()
try:
  d=json.loads(raw)
except Exception:
  print(""); raise SystemExit
# tolerate list or dict
items=d if isinstance(d,list) else d.get("workspaces") or d.get("data") or []
for w in items:
  if "gpt-sol-improve" in str(w.get("label") or ""):
    print(w.get("pane_id") or w.get("root_pane") or "")
    break
' || true)"
  name="gpt-sol-improve"
  herdr agent rename "$name" --clear 2>/dev/null || true
  if [[ -z "$pane" ]]; then
    echo "BLOCKED herdr_no_pane" | tee "$WITNESS"
    echo "END_GPT_SOL" >> "$WITNESS"
    return 1
  fi
  herdr agent start "$name" --kind codex --pane "$pane" -- -s danger-full-access -a never
  python3 - "$name" "$PROMPT" <<'PY'
import pathlib, subprocess, sys
name, prompt = sys.argv[1], sys.argv[2]
text = pathlib.Path(prompt).read_text()
r = subprocess.run(["herdr","agent","prompt",name,text,"--wait","--until","working","--timeout","45000"],
                   text=True)
raise SystemExit(r.returncode)
PY
}

echo "SPAWN q=$Q witness=$WITNESS need_herdr=$NEED_HERDR"
set +e
rc=1
if [[ "$NEED_HERDR" == "1" ]]; then
  herdr_spawn
  rc=$?
else
  # -a never is a top-level flag (not on `exec`). danger-full-access so rg of C works.
  "$CODEX" -a never exec \
    -C "$CNET" \
    -s danger-full-access \
    --skip-git-repo-check \
    - < "$PROMPT"
  rc=$?
  if [[ $rc -ne 0 && ! -s "$WITNESS" ]]; then
    echo "codex exec failed rc=$rc; herdr fallback"
    herdr_spawn
    rc=$?
  fi
fi
set -e

if [[ ! -s "$WITNESS" ]]; then
  {
    echo "# gpt-sol CNET improver"
    echo "STUCK: $Q"
    echo "KIND=skip"
    echo "NOTE: codex exec rc=$rc produced no witness; parent stub."
    echo "END_GPT_SOL"
  } > "$WITNESS"
fi

# A completed brick consumes its exact queue request. Model output is parsed as
# data only; the query is never interpolated into shell code.
if [[ "$rc" -eq 0 ]]; then
  "$PY" - "$JOBS" "$Q" "$WITNESS" "$BRICKS" <<'PY'
import json, os, re, stat, sys, tempfile
from pathlib import Path

jobs_p = Path(sys.argv[1])
query = sys.argv[2]
witness_p = Path(sys.argv[3])
bricks_p = Path(sys.argv[4])
try:
    witness_text = witness_p.read_text(errors="replace")
except OSError:
    raise SystemExit(0)
witness_lines = witness_text.splitlines()
kind = next((line.partition("=")[2].strip() for line in witness_lines
             if line.startswith("KIND=")), "")
if kind != "brick" or not jobs_p.is_file():
    raise SystemExit(0)
live_luts = {
    name for name in re.findall(r"\b([A-Za-z0-9_.-]+\.lut)\b", witness_text)
    if (bricks_p / name).is_file()
}
if not live_luts:
    print("QUEUE_CLEAN skipped=no_live_brick")
    raise SystemExit(0)

target = query.strip().casefold()
try:
    original = jobs_p.read_bytes().splitlines(keepends=True)
except OSError as exc:
    print(f"QUEUE_CLEAN_FAIL read={type(exc).__name__}", file=sys.stderr)
    raise SystemExit(1)

def queued_query(line):
    try:
        text = line.decode("utf-8").rstrip("\r\n")
    except UnicodeDecodeError:
        return ""
    if text.lstrip().startswith("{"):
        try:
            row = json.loads(text)
        except json.JSONDecodeError:
            return text.strip()
        if not isinstance(row, dict):
            return ""
        value = row.get("query") or row.get("q")
        return value.strip() if isinstance(value, str) else ""
    return text.strip()

kept = [line for line in original
        if queued_query(line).casefold() != target]
removed = len(original) - len(kept)
if not removed:
    print("QUEUE_CLEAN removed=0")
    raise SystemExit(0)

mode = stat.S_IMODE(jobs_p.stat().st_mode)
fd, temp_name = tempfile.mkstemp(prefix=f".{jobs_p.name}.", dir=jobs_p.parent)
try:
    with os.fdopen(fd, "wb") as out:
        out.writelines(kept)
        out.flush()
        os.fsync(out.fileno())
    os.chmod(temp_name, mode)
    os.replace(temp_name, jobs_p)
finally:
    try:
        os.unlink(temp_name)
    except FileNotFoundError:
        pass
print(f"QUEUE_CLEAN removed={removed}")
PY
fi
ln -sfn "$WITNESS" "$WITNESS_DIR/cnet-gpt-sol-improve-latest.md"
printf 'ts=%s\nhash=%s\nstatus=rc%s\nq=%s\nwitness=%s\n' \
  "$(date +%s)" "$H" "$rc" "$Q" "$WITNESS" > "$STATE"
echo "DONE rc=$rc witness=$WITNESS"
# Follow-up into last Discord origin. Skip on KIND=skip.
if [[ -x "$CNET/scripts/cnet_nanny_deliver.sh" ]]; then
  "$CNET/scripts/cnet_nanny_deliver.sh" "$WITNESS" || true
fi
exit 0
