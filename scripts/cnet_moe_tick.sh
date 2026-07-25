#!/usr/bin/env bash
# CNET 24/7 learning tick — transformer-MoE block against a held-out entropy floor.
#
# Replaces the gap lane as the loop's learning substrate. The gap lane's unit is
# a one-hot -> one-hot table row certified by exact replay; a one-hot input space
# has no metric, so more rows never become generalisation. Here the artefact is a
# checkpoint and the predicate is:
#
#     certified := held-out CE below H1, the analytic order-1 plateau
#
# H1 cannot be reached by memorising the marginal — beating it requires the
# attention to use the previous token — and the source is sampled fresh forever,
# so there is no training set to overfit.
#
# Two checkpoints, deliberately:
#   live.ckpt  training always advances from here (SGD is noisy; ratcheting the
#              live weights on every worse tick would stop learning outright)
#   best.ckpt  replaced only when held-out CE improves — the certified artefact
# A divergence guard rolls live back to best if CE blows up or goes non-finite.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

DIR="${CNET_MOE_DIR:-$REPO/artifacts/moe}"
LIVE="$DIR/live.ckpt"
BEST="$DIR/best.ckpt"
STATE="$DIR/state.json"
EVID="$DIR/evidence.jsonl"
LOG="$DIR/tick_$(date +%Y%m%d).log"
LOCK="$DIR/tick.lock"
mkdir -p "$DIR"

exec 9>"$LOCK"
if ! flock -n 9; then echo "moe_tick: another tick holds the lock; exit"; exit 0; fi

STEPS="${MOE_STEPS:-400}"
EVAL_N="${MOE_EVAL_N:-64}"
LR="${MOE_LR:-0.003}"
DIVERGE_FACTOR="${MOE_DIVERGE_FACTOR:-1.25}"

if [[ ! -x bin/moe_xf_tick ]]; then
  echo "moe_tick: bin/moe_xf_tick missing (make moe_xf_tick)" >&2
  exit 1
fi

out=$(MOE_CKPT="$LIVE" MOE_CKPT_OUT="$LIVE.cand" MOE_STEPS="$STEPS" \
      MOE_EVAL_N="$EVAL_N" MOE_LR="$LR" \
      timeout "${MOE_TIMEOUT:-3000}" ./bin/moe_xf_tick 2>&1) || {
  echo "$out" | tail -5 >&2
  echo "moe_tick: trainer failed" >&2
  exit 1
}
echo "[$(date -Iseconds)] $out" >>"$LOG"

CNET_MOE_DIR="$DIR" python3 - "$out" <<'PY'
import json, os, subprocess, sys, time, hashlib, math
from pathlib import Path

raw = sys.argv[1]
line = [l for l in raw.splitlines() if l.startswith("MOE_TICK ")]
if not line:
    print("moe_tick: no MOE_TICK line", file=sys.stderr); raise SystemExit(1)
t = json.loads(line[-1].split(" ", 1)[1])

# Passed explicitly by the caller: inside `python3 - <<PY` __file__ is "<stdin>",
# so a Path(__file__)-based default resolves outside the repo.
DIR   = Path(os.environ["CNET_MOE_DIR"])
LIVE  = DIR / "live.ckpt"
BEST  = DIR / "best.ckpt"
STATE = DIR / "state.json"
EVID  = DIR / "evidence.jsonl"
CAND  = Path(str(LIVE) + ".cand")
factor = float(os.environ.get("MOE_DIVERGE_FACTOR", "1.25"))

st = {}
if STATE.exists():
    try: st = json.loads(STATE.read_text())
    except Exception: st = {}

ce   = float(t["heldout_ce"])
h1   = float(t["h1"])
h2   = float(t["h2"])
eval_n = int(t["eval_n"])

# The floors are sample means of the analytic per-sequence oracle, so a
# different eval_n is a DIFFERENT held-out set with different floors: CE is not
# comparable across it. Reset the ratchet rather than ratchet against a number
# that measured something else.
prev_n = st.get("eval_n")
basis_changed = prev_n is not None and int(prev_n) != eval_n
best = None if basis_changed else st.get("best_ce")
best = float(best) if best is not None else None
if basis_changed:
    st["basis_reset"] = f"eval_n {prev_n} -> {eval_n}; previous best_ce discarded"

def sha(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()

action = "advanced"
if not math.isfinite(ce):
    action = "diverged_rollback"
elif best is not None and ce > best * factor:
    action = "diverged_rollback"

if action == "diverged_rollback":
    # Live weights blew up: restart training from the last certified artefact
    # rather than carrying a broken model into the next tick.
    CAND.unlink(missing_ok=True)
    if BEST.exists():
        LIVE.write_bytes(BEST.read_bytes())
else:
    CAND.replace(LIVE)          # training always advances
    if best is None or ce < best:
        BEST.write_bytes(LIVE.read_bytes())
        action = "promoted"

promoted = action == "promoted"
certified = bool(promoted and ce < h1)

st.update({
    "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    "step": t["step_to"],
    "heldout_ce": ce,
    "train_ce": t["train_ce"],
    "h1": h1, "h2": h2, "uniform": t["uniform"],
    "gap_to_floor": ce - h2,
    "below_h1": bool(ce < h1),
    "action": action,
    "device": t["device"],
    "eval_n": t["eval_n"],
    "secs": t["secs"],
})
if promoted:
    st["best_ce"] = ce
    st["best_step"] = t["step_to"]
    st["best_sha256"] = sha(BEST)
    st["certified"] = certified
st.setdefault("best_ce", ce if promoted else (best if best is not None else ce))
STATE.write_text(json.dumps(st, indent=2) + "\n")

# Evidence record. Same discipline as the CNB units — what was measured, on what,
# with an artefact hash — but the claim is a held-out CE, not a replay-exact row.
if promoted:
    with EVID.open("a") as f:
        f.write(json.dumps({
            "version": 1,
            "kind": "moe_xf_checkpoint",
            "ts": st["ts"],
            "step": t["step_to"],
            "heldout_ce": ce,
            "h1": h1, "h2": h2,
            "gap_to_floor": ce - h2,
            "certified_below_h1": certified,
            "eval_n": t["eval_n"],
            "eval_seed": "0xE7A15EED",
            "source": "markov2_order2_fresh_stream",
            "artifact_sha256": st["best_sha256"],
            "device": t["device"],
        }) + "\n")

print("MOE_TICK_OK", json.dumps({
    "action": action, "step": t["step_to"], "heldout_ce": round(ce, 6),
    "best_ce": round(float(st["best_ce"]), 6), "h1": round(h1, 6), "h2": round(h2, 6),
    "gap_to_floor": round(ce - h2, 6), "below_h1": st["below_h1"],
    "certified": certified,
}))
PY
