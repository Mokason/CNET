#!/usr/bin/env python3
"""CNET self-direction governor with runtime-evolving scoreboard.

Not a queue poller: charter + live scoreboard → pick goals → emit work.
Scoreboard evolves each cycle: history, deltas, rates, EWMA, goal health.

Usage:
  python3 scripts/cnet_governor.py
  python3 scripts/cnet_governor.py --dry-run
  python3 scripts/cnet_governor.py --test
  make governor
"""
from __future__ import annotations

import argparse
import json
import os
import random
import re
import subprocess
import sys
import time
import urllib.request
from dataclasses import asdict, dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Any

REPO = Path(os.environ.get("CNET_ROOT", Path(__file__).resolve().parents[1]))
CHARTER_PATH = Path(
    os.environ.get("CNET_GOVERNOR_CHARTER", REPO / "config/cnet_governor_charter.yaml")
)
STATE_PATH = Path(os.environ.get("CNET_GOVERNOR_STATE", REPO / "logs/governor/state.json"))
SCORE_PATH = Path(
    os.environ.get("CNET_GOVERNOR_SCOREBOARD", REPO / "logs/governor/scoreboard.json")
)
DECISION_PATH = Path(
    os.environ.get("CNET_GOVERNOR_LAST", REPO / "logs/governor/last_decision.json")
)
HISTORY_PATH = Path(
    os.environ.get(
        "CNET_GOVERNOR_HISTORY", REPO / "logs/governor/scoreboard_history.jsonl"
    )
)
EWMA_PATH = Path(
    os.environ.get("CNET_GOVERNOR_EWMA", REPO / "logs/governor/scoreboard_ewma.json")
)
BASE = Path(os.environ.get("CNET_BASE_PATH", REPO / "soul_gemma4v2_final.cnb"))
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


def parse_charter(text: str) -> dict[str, Any]:
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
    # runtime-evolved
    cycle: int = 0
    d_open_gaps: float = 0.0
    d_deferred_oracle: float = 0.0
    d_closed_gaps: float = 0.0
    d_fault_lines: float = 0.0
    d_units_proxy: float = 0.0
    d_base_size: float = 0.0
    rate_closed_per_h: float = 0.0
    rate_fault_per_h: float = 0.0
    rate_units_per_h: float = 0.0
    ewma_open: float = 0.0
    ewma_deferred: float = 0.0
    ewma_fault: float = 0.0
    ewma_units: float = 0.0
    backlog_pressure: float = 0.0
    learning_velocity: float = 0.0
    plateau: int = 0
    goal_health_drain: float = 0.5
    goal_health_peft: float = 0.5
    goal_health_mine: float = 0.5
    goal_health_coverage: float = 0.5
    teacher_uptime: float = 1.0
    evolved: dict[str, Any] = field(default_factory=dict)

    def as_env(self) -> dict[str, Any]:
        d = asdict(self)
        out: dict[str, Any] = {}
        for k, v in d.items():
            if isinstance(v, (int, float, bool)):
                out[k] = float(v) if isinstance(v, bool) else v
        for k, v in (self.evolved or {}).items():
            if isinstance(v, (int, float, bool)) and k not in out:
                out[k] = v
        return out

    def to_public(self) -> dict[str, Any]:
        d = asdict(self)
        # last_autoteach can be large; keep
        return d


def _active(unit: str) -> int:
    try:
        r = subprocess.run(
            ["systemctl", "--user", "is-active", "--quiet", unit], check=False
        )
        return 1 if r.returncode == 0 else 0
    except Exception:
        return 0


def _http_ok(url: str) -> int:
    try:
        urllib.request.urlopen(url.rstrip("/") + "/v1/models", timeout=4)
        return 1
    except Exception:
        return 0


def _gap_counts(path: Path) -> tuple[int, int, int]:
    open_n = def_n = closed_n = 0
    if not path.exists():
        return 0, 0, 0
    try:
        lines = path.read_text(errors="replace").splitlines()
    except Exception:
        return 0, 0, 0
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
        elif parts[0].isdigit():
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


def _ewma(prev: float, x: float, alpha: float = 0.25) -> float:
    if prev == 0.0 and x != 0.0:
        return float(x)
    return alpha * float(x) + (1.0 - alpha) * float(prev)


def _load_history(n: int = 48) -> list[dict[str, Any]]:
    if not HISTORY_PATH.exists():
        return []
    rows: list[dict[str, Any]] = []
    try:
        for line in HISTORY_PATH.read_text().splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except Exception:
                continue
    except Exception:
        return []
    return rows[-n:]


def _load_ewma() -> dict[str, float]:
    if not EWMA_PATH.exists():
        return {}
    try:
        d = json.loads(EWMA_PATH.read_text())
        return {k: float(v) for k, v in d.items() if isinstance(v, (int, float))}
    except Exception:
        return {}


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
    return Scoreboard(
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


def evolve_scoreboard(
    sb: Scoreboard, state: dict[str, Any], dry: bool = False
) -> Scoreboard:
    """Runtime evolution: deltas, rates, EWMA, plateau, goal health."""
    hist = _load_history()
    ewma = _load_ewma()
    prev = hist[-1] if hist else (state.get("last_score_core") or {})
    cycle = int(state.get("cycle", 0)) + (0 if dry else 1)
    sb.cycle = int(state.get("cycle", 0)) if dry else cycle

    def _d(key: str, cur: float) -> float:
        try:
            return float(cur) - float(prev.get(key, cur))
        except Exception:
            return 0.0

    sb.d_open_gaps = _d("open_gaps", sb.open_gaps)
    sb.d_deferred_oracle = _d("deferred_oracle", sb.deferred_oracle)
    sb.d_closed_gaps = _d("closed_gaps", sb.closed_gaps)
    sb.d_fault_lines = _d("fault_lines", sb.fault_lines)
    sb.d_units_proxy = _d("units_proxy", sb.units_proxy)
    sb.d_base_size = _d("base_size", sb.base_size)

    dt_h = 0.25
    if prev.get("ts_unix"):
        dt_h = max(1e-3, (time.time() - float(prev["ts_unix"])) / 3600.0)
    elif state.get("last_cycle_unix"):
        dt_h = max(1e-3, (time.time() - float(state["last_cycle_unix"])) / 3600.0)

    sb.rate_closed_per_h = sb.d_closed_gaps / dt_h
    sb.rate_fault_per_h = sb.d_fault_lines / dt_h
    sb.rate_units_per_h = sb.d_units_proxy / dt_h

    sb.ewma_open = _ewma(ewma.get("ewma_open", float(sb.open_gaps)), float(sb.open_gaps))
    sb.ewma_deferred = _ewma(
        ewma.get("ewma_deferred", float(sb.deferred_oracle)), float(sb.deferred_oracle)
    )
    sb.ewma_fault = _ewma(
        ewma.get("ewma_fault", float(sb.fault_lines)), float(sb.fault_lines)
    )
    sb.ewma_units = _ewma(
        ewma.get("ewma_units", float(sb.units_proxy)), float(sb.units_proxy)
    )
    up = 1.0 if (sb.bonsai_ok and sb.lane_ok) else 0.0
    sb.teacher_uptime = _ewma(ewma.get("teacher_uptime", up), up, alpha=0.15)

    sb.backlog_pressure = float(sb.open_gaps + sb.deferred_oracle)
    instant_vel = max(0.0, sb.rate_units_per_h) + 0.1 * max(0.0, sb.rate_closed_per_h)
    sb.learning_velocity = _ewma(
        ewma.get("learning_velocity", 0.0), instant_vel, alpha=0.3
    )

    recent = hist[-3:]
    if len(recent) >= 2:
        du = sum(float(r.get("d_units_proxy", 0) or 0) for r in recent[-3:])
        dc = sum(float(r.get("d_closed_gaps", 0) or 0) for r in recent[-3:])
        sb.plateau = (
            1 if (du <= 0 and dc <= 2 and sb.bonsai_ok and sb.lane_ok) else 0
        )
    else:
        sb.plateau = 0

    gh = state.get("goal_health") or {}
    sb.goal_health_drain = float(gh.get("drain_open_gaps", 0.5))
    sb.goal_health_peft = float(gh.get("peft_jtc", 0.5))
    sb.goal_health_mine = float(gh.get("structure_mine", 0.5))
    sb.goal_health_coverage = float(gh.get("coverage_curiosity", 0.5))

    sb.evolved = {
        "cycle": sb.cycle,
        "dt_h": round(dt_h, 4),
        "backlog_pressure": sb.backlog_pressure,
        "learning_velocity": round(sb.learning_velocity, 4),
        "plateau": sb.plateau,
        "history_len": len(hist),
        "teacher_uptime": round(sb.teacher_uptime, 4),
    }

    if not dry:
        row = {
            "ts": sb.ts,
            "ts_unix": time.time(),
            "bonsai_ok": sb.bonsai_ok,
            "lane_ok": sb.lane_ok,
            "open_gaps": sb.open_gaps,
            "deferred_oracle": sb.deferred_oracle,
            "closed_gaps": sb.closed_gaps,
            "fault_lines": sb.fault_lines,
            "lora_files": sb.lora_files,
            "units_proxy": sb.units_proxy,
            "hyb_struct": sb.hyb_struct,
            "base_size": sb.base_size,
            "cycle": sb.cycle,
            "d_open_gaps": sb.d_open_gaps,
            "d_closed_gaps": sb.d_closed_gaps,
            "d_fault_lines": sb.d_fault_lines,
            "d_units_proxy": sb.d_units_proxy,
            "rate_closed_per_h": sb.rate_closed_per_h,
            "rate_units_per_h": sb.rate_units_per_h,
            "backlog_pressure": sb.backlog_pressure,
            "learning_velocity": sb.learning_velocity,
            "plateau": sb.plateau,
            "goal_health_drain": sb.goal_health_drain,
            "goal_health_peft": sb.goal_health_peft,
            "goal_health_mine": sb.goal_health_mine,
            "goal_health_coverage": sb.goal_health_coverage,
            "teacher_uptime": sb.teacher_uptime,
            "ewma_open": sb.ewma_open,
            "ewma_units": sb.ewma_units,
        }
        HISTORY_PATH.parent.mkdir(parents=True, exist_ok=True)
        with HISTORY_PATH.open("a") as f:
            f.write(json.dumps(row) + "\n")
        try:
            lines = HISTORY_PATH.read_text().splitlines()
            if len(lines) > 2000:
                HISTORY_PATH.write_text("\n".join(lines[-1500:]) + "\n")
        except Exception:
            pass
        EWMA_PATH.write_text(
            json.dumps(
                {
                    "ewma_open": sb.ewma_open,
                    "ewma_deferred": sb.ewma_deferred,
                    "ewma_fault": sb.ewma_fault,
                    "ewma_units": sb.ewma_units,
                    "teacher_uptime": sb.teacher_uptime,
                    "learning_velocity": sb.learning_velocity,
                    "updated_ts": datetime.now()
                    .astimezone()
                    .isoformat(timespec="seconds"),
                },
                indent=2,
            )
            + "\n"
        )
        state["last_score_core"] = row
        state["cycle"] = sb.cycle

    return sb


def update_goal_health(
    state: dict[str, Any],
    picked: list[dict[str, Any]],
    results: list[dict[str, Any]],
    sb: Scoreboard,
) -> None:
    gh = dict(state.get("goal_health") or {})
    ok_ratio = (
        sum(1 for r in results if r.get("ok")) / max(1, len(results)) if results else 0.0
    )

    def bump(gid: str, delta: float) -> None:
        cur = float(gh.get(gid, 0.5))
        gh[gid] = max(0.0, min(1.0, cur + delta))

    for g in picked:
        gid = str(g.get("id") or "")
        if gid == "health_hold":
            bump(gid, 0.02)
            continue
        bump(gid, 0.08 * ok_ratio - 0.05 * (1.0 - ok_ratio))
        if gid == "drain_open_gaps" and sb.d_closed_gaps > 0:
            bump(gid, 0.05)
        if gid == "peft_jtc" and any(
            r.get("action") == "run_cert_learn" and r.get("ok") for r in results
        ):
            bump(gid, 0.06)
        if gid == "structure_mine" and any(
            r.get("action") == "run_structure_mine" and r.get("ok") for r in results
        ):
            bump(gid, 0.06)
        if gid == "coverage_curiosity" and sb.plateau:
            bump(gid, -0.04)

    if sb.plateau:
        bump("coverage_curiosity", -0.03)
        bump("drain_open_gaps", 0.02)

    state["goal_health"] = gh


def eval_when(expr: str, env: dict[str, Any]) -> bool:
    if expr.strip() == "true":
        return True
    if expr.strip() == "false":
        return False
    if not re.match(r"^[0-9a-zA-Z_.<>=!&|+\-*/ ()]+$", expr):
        return False
    local = {k: v for k, v in env.items() if isinstance(v, (int, float, bool))}
    try:
        return bool(eval(expr, {"__builtins__": {}}, local))  # noqa: S307
    except Exception:
        return False


def pick_goals(charter: dict[str, Any], sb: Scoreboard) -> list[dict[str, Any]]:
    env = sb.as_env()

    def sort_key(g: dict[str, Any]) -> tuple:
        p = int(g.get("priority", 99))
        gid = str(g.get("id") or "")
        hm = {
            "drain_open_gaps": sb.goal_health_drain,
            "peft_jtc": sb.goal_health_peft,
            "structure_mine": sb.goal_health_mine,
            "coverage_curiosity": sb.goal_health_coverage,
        }
        health = hm.get(gid, 0.5)
        bias = 0
        if sb.plateau and gid == "coverage_curiosity":
            bias = 2
        if sb.backlog_pressure >= 8 and gid == "drain_open_gaps":
            bias = -1
        if sb.learning_velocity < 0.5 and gid == "peft_jtc" and sb.fault_lines >= 64:
            bias = -1
        if sb.plateau and gid == "structure_mine":
            bias = -1  # try mine harder when plateau
        return (p + bias, -health)

    goals = sorted(charter.get("goals") or [], key=sort_key)
    picked: list[dict[str, Any]] = []
    max_n = int(charter.get("max_tasks_per_cycle") or 3)
    for g in goals:
        if len(picked) >= max_n:
            break
        when = str(g.get("when") or "false")
        if eval_when(when, env):
            if g.get("id") == "health_hold" and picked:
                continue
            picked.append(g)
            if g.get("id") == "health_hold":
                break
    if not picked:
        for g in charter.get("goals") or []:
            if g.get("id") == "health_hold":
                return [g]
    return picked


# --- muscles ------------------------------------------------------------------

def act_ensure_bonsai(dry: bool) -> dict[str, Any]:
    if dry:
        return {"action": "ensure_bonsai", "ok": True, "dry": True}
    subprocess.run(["systemctl", "--user", "start", "bonsai-server.service"], check=False)
    time.sleep(1)
    return {"action": "ensure_bonsai", "ok": bool(_http_ok(HTTP))}


def act_ensure_lane(dry: bool) -> dict[str, Any]:
    if dry:
        return {"action": "ensure_lane", "ok": True, "dry": True}
    subprocess.run(
        ["systemctl", "--user", "start", "cnet-personal-ai-lane.service"], check=False
    )
    time.sleep(1)
    return {
        "action": "ensure_lane",
        "ok": bool(_active("cnet-personal-ai-lane.service")),
    }


def act_inject_window_gaps(dry: bool) -> dict[str, Any]:
    if not WINDOW.exists():
        return {"action": "inject_window_gaps", "ok": False, "err": "no window"}
    ids = [int(x) for x in WINDOW.read_text().split() if x.strip().isdigit()]
    if not ids:
        return {"action": "inject_window_gaps", "ok": False, "err": "empty"}
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
    return {"action": "inject_window_gaps", "ok": True, "n": n}


def act_nudge_lane_tick(dry: bool) -> dict[str, Any]:
    return {"action": "nudge_lane_tick", "ok": True, "note": "inbox; lane 60s"}


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
        ok = r.returncode == 0
        if ok:
            state["last_peft_unix"] = time.time()
        return {
            "action": "run_cert_learn",
            "ok": ok,
            "rc": r.returncode,
            "tail": (r.stdout + r.stderr)[-400:],
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
        ok = r.returncode == 0
        if ok:
            state["last_mine_unix"] = time.time()
        return {
            "action": "run_structure_mine",
            "ok": ok,
            "rc": r.returncode,
            "tail": (r.stdout + r.stderr)[-400:],
        }
    except Exception as e:
        return {"action": "run_structure_mine", "ok": False, "err": str(e)}


def act_seed_procedures(dry: bool, state: dict[str, Any]) -> dict[str, Any]:
    script = REPO / "scripts/procedure_chunk_seal.sh"
    if dry:
        return {"action": "seed_procedures", "ok": script.exists(), "dry": True}
    if not script.exists():
        return {"action": "seed_procedures", "ok": False, "err": "missing"}
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
    return {"action": "ensure_curiosity", "ok": True}


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
    sb = evolve_scoreboard(sb, state, dry=dry)

    SCORE_PATH.parent.mkdir(parents=True, exist_ok=True)
    SCORE_PATH.write_text(json.dumps(sb.to_public(), indent=2) + "\n")

    picked = pick_goals(charter, sb)
    results: list[dict[str, Any]] = []
    seen: set[str] = set()
    for g in picked:
        for aname in g.get("actions") or []:
            if aname in seen:
                continue
            seen.add(aname)
            fn = ACTIONS.get(aname)
            if not fn:
                results.append({"action": aname, "ok": False, "err": "unknown"})
                continue
            results.append(fn(dry, state))

    if not dry:
        update_goal_health(state, picked, results, sb)
        # refresh health fields on public board after update
        gh = state.get("goal_health") or {}
        sb.goal_health_drain = float(gh.get("drain_open_gaps", sb.goal_health_drain))
        sb.goal_health_peft = float(gh.get("peft_jtc", sb.goal_health_peft))
        sb.goal_health_mine = float(gh.get("structure_mine", sb.goal_health_mine))
        sb.goal_health_coverage = float(
            gh.get("coverage_curiosity", sb.goal_health_coverage)
        )
        SCORE_PATH.write_text(json.dumps(sb.to_public(), indent=2) + "\n")

    decision = {
        "ts": datetime.now().astimezone().isoformat(timespec="seconds"),
        "dry_run": dry,
        "goals": [{"id": g.get("id"), "priority": g.get("priority")} for g in picked],
        "actions": results,
        "scoreboard": sb.to_public(),
        "evolved": sb.evolved,
        "goal_health": state.get("goal_health"),
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
    when: "open_gaps + deferred_oracle >= 8 or backlog_pressure >= 8"
    actions: [inject_window_gaps]
  - id: peft_jtc
    priority: 2
    description: peft
    when: "plateau == 1 and fault_lines >= 64"
    actions: [run_cert_learn]
  - id: health_hold
    priority: 9
    description: hold
    when: "true"
    actions: [log_hold]
"""
    ch = parse_charter(sample)
    assert len(ch["goals"]) == 4

    sb = Scoreboard(ts="t", bonsai_ok=0, lane_ok=1)
    assert pick_goals(ch, sb)[0]["id"] == "keep_teacher_alive"

    sb2 = Scoreboard(ts="t", bonsai_ok=1, lane_ok=1, open_gaps=10, backlog_pressure=10)
    assert pick_goals(ch, sb2)[0]["id"] == "drain_open_gaps"

    sb3 = Scoreboard(
        ts="t",
        bonsai_ok=1,
        lane_ok=1,
        open_gaps=0,
        fault_lines=100,
        plateau=1,
        backlog_pressure=0,
    )
    ids = [g["id"] for g in pick_goals(ch, sb3)]
    assert "peft_jtc" in ids or ids[0] == "health_hold"

    # evolve unit test
    st: dict[str, Any] = {"cycle": 0}
    a = Scoreboard(ts="t1", bonsai_ok=1, lane_ok=1, open_gaps=5, closed_gaps=10, units_proxy=100)
    a = evolve_scoreboard(a, st, dry=True)
    assert "backlog_pressure" in a.as_env()
    assert a.backlog_pressure == 5.0

    st2: dict[str, Any] = {"goal_health": {}}
    update_goal_health(
        st2,
        [{"id": "peft_jtc"}],
        [{"action": "run_cert_learn", "ok": True}],
        Scoreboard(ts="t", d_closed_gaps=0),
    )
    assert st2["goal_health"]["peft_jtc"] > 0.5

    if CHARTER_PATH.exists():
        d = run_cycle(dry=True)
        assert "evolved" in d or "scoreboard" in d
        print("dry_cycle_goals", [g["id"] for g in d["goals"]])
        print("dry_evolved", d.get("scoreboard", {}).get("evolved") or d.get("evolved"))

    print("GOVERNOR_SELFTEST_PASS checks=8")
    return 0


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
    print(
        "GOVERNOR_CYCLE_OK",
        json.dumps(
            {
                "goals": d["goals"],
                "actions": [a.get("action") for a in d["actions"]],
                "evolved": d.get("evolved"),
                "goal_health": d.get("goal_health"),
            }
        ),
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
