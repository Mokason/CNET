#!/usr/bin/env bash
# Overnight caretaker for the CNET learning loop.
#
# The 24/7 systemd stack learns on its own; this exists for the one thing it
# cannot do -- notice that it has run out of curriculum and mint more. v4 was
# consumed overnight (197 -> 0) and the lane then idled until a human minted v5.
# This closes that gap without needing anyone awake.
#
# Deliberately does NOT commit or push: unattended git writes are a bigger risk
# than a window that waits until morning to be reviewed. It mints, wires and
# restarts so learning continues; the operator commits later.
#
# Silent-ish by design: everything lands in logs/overnight_caretaker.log.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1
ROOT="$PWD"
LOG="$ROOT/logs/overnight_caretaker.log"
BASE="${CNET_BASE_PATH:-$ROOT/soul_gemma4v2_final.cnb}"
INBOX="$BASE.inbox"
GAPS="$BASE.gaps.txt"
BONSAI="${CNET_BONSAI_GGUF:-/home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf}"
LOW_WATER="${CNET_WINDOW_LOW_WATER:-30}"   # mint when remaining drops below this
MEM_FLOOR_GB="${CNET_MEM_FLOOR_GB:-10}"
mkdir -p "$ROOT/logs"
say() { printf '%s %s\n' "$(date -Iseconds)" "$*" | tee -a "$LOG"; }

say "=== caretaker start ==="

# ---- 1. core services -------------------------------------------------------
for u in cnetd cnet-personal-ai-lane bonsai-compete-rocm; do
  if [ "$(systemctl --user is-active $u 2>/dev/null)" != active ]; then
    say "ALERT $u inactive -> restarting"
    systemctl --user restart "$u" 2>/dev/null && say "  $u restarted"
  fi
done
nfail=$(systemctl --user list-units --state=failed --no-legend 2>/dev/null | grep -c .)
[ "$nfail" -gt 0 ] && say "ALERT $nfail failed unit(s): $(systemctl --user list-units --state=failed --no-legend 2>/dev/null | awk '{print $1}' | tr '\n' ' ')"

# ---- 2. memory --------------------------------------------------------------
avail=$(free -g | awk 'NR==2{print $7}')
if [ "${avail:-99}" -lt "$MEM_FLOOR_GB" ]; then
  say "ALERT memory ${avail}GB < ${MEM_FLOOR_GB}GB -> restarting hermes gateway+dashboard"
  # NOT killing CnetMcpServer directly: its watchdog does not respawn, so the
  # session would silently lose the MCP until restarted anyway.
  systemctl --user restart hermes-gateway.service hermes-dashboard.service 2>/dev/null
  sleep 20
  say "  memory now $(free -g | awk 'NR==2{print $7}')GB"
fi

# ---- 3. curriculum runway ---------------------------------------------------
CUR=$(systemctl --user show cnet-personal-ai-lane.service -p Environment 2>/dev/null \
        | tr ' ' '\n' | grep -m1 '^CNET_WINDOW_FILE=' | cut -d= -f2-)
[ -z "$CUR" ] && { say "ALERT cannot resolve CNET_WINDOW_FILE; aborting runway check"; exit 1; }
rem=$(CNET_BASE_PATH="$BASE" CNET_WINDOW_FILE="$CUR" CNET_GAP_INBOX="$INBOX" \
        "$ROOT/bin/gap_inject" --dry-run --n 1 2>&1 | grep -oE 'remaining=[0-9]+' | cut -d= -f2)
units=$(tail -1 "$BASE.hill_climb.jsonl" 2>/dev/null | grep -oE '"units":[0-9]+' | cut -d: -f2)
say "window=$(basename "$CUR") remaining=${rem:-?} units=${units:-?} mem=${avail}GB failed=$nfail"

if [ -n "${rem:-}" ] && [ "$rem" -le "$LOW_WATER" ]; then
  say "runway low (${rem} <= ${LOW_WATER}) -> minting next window"
  ver=$(basename "$CUR" | grep -oE 'v[0-9]+' | tr -d v); ver=$((ver+1))
  NEW="$ROOT/english_window_256_bonsai_v${ver}.txt"
  make -s window_tools >/dev/null 2>&1 || { say "ALERT window_tools build failed"; exit 1; }
  POOL=$(mktemp); EXCL=$(mktemp); trap 'rm -f "$POOL" "$EXCL"' EXIT
  "$ROOT/bin/gen_window_candidates" "$BONSAI" 16735 -o "$POOL" >/dev/null 2>&1 \
    || { say "ALERT candidate pool generation failed"; exit 1; }
  # exclusions: ledger-closed (status field 2, tkNqN with both halves equal),
  # inbox-pending, and every id in every prior window.
  awk 'NR>2 && $2=="2"' "$GAPS" 2>/dev/null | grep -oE 'tk[0-9]+q[0-9]+' \
    | awk -F'[tkq]+' '{if($2==$3) print $2}' >"$EXCL"
  grep -oE 'tk[0-9]+q[0-9]+' "$INBOX" 2>/dev/null \
    | awk -F'[tkq]+' '{if($2==$3) print $2}' >>"$EXCL"
  cat "$ROOT"/english_window_256_bonsai*.txt 2>/dev/null | grep -E '^[0-9]+$' >>"$EXCL"
  awk 'NR==FNR{skip[$1]=1;next} !($1 in skip){print; n++} n>=256{exit}' "$EXCL" "$POOL" >"$NEW"
  got=$(grep -c . "$NEW")
  if [ "$got" -ne 256 ]; then say "ALERT pool exhausted: only $got fresh ids -- NOT wiring"; exit 1; fi
  "$ROOT/bin/xlate_window" dump "$BONSAI" "$NEW" -o /tmp/cw.$$ >/dev/null 2>&1 \
    && awk -F'\t' '{print $1 "\t" $2}' /tmp/cw.$$ >"${NEW%.txt}.words.txt" && rm -f /tmp/cw.$$
  # validate: aligned 1:1 and no empty word (cnet_record_words_load refuses the
  # WHOLE file on one malformed line, which would silently starve the lane)
  bad=$(awk -F'\t' 'NF<2 || $2==""{c++} END{print c+0}' "${NEW%.txt}.words.txt")
  aligned=$(paste <(cat "$NEW") <(cut -f1 "${NEW%.txt}.words.txt") | awk '$1!=$2{c++} END{print c+0}')
  if [ "$bad" -ne 0 ] || [ "$aligned" -ne 0 ]; then
    say "ALERT v${ver} words invalid (bad=$bad misaligned=$aligned) -- NOT wiring"; exit 1
  fi
  fresh=$(CNET_BASE_PATH="$BASE" CNET_WINDOW_FILE="$NEW" CNET_GAP_INBOX="$INBOX" \
            "$ROOT/bin/gap_inject" --dry-run --n 1 2>&1 | grep -oE 'remaining=[0-9]+' | cut -d= -f2)
  say "  v${ver} minted: 256 ids, injector reports remaining=${fresh}"
  D="$HOME/.config/systemd/user"
  for svc in cnet-autoteach cnet-personal-ai-lane; do
    old=$(ls "$D/$svc.service.d/"window-v*.conf 2>/dev/null | head -1)
    [ -n "$old" ] || continue
    sed -i "s#english_window_256_bonsai_v[0-9]*\.txt#english_window_256_bonsai_v${ver}.txt#g" "$old"
    mv "$old" "$D/$svc.service.d/window-v${ver}.conf"
  done
  systemctl --user daemon-reload
  systemctl --user restart cnet-personal-ai-lane.service
  systemctl --user start cnet-autoteach.service
  say "  wired v${ver} and restarted lane"
fi
say "=== caretaker done ==="
