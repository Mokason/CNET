#!/usr/bin/env python3
"""CNET self-direction governor.

Not a queue poller: reads charter + live scoreboard, picks goals, EMITS work
into existing muscles (inbox, autoteach, PEFT, mine, lane).

Usage:
  python3 scripts/cnet_governor.py              # one cycle
  python3 scripts/cnet_governor.py --dry-run    # decide only
  python3 scripts/cnet_governor.py --test       # hermetic self-check
  make governor                                 # gate
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, asdict, field
from datetime import datetime
from pathlib import Path
from typing import Any

REPO = Path(os.environ.get("CNET_ROOT", Path(__file__).resolve().parents[1]))
CHARTER_PATH = Path(
    os.environ.get("CNET_GOVERNOR_CHARTER", REPO / "config/cnet_governor_charter.yaml")
)
STATE_PATH = Path(
    os.environ.get("CNET_GOVERNOR_STATE", REPO / "logs/governor/state.json")
)
SCORE_PATH = Path(
    os.environ.get("CNET_GOVERNOR_SCOREBOARD", REPO / "logs/governor/scoreboard.json")
)
DECISION_PATH = Path(
    os.environ.get("CNET_GOVERNOR_LAST", REPO / "logs/governor/last_decision.json")
)
BASE = Path(
    os.environ.get("CNET_BASE_PATH", REPO / "soul_gemma4v2_final.cnb")
)
INBOX = Path(os.environ.get("CNET_GAP_INBOX", str(BASE) + ".inbox"))
GAPS = Path(str(BASE) + ".gaps.txt")
FAULT = Path(os.environ.get("CNET_FAULT_LOG", REPO / "logs/cnet_faults.jsonl"))
LORA_DIR = Path(os.environ.get("CNET_LORA_STORE_DIR", REPO / "logs/lora_store"))
WINDOW = Path(
    os.environ.get(
        "CNET_RESIDUAL_WINDOW",
        os.environ.get("CNET_WINDOW_FILE", REPO / "english_window_256_bonsai.txt"),
    )
)
HTTP = os.environ.get("CNET_RESIDUAL_HTTP", "http://127.0.0.1:8080")
INJECT_N = int(os.environ.get("CNET_GOVERNOR_INJECT", "4"))


# --- minimal YAML subset (no PyYAML required) ---------------------------------
def parse_charter(text: str) -> dict[str, Any]:
    """Parse our simple charter YAML (version/goals list)."""
    data: dict[str, Any] = {"goals": [], "max_tasks_per_cycle": 3, "budget_sec": 900}
    cur: dict[str, Any] | None = None
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].rstrip()
        if not line.strip():
            continue
        if re.match(r"^version:\s*", line):
            data["version"] = int(line.split(":", 1)[1].strip())
        elif re.match(r"^max_tasks_per_cycle:\s*", line):
            data["max_tasks_per_cycle"] = int(line.split(":", 1)[1].strip())
        elif re.match(r"^budget_sec:\s*", line):
            data["budget_sec"] = int(line.split(":", 1)[1].strip())
        elif re.match(r"^\s*-\s*id:\s*", line):
            if cur:
                data["goals"].append(cur)
            cur = {"id": line.split("id:", 1)[1].strip()}
        elif cur is not None:
            m = re.match(r"^\s+(priority|description|when|actions):\s*(.*)$", line)
            if not m:
                continue
            k, v = m.group(1), m.group(2).strip()
            if k == "priority":
                cur[k] = int(v)
            elif k == "actions":
                inner = v.strip("[]")
                cur[k] = [a.strip() for a in inner.split(",") if a.strip()]
            else:
                cur[k] = v.strip().strip('"').strip("'")
    if cur:
        data["goals"].append(cur)
    return data


@dataclass
class Scoreboard:
    ts: str
    bonsai_ok: int = 0
    lane_ok: int = 0
    open_gaps: int = 0
    deferred_oracle: int = 0
    closed_gaps: int = 0
    fault_lines: int = 0
    lora_files: int = 0
    units_proxy: int = 0
    hyb_struct: int = 0
    base_size: int = 0
    hours_since_peft: float = 99.0
    hours_since_mine: float = 99.0
    hours_since_procedure: float = 99.0
    last_autoteach: dict[str, Any] = field(default_factory=dict)

    def as_env(self) -> dict[str, Any]:
        d = asdict(self)
        # expose ints for eval
        return d


def _active(unit: str) -> int:
    try:
        r = subprocess.run(
            ["systemctl", "--user", "is-active", "--quiet", unit],
            check=False,
        )
        return 1 if r.returncode == 0 else 0
    except Exception:
        return 0


def _http_ok(url: str) -> int:
    try:
        import urllib.request

        urllib.request.urlopen(url.rstrip("/") + "/v1/models", timeout=4)
        return 1
    except Exception:
        return 0


def _gap_counts(path: Path) -> tuple[int, int, int]:
    """Return open, deferred_waiting_oracle-ish, closed from ledger."""
    open_n = def_n = closed_n = 0
    if not path.exists():
        return 0, 0, 0
    try:
        lines = path.read_text(errors="replace").splitlines()
    except Exception:
        return 0, 0, 0
    # Format observed: leading fields include status in col2 (1 open/deferred family, 2 closed)
    # Also search waiting_oracle token.
    for ln in lines[2:]:
        if not ln.strip() or ln.startswith("CNET_"):
            continue
        parts = ln.split()
        if len(parts) < 2:
            continue
        st = parts[1] if parts[1].isdigit() else ""
        if "waiting_oracle" in ln or "waiting_charter" in ln:
            def_n += 1
        elif st == "2":
            closed_n += 1
        elif st == "1":
            open_n += 1
        else:
            # status 0-ish open candidates
            if parts[0].isdigit():
                open_n += 1
    return open_n, def_n, closed_n


def _hours_since(state: dict[str, Any], key: str) -> float:
    t = state.get(key)
    if not t:
        return 99.0
    try:
        return max(0.0, (time.time() - float(t)) / 3600.0)
    except Exception:
        return 99.0


def collect_scoreboard(state: dict[str, Any]) -> Scoreboard:
    open_n, def_n, closed_n = _gap_counts(GAPS)
    fault_n = 0
    if FAULT.exists():
        try:
            fault_n = sum(1 for _ in FAULT.open())
        except Exception:
            fault_n = 0
    lora_n = len(list(LORA_DIR.glob("*"))) if LORA_DIR.is_dir() else 0
    units = hyb = size = 0
    if BASE.exists():
        b = BASE.read_bytes()
        units = b.count(b"acq_") + b.count(b"json_toolcall") + b.count(b"hyb_struct")
        hyb = b.count(b"hyb_struct")
        size = BASE.stat().st_size
    at: dict[str, Any] = {}
    atp = REPO / "logs/autoteach/last_tick.json"
    if atp.exists():
        try:
            at = json.loads(atp.read_text())
        except Exception:
            at = {}
    sb = Scoreboard(
        ts=datetime.now().astimezone().isoformat(timespec="seconds"),
        bonsai_ok=1 if _active("bonsai-server.service") and _http_ok(HTTP) else 0,
        lane_ok=1 if _active("cnet-personal-ai-lane.service") else 0,
        open_gaps=open_n,
        deferred_oracle=def_n,
        closed_gaps=closed_n,
        fault_lines=fault_n,
        lora_files=lora_n,
        units_proxy=units,
        hyb_struct=hyb,
        base_size=size,
        hours_since_peft=_hours_since(state, "last_peft_unix"),
        hours_since_mine=_hours_since(state, "last_mine_unix"),
        hours_since_procedure=_hours_since(state, "last_procedure_unix"),
        last_autoteach=at,
    )
    return sb


def eval_when(expr: str, env: dict[str, Any]) -> bool:
    """Safe boolean eval over scoreboard numeric fields."""
    if expr.strip() == "true":
        return True
    if expr.strip() == "false":
        return False
    # allow only names, numbers, and operators
    if not re.match(r"^[0-9a-zA-Z_.<>=!&|+\-*/ ()]+$", expr):
        return False
    local = {k: v for k, v in env.items() if isinstance(v, (int, float, bool))}
    try:
        return bool(eval(expr, {"__builtins__": {}}, local))  # noqa: S307 — sandboxed
    except Exception:
        return False


def pick_goals(charter: dict[str, Any], sb: Scoreboard) -> list[dict[str, Any]]:
    env = sb.as_env()
    goals = sorted(charter.get("goals") or [], key=lambda g: int(g.get("priority", 99)))
    picked: list[dict[str, Any]] = []
    max_n = int(charter.get("max_tasks_per_cycle") or 3)
    for g in goals:
        if len(picked) >= max_n:
            break
        when = str(g.get("when") or "false")
        if eval_when(when, env):
            # health_hold only if nothing else picked
            if g.get("id") == "health_hold" and picked:
                continue
            picked.append(g)
            # keep_teacher / drain can pair with peft; hold is exclusive-ish
            if g.get("id") == "health_hold":
                break
    if not picked:
        # force hold
        for g in goals:
            if g.get("id") == "health_hold":
                return [g]
    return picked


# --- actions (muscles) -------------------------------------------------------

def act_ensure_bonsai(dry: bool) -> dict[str, Any]:
    if dry:
        return {"action": "ensure_bonsai", "ok": True, "dry": True}
    subprocess.run(["systemctl", "--user", "start", "bonsai-server.service"], check=False)
    time.sleep(1)
    ok = _http_ok(HTTP)
    return {"action": "ensure_bonsai", "ok": bool(ok)}


def act_ensure_lane(dry: bool) -> dict[str, Any]:
    if dry:
        return {"action": "ensure_lane", "ok": True, "dry": True}
    subprocess.run(
        ["systemctl", "--user", "start", "cnet-personal-ai-lane.service"], check=False
    )
    time.sleep(1)
    return {"action": "ensure_lane", "ok": bool(_active("cnet-personal-ai-lane.service"))}


def act_inject_window_gaps(dry: bool) -> dict[str, Any]:
    if not WINDOW.exists():
        return {"action": "inject_window_gaps", "ok": False, "err": "no window"}
    ids = [int(x) for x in WINDOW.read_text().split() if x.strip().isdigit()]
    if not ids:
        return {"action": "inject_window_gaps", "ok": False, "err": "empty window"}
    import random

    k = int(os.environ.get("CNET_CURIOSITY_K", "3"))
    W = len(ids)
    n = min(INJECT_N, len(ids))
    random.seed(int(time.time()) // 600)
    picks = random.sample(ids, n)
    lines = [f"NO_PLAN 1 {W} 1 w_cur 1 {W} {k} tk{tid}q{tid}\n" for tid in picks]
    if dry:
        return {"action": "inject_window_gaps", "ok": True, "n": n, "dry": True}
    INBOX.parent.mkdir(parents=True, exist_ok=True)
    with INBOX.open("a") as f:
        f.writelines(lines)
    return {"action": "inject_window_gaps", "ok": True, "n": n, "inbox": str(INBOX)}


def act_nudge_lane_tick(dry: bool) -> dict[str, Any]:
    # Lane is interval-driven; restarting is heavy. Touch inbox is enough.
    # Optionally send no-op: systemctl kill -s USR1 not wired.
    return {"action": "nudge_lane_tick", "ok": True, "note": "inbox feed; lane polls 60s"}


def act_run_cert_learn(dry: bool, state: dict[str, Any]) -> dict[str, Any]:
    binp = REPO / "bin/cnet_cert_learn_tick"
    if dry:
        return {"action": "run_cert_learn", "ok": binp.exists(), "dry": True}
    if not binp.exists():
        return {"action": "run_cert_learn", "ok": False, "err": "missing binary"}
    env = os.environ.copy()
    env.update(
        {
            "CNET_BASE_PATH": str(BASE),
            "CNET_FAULT_LOG": str(FAULT),
            "CNET_LORA_STORE_DIR": str(LORA_DIR),
            "CNET_FAULT_DEDUPE": "0",
            "CNET_RESIDUAL_HTTP": HTTP,
            "CNET_RESIDUAL_WINDOW": str(WINDOW),
            "CNET_SOUL_RESIDUAL_PREFER_HERMETIC": "0",
        }
    )
    try:
        r = subprocess.run(
            [str(binp)],
            cwd=str(REPO),
            env=env,
            capture_output=True,
            text=True,
            timeout=600,
        )
        ok = r.returncode == 0 and "CERT_LEARN_TICK_DONE" in (r.stdout + r.stderr)
        if ok:
            state["last_peft_unix"] = time.time()
        return {
            "action": "run_cert_learn",
            "ok": ok,
            "rc": r.returncode,
            "tail": (r.stdout + r.stderr)[-500:],
        }
    except Exception as e:
        return {"action": "run_cert_learn", "ok": False, "err": str(e)}


def act_run_structure_mine(dry: bool, state: dict[str, Any]) -> dict[str, Any]:
    binp = REPO / "bin/struct_mine_persist"
    if dry:
        return {"action": "run_structure_mine", "ok": binp.exists(), "dry": True}
    if not binp.exists():
        return {"action": "run_structure_mine", "ok": False, "err": "missing binary"}
    env = os.environ.copy()
    env.update(
        {
            "CNET_BASE_PATH": str(BASE),
            "CNET_RESIDUAL_HTTP": HTTP,
            "CNET_RESIDUAL_WINDOW": str(WINDOW),
            "CNET_STRUCTURE_EXPAND_N": "32",
            "CNET_SOUL_RESIDUAL_PREFER_HERMETIC": "0",
        }
    )
    try:
        r = subprocess.run(
            [str(binp)],
            cwd=str(REPO),
            env=env,
            capture_output=True,
            text=True,
            timeout=900,
        )
        ok = r.returncode == 0 and "STRUCT_MINE_PERSIST_PASS" in (r.stdout + r.stderr)
        if ok or r.returncode == 0:
            state["last_mine_unix"] = time.time()
        return {
            "action": "run_structure_mine",
            "ok": ok or r.returncode == 0,
            "rc": r.returncode,
            "tail": (r.stdout + r.stderr)[-500:],
        }
    except Exception as e:
        return {"action": "run_structure_mine", "ok": False, "err": str(e)}


def act_seed_procedures(dry: bool, state: dict[str, Any]) -> dict[str, Any]:
    script = REPO / "scripts/procedure_chunk_seal.sh"
    if dry:
        return {"action": "seed_procedures", "ok": script.exists(), "dry": True}
    if not script.exists():
        return {"action": "seed_procedures", "ok": False, "err": "missing script"}
    r = subprocess.run(
        ["bash", str(script)], cwd=str(REPO), capture_output=True, text=True, timeout=60
    )
    if r.returncode == 0:
        state["last_procedure_unix"] = time.time()
    return {
        "action": "seed_procedures",
        "ok": r.returncode == 0,
        "out": (r.stdout or "").strip()[-200:],
    }


def act_ensure_curiosity(dry: bool) -> dict[str, Any]:
    # Curiosity is env on lane drop-in; ensure lane running is enough.
    return {"action": "ensure_curiosity", "ok": True, "note": "lane drop-in CNET_CURIOSITY=1"}


def act_log_hold(dry: bool) -> dict[str, Any]:
    return {"action": "log_hold", "ok": True, "dry": dry}


ACTIONS = {
    "ensure_bonsai": lambda dry, st: act_ensure_bonsai(dry),
    "ensure_lane": lambda dry, st: act_ensure_lane(dry),
    "inject_window_gaps": lambda dry, st: act_inject_window_gaps(dry),
    "nudge_lane_tick": lambda dry, st: act_nudge_lane_tick(dry),
    "run_cert_learn": lambda dry, st: act_run_cert_learn(dry, st),
    "run_structure_mine": lambda dry, st: act_run_structure_mine(dry, st),
    "seed_procedures": lambda dry, st: act_seed_procedures(dry, st),
    "ensure_curiosity": lambda dry, st: act_ensure_curiosity(dry),
    "log_hold": lambda dry, st: act_log_hold(dry),
}


def load_state() -> dict[str, Any]:
    if STATE_PATH.exists():
        try:
            return json.loads(STATE_PATH.read_text())
        except Exception:
            pass
    return {}


def save_state(st: dict[str, Any]) -> None:
    STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
    STATE_PATH.write_text(json.dumps(st, indent=2) + "\n")


def run_cycle(dry: bool = False) -> dict[str, Any]:
    charter = parse_charter(CHARTER_PATH.read_text())
    state = load_state()
    sb = collect_scoreboard(state)
    SCORE_PATH.parent.mkdir(parents=True, exist_ok=True)
    SCORE_PATH.write_text(json.dumps(asdict(sb), indent=2) + "\n")

    picked = pick_goals(charter, sb)
    results: list[dict[str, Any]] = []
    seen_actions: set[str] = set()
    for g in picked:
        for aname in g.get("actions") or []:
            if aname in seen_actions:
                continue
            seen_actions.add(aname)
            fn = ACTIONS.get(aname)
            if not fn:
                results.append({"action": aname, "ok": False, "err": "unknown"})
                continue
            results.append(fn(dry, state))

    decision = {
        "ts": datetime.now().astimezone().isoformat(timespec="seconds"),
        "dry_run": dry,
        "goals": [{"id": g.get("id"), "priority": g.get("priority")} for g in picked],
        "actions": results,
        "scoreboard": asdict(sb),
        "charter": str(CHARTER_PATH),
    }
    DECISION_PATH.parent.mkdir(parents=True, exist_ok=True)
    DECISION_PATH.write_text(json.dumps(decision, indent=2) + "\n")
    if not dry:
        state["last_cycle_unix"] = time.time()
        state["last_goals"] = [g.get("id") for g in picked]
        save_state(state)
    return decision


def self_test() -> int:
    """Hermetic policy tests (no systemd required for logic)."""
    fails = 0
    sample = """
version: 1
max_tasks_per_cycle: 2
budget_sec: 60
goals:
  - id: keep_teacher_alive
    priority: 0
    description: up
    when: "bonsai_ok == 0"
    actions: [ensure_bonsai]
  - id: drain_open_gaps
    priority: 1
    description: gaps
    when: "open_gaps + deferred_oracle >= 8"
    actions: [inject_window_gaps]
  - id: health_hold
    priority: 9
    description: hold
    when: "true"
    actions: [log_hold]
"""
    ch = parse_charter(sample)
    assert ch["max_tasks_per_cycle"] == 2, ch
    assert len(ch["goals"]) == 3, ch

    sb = Scoreboard(ts="t", bonsai_ok=0, lane_ok=1, open_gaps=0, deferred_oracle=0)
    p = pick_goals(ch, sb)
    assert p[0]["id"] == "keep_teacher_alive", p

    sb2 = Scoreboard(ts="t", bonsai_ok=1, lane_ok=1, open_gaps=10, deferred_oracle=0)
    p2 = pick_goals(ch, sb2)
    assert p2[0]["id"] == "drain_open_gaps", p2

    sb3 = Scoreboard(ts="t", bonsai_ok=1, lane_ok=1, open_gaps=0, deferred_oracle=0)
    p3 = pick_goals(ch, sb3)
    assert p3[0]["id"] == "health_hold", p3

    assert eval_when("fault_lines >= 64", {"fault_lines": 100}) is True
    assert eval_when("fault_lines >= 64", {"fault_lines": 3}) is False
    assert eval_when("true", {}) is True

    # dry-run cycle against live paths if present
    if CHARTER_PATH.exists():
        d = run_cycle(dry=True)
        assert "goals" in d and "scoreboard" in d, d
        print("dry_cycle_goals", [g["id"] for g in d["goals"]])

    print("GOVERNOR_SELFTEST_PASS checks=6")
    return fails


def main() -> int:
    ap = argparse.ArgumentParser(description="CNET self-direction governor")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--test", action="store_true")
    args = ap.parse_args()
    if args.test:
        return self_test()
    if not CHARTER_PATH.exists():
        print(f"missing charter {CHARTER_PATH}", file=sys.stderr)
        return 2
    d = run_cycle(dry=args.dry_run)
    print("GOVERNOR_CYCLE_OK", json.dumps({"goals": d["goals"], "actions": [a.get("action") for a in d["actions"]]}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
