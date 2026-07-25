#!/usr/bin/env python3
"""CNET autonomous governor v3 — single engine, self-evolving.

Fixes weak v2 points:
- Hermes/agent error fuel (not only window gaps)
- Projects drive ranking hard
- Eval veto freezes seals on regression
- Outcome credit with min dt (anti flashy velocity)
- Self-evolution of meta weights/thresholds from history
- One entrypoint (this file); C binary optional legacy
"""
from __future__ import annotations

import json
import os
import random
import re
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Any

ROOT = Path(os.environ.get("CNET_ROOT", Path(__file__).resolve().parents[1]))
GOV = Path(os.environ.get("CNET_GOVERNOR_DIR", ROOT / "logs/governor"))
BASE = Path(os.environ.get("CNET_BASE_PATH", ROOT / "soul_gemma4v2_final.cnb"))
CHARTER = Path(
    os.environ.get("CNET_GOVERNOR_CHARTER", ROOT / "config/cnet_governor_charter.yaml")
)
PINS = ROOT / "config/governor_pins.yaml"
PROJECTS = ROOT / "config/governor_projects.json"
META_PATH = GOV / "meta_evolved.json"
HTTP = os.environ.get("CNET_RESIDUAL_HTTP", "http://127.0.0.1:8080")
HERMES_ERR = Path(
    os.environ.get("HERMES_ERRORS_LOG", Path.home() / ".hermes/logs/errors.log")
)
AGENT_LOG = Path(
    os.environ.get("HERMES_AGENT_LOG", Path.home() / ".hermes/logs/agent.log")
)

# Defaults (self-evolution mutates a copy in META_PATH)
META_DEFAULTS = {
    "version": 3,
    "w_backlog": 2.5,
    "w_real_miss": 3.0,
    "w_hermes_err": 2.5,
    "w_eval": 3.0,
    "w_velocity": 1.0,
    "w_web": 0.8,
    "w_procedure": 1.2,
    "threshold_backlog": 8.0,
    "threshold_miss": 0.15,
    "threshold_eval_good": 0.40,
    "threshold_eval_veto": 0.05,  # drop vs previous → veto seals
    "min_dt_h": 0.05,  # ignore velocity if cycle faster than 3 min
    "inject_n": 4,
    "max_web_notes": 80,
    "evolve_rate": 0.08,
}


def sh(cmd: str, timeout: int = 600) -> subprocess.CompletedProcess:
    return subprocess.run(
        cmd,
        shell=True,
        cwd=str(ROOT),
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def active(unit: str) -> bool:
    return sh(f"systemctl --user is-active --quiet {unit}").returncode == 0


def load_json(p: Path | str, default: Any = None) -> Any:
    try:
        return json.loads(Path(p).read_text())
    except Exception:
        return default if default is not None else {}


def save_json(p: Path, obj: Any) -> None:
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(json.dumps(obj, indent=2) + "\n")


def load_meta() -> dict[str, Any]:
    m = dict(META_DEFAULTS)
    m.update(load_json(META_PATH, {}))
    return m


def parse_pins() -> dict[str, Any]:
    pins = {
        "force_goal": "",
        "freeze_seals": 0,
        "pause_inject": 0,
        "pause_web": 0,
        "max_tasks_override": 0,
        "eval_veto_override": 0,  # 1 = ignore eval veto
    }
    if not PINS.exists():
        return pins
    for line in PINS.read_text().splitlines():
        line = line.split("#")[0].strip()
        if ":" not in line:
            continue
        k, v = line.split(":", 1)
        k, v = k.strip(), v.strip()
        if k not in pins:
            continue
        pins[k] = int(v) if v.isdigit() else v
    return pins


def parse_charter() -> dict[str, Any]:
    goals: list[dict] = []
    cur = None
    max_tasks = 3
    for raw in CHARTER.read_text().splitlines():
        line = raw.split("#")[0].rstrip()
        if not line.strip():
            continue
        if line.startswith("max_tasks_per_cycle:"):
            max_tasks = int(line.split(":", 1)[1])
        elif re.match(r"\s*-\s*id:", line):
            if cur:
                goals.append(cur)
            cur = {
                "id": line.split("id:", 1)[1].strip(),
                "priority": 99,
                "when": "false",
                "actions": [],
            }
        elif cur is not None:
            m = re.match(r"\s+(priority|when|actions):\s*(.*)$", line)
            if not m:
                continue
            k, v = m.group(1), m.group(2).strip()
            if k == "priority":
                cur[k] = int(v)
            elif k == "actions":
                inner = v.strip("[]")
                cur[k] = [a.strip() for a in inner.split(",") if a.strip()]
            else:
                cur[k] = v.strip().strip('"')
    if cur:
        goals.append(cur)
    return {"max_tasks": max_tasks, "goals": goals}


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


def count_hermes_errors(hours: float = 6.0) -> dict[str, Any]:
    """Parse recent Hermes error/agent logs for real-task failure fuel."""
    cutoff = time.time() - hours * 3600
    n = 0
    kinds: dict[str, int] = {}
    samples: list[str] = []

    def eat(path: Path) -> None:
        nonlocal n
        if not path.exists():
            return
        try:
            # read tail only
            data = path.read_bytes()
            if len(data) > 2_000_000:
                data = data[-2_000_000:]
            text = data.decode("utf-8", errors="replace")
        except Exception:
            return
        for line in text.splitlines()[-2000:]:
            low = line.lower()
            if not any(
                x in low
                for x in (
                    "error",
                    "exception",
                    "traceback",
                    "failed",
                    "timeout",
                    "refused",
                )
            ):
                continue
            # crude time filter if ISO present
            n += 1
            if "timeout" in low:
                kinds["timeout"] = kinds.get("timeout", 0) + 1
            elif "tool" in low:
                kinds["tool"] = kinds.get("tool", 0) + 1
            elif "json" in low or "parse" in low:
                kinds["json"] = kinds.get("json", 0) + 1
            else:
                kinds["other"] = kinds.get("other", 0) + 1
            if len(samples) < 5:
                samples.append(line[:160])

    eat(HERMES_ERR)
    eat(AGENT_LOG)
    # also CNET acct errors if present
    acct = ROOT / "logs/cnet_acct.jsonl"
    if acct.exists():
        try:
            for line in acct.read_text(errors="replace").splitlines()[-500:]:
                if "error" in line.lower() or '"source": "error"' in line.lower():
                    n += 1
                    kinds["cnet_acct"] = kinds.get("cnet_acct", 0) + 1
        except Exception:
            pass

    rate = min(1.0, n / 50.0)  # 50 errors/6h → rate 1
    return {
        "hermes_err_n": n,
        "hermes_err_rate": round(rate, 4),
        "hermes_err_kinds": kinds,
        "hermes_err_samples": samples,
        "window_h": hours,
    }


def project_scores(sb: dict[str, Any], meta: dict[str, Any]) -> list[dict[str, Any]]:
    conf = load_json(PROJECTS, {"projects": []})
    out = []
    for p in conf.get("projects") or []:
        if not p.get("active", True):
            continue
        metric = p.get("metric")
        target = float(p.get("target") or 0)
        weight = float(p.get("weight") or 1)
        prefer_lower = bool(p.get("prefer_lower"))
        val = float(sb.get(metric) or 0)
        if prefer_lower:
            # higher score = more urgent when val >> target
            gap = max(0.0, val - target) / max(abs(target), 1e-6)
        else:
            gap = max(0.0, target - val) / max(abs(target), 1e-6)
        # map metric names to meta weights
        mw = meta.get("w_eval", 1) if "eval" in metric or "jtc" in metric else weight
        if metric == "backlog_pressure":
            mw = meta.get("w_backlog", weight)
        if metric == "real_miss_rate":
            mw = meta.get("w_real_miss", weight)
        if metric == "web_notes":
            mw = meta.get("w_web", weight)
        urgency = gap * float(mw)
        out.append(
            {
                "id": p.get("id"),
                "metric": metric,
                "value": val,
                "target": target,
                "urgency": round(urgency, 4),
                "prefer_lower": prefer_lower,
            }
        )
    out.sort(key=lambda x: -x["urgency"])
    return out


def self_evolve(meta: dict[str, Any], sb: dict[str, Any], state: dict[str, Any]) -> dict[str, Any]:
    """Adjust meta weights from recent outcomes — governor evolves its own policy."""
    rate = float(meta.get("evolve_rate") or 0.08)
    hist = []
    hp = GOV / "scoreboard_history.jsonl"
    if hp.exists():
        for line in hp.read_text().splitlines()[-12:]:
            try:
                hist.append(json.loads(line))
            except Exception:
                pass

    # If backlog not falling while drain often chosen → raise w_real_miss / lower inject bias
    drain_streak = int(state.get("drain_streak") or 0)
    if sb.get("d_backlog_pressure", 0) >= 0 and drain_streak >= 3:
        meta["w_real_miss"] = min(5.0, float(meta["w_real_miss"]) + rate)
        meta["threshold_backlog"] = max(4.0, float(meta["threshold_backlog"]) - 0.2)
        meta["inject_n"] = max(2, int(meta.get("inject_n", 4)) - 1)
        state["evolve_note"] = "backlog_stuck_raise_miss_weight"
    elif sb.get("d_backlog_pressure", 0) < -2:
        meta["w_backlog"] = max(1.0, float(meta["w_backlog"]) - rate * 0.5)
        meta["inject_n"] = min(6, int(meta.get("inject_n", 4)) + 0)
        state["evolve_note"] = "backlog_falling_ok"

    # Eval regression → stronger veto / peft weight
    d_eval = sb.get("d_eval_jtc")
    if d_eval is not None and float(d_eval) < -float(meta.get("threshold_eval_veto") or 0.05):
        meta["w_eval"] = min(5.0, float(meta["w_eval"]) + rate * 2)
        state["eval_veto"] = 1
        state["evolve_note"] = "eval_regression_veto"
    elif d_eval is not None and float(d_eval) > 0.02:
        meta["w_eval"] = max(1.5, float(meta["w_eval"]) - rate * 0.3)
        state["eval_veto"] = 0

    # Hermes errors high → boost hermes weight, prefer peft/jtc
    if float(sb.get("hermes_err_rate") or 0) > 0.3:
        meta["w_hermes_err"] = min(5.0, float(meta["w_hermes_err"]) + rate)
        state["evolve_note"] = "hermes_errors_up"
    else:
        meta["w_hermes_err"] = max(1.0, float(meta["w_hermes_err"]) - rate * 0.2)

    # Plateau long → boost velocity weight and break_plateau bias
    if int(sb.get("plateau") or 0) == 1:
        meta["w_velocity"] = min(3.0, float(meta["w_velocity"]) + rate)
    else:
        meta["w_velocity"] = max(0.5, float(meta["w_velocity"]) - rate * 0.2)

    # Web notes glut → lower web weight
    if int(sb.get("web_notes") or 0) > int(meta.get("max_web_notes") or 80) * 0.7:
        meta["w_web"] = max(0.2, float(meta["w_web"]) - rate)

    # Smooth history: if velocity noisy (tiny dt), increase min_dt_h
    if float(sb.get("dt_h") or 1) < 0.02:
        meta["min_dt_h"] = min(0.2, float(meta.get("min_dt_h") or 0.05) + 0.01)

    meta["updated_ts"] = datetime.now().astimezone().isoformat(timespec="seconds")
    meta["cycle"] = int(sb.get("cycle") or 0)
    save_json(META_PATH, meta)
    # also append evolution log
    with (GOV / "meta_history.jsonl").open("a") as f:
        f.write(
            json.dumps(
                {
                    "ts": meta["updated_ts"],
                    "note": state.get("evolve_note"),
                    "w_eval": meta["w_eval"],
                    "w_real_miss": meta["w_real_miss"],
                    "w_hermes_err": meta["w_hermes_err"],
                    "threshold_backlog": meta["threshold_backlog"],
                    "inject_n": meta["inject_n"],
                    "eval_veto": state.get("eval_veto", 0),
                }
            )
            + "\n"
        )
    return meta


def collect(state: dict, pins: dict, meta: dict) -> dict[str, Any]:
    sh("bash scripts/governor_hooks.sh pre", timeout=120)
    # enrich miss bus with hermes
    hermes = count_hermes_errors(6.0)
    save_json(GOV / "hermes_miss.json", hermes)
    # merge into miss_bus
    sh("bash scripts/governor_miss_ingest.sh", timeout=60)
    miss = load_json(GOV / "miss_bus.json", {})
    miss.update(hermes)
    save_json(GOV / "miss_bus.json", miss)

    res = load_json(GOV / "resource_snap.json", {})
    ev = load_json(GOV / "eval_probe.json", {})

    open_n = def_n = closed_n = 0
    gaps = Path(str(BASE) + ".gaps.txt")
    if gaps.exists():
        for i, ln in enumerate(gaps.read_text(errors="replace").splitlines()):
            if i < 2:
                continue
            if "waiting_oracle" in ln or "waiting_charter" in ln:
                def_n += 1
            else:
                p = ln.split()
                if len(p) > 1 and p[1] == "2":
                    closed_n += 1
                elif len(p) > 1 and p[1] == "1":
                    open_n += 1

    fault = Path(os.environ.get("CNET_FAULT_LOG", ROOT / "logs/cnet_faults.jsonl"))
    fault_lines = sum(1 for _ in fault.open()) if fault.exists() else 0
    lora = (
        len(list((ROOT / "logs/lora_store").glob("*")))
        if (ROOT / "logs/lora_store").is_dir()
        else 0
    )
    b = BASE.read_bytes() if BASE.exists() else b""
    units = b.count(b"acq_") + b.count(b"json_toolcall") + b.count(b"hyb_struct")
    web_notes = (
        int((GOV / "web_notes_count").read_text())
        if (GOV / "web_notes_count").exists()
        else 0
    )

    def hrs(k: str) -> float:
        t = state.get(k) or 0
        return 99.0 if not t else max(0.0, (time.time() - float(t)) / 3600.0)

    prev = load_json(GOV / "scoreboard.json", {})
    prev_ts = float(prev.get("ts_unix") or 0)
    dt_h = max(float(meta.get("min_dt_h") or 0.05), (time.time() - prev_ts) / 3600.0) if prev_ts else 0.25

    eval_delta = ev.get("eval_jtc_delta")
    eval_f = float(eval_delta) if eval_delta is not None else 0.0
    d_eval = ev.get("d_eval_jtc")
    d_eval_f = float(d_eval) if d_eval is not None else 0.0

    # combined real miss: waiting oracle + hermes errors
    real_miss = max(float(miss.get("real_miss_rate") or 0), float(hermes.get("hermes_err_rate") or 0))

    sb: dict[str, Any] = {
        "ts": datetime.now().astimezone().isoformat(timespec="seconds"),
        "ts_unix": time.time(),
        "engine": "governor_autonomous_v3",
        "cycle": int(state.get("cycle") or 0) + 1,
        "bonsai_ok": 1
        if active("bonsai-server.service")
        and sh(f"curl -sf --max-time 4 {HTTP}/v1/models >/dev/null").returncode == 0
        else 0,
        "lane_ok": 1 if active("cnet-personal-ai-lane.service") else 0,
        "open_gaps": open_n,
        "deferred_oracle": def_n,
        "closed_gaps": closed_n,
        "fault_lines": fault_lines,
        "lora_files": lora,
        "units_proxy": units,
        "hours_since_peft": hrs("last_peft_unix"),
        "hours_since_mine": hrs("last_mine_unix"),
        "hours_since_procedure": hrs("last_procedure_unix"),
        "hours_since_web": hrs("last_web_unix"),
        "hours_since_eval": hrs("last_eval_unix"),
        "real_miss_rate": round(real_miss, 4),
        "hermes_err_n": hermes.get("hermes_err_n", 0),
        "hermes_err_rate": hermes.get("hermes_err_rate", 0),
        "eval_jtc_delta": eval_f,
        "d_eval_jtc": d_eval_f,
        "web_notes": web_notes,
        "busy": int(res.get("busy") or 0),
        "allow_heavy": int(res.get("allow_heavy") if res.get("allow_heavy") is not None else 1),
        "night": int(res.get("night") or 0),
        "pause_inject": int(pins.get("pause_inject") or 0),
        "pause_web": int(pins.get("pause_web") or 0),
        "freeze_seals": int(pins.get("freeze_seals") or 0),
        "pending_outcome": int(state.get("pending_outcome") or 0),
        "eval_veto": int(state.get("eval_veto") or 0),
        "backlog_pressure": float(open_n + def_n),
        "teacher_uptime": 1.0 if active("bonsai-server.service") else 0.0,
        "dt_h": round(dt_h, 4),
        "threshold_backlog": float(meta.get("threshold_backlog") or 8),
        "threshold_miss": float(meta.get("threshold_miss") or 0.15),
    }

    for k in ("open_gaps", "closed_gaps", "fault_lines", "units_proxy", "backlog_pressure"):
        sb[f"d_{k}"] = float(sb.get(k, 0)) - float(prev.get(k, sb.get(k, 0)))

    # velocity only if dt sufficient
    if dt_h >= float(meta.get("min_dt_h") or 0.05):
        sb["rate_closed_per_h"] = sb.get("d_closed_gaps", 0) / dt_h
        sb["rate_units_per_h"] = sb.get("d_units_proxy", 0) / dt_h
    else:
        sb["rate_closed_per_h"] = 0.0
        sb["rate_units_per_h"] = 0.0
        sb["velocity_suppressed"] = 1

    ewma = load_json(GOV / "scoreboard_ewma.json", {})
    inst = max(0.0, sb["rate_units_per_h"]) + 0.1 * max(0.0, sb["rate_closed_per_h"])
    prev_v = float(ewma.get("learning_velocity") or 0)
    sb["learning_velocity"] = 0.3 * inst + 0.7 * prev_v

    hist = []
    hp = GOV / "scoreboard_history.jsonl"
    if hp.exists():
        for line in hp.read_text().splitlines()[-5:]:
            try:
                hist.append(json.loads(line))
            except Exception:
                pass
    if len(hist) >= 2 and dt_h >= float(meta.get("min_dt_h") or 0.05):
        du = sum(float(h.get("d_units_proxy") or 0) for h in hist[-3:])
        dc = sum(float(h.get("d_closed_gaps") or 0) for h in hist[-3:])
        sb["plateau"] = (
            1 if du <= 0 and dc <= 2 and sb["bonsai_ok"] and sb["lane_ok"] else 0
        )
    else:
        sb["plateau"] = int(state.get("plateau") or 0)

    gh = state.get("goal_health") or {}
    sb["goal_health_drain"] = float(gh.get("drain_open_gaps", 0.5))
    sb["goal_health_peft"] = float(gh.get("peft_jtc", 0.5))
    sb["goal_health_mine"] = float(gh.get("structure_mine", 0.5))
    sb["goal_health_coverage"] = float(gh.get("coverage_curiosity", 0.5))

    # project flags from urgency
    projs = project_scores(sb, meta)
    sb["projects"] = projs
    sb["top_project"] = projs[0]["id"] if projs else ""
    sb["project_web"] = 1 if any(p["id"] == "web_knowledge" and p["urgency"] > 0.2 for p in projs) else 0
    sb["project_jtc"] = 1 if any(p["id"] == "jtc_lift" and p["urgency"] > 0.15 for p in projs) else 0
    if eval_f < float(meta.get("threshold_eval_good") or 0.40):
        sb["project_jtc"] = 1

    # eval veto auto
    if not pins.get("eval_veto_override") and d_eval_f < -float(
        meta.get("threshold_eval_veto") or 0.05
    ):
        sb["eval_veto"] = 1
        sb["freeze_seals"] = 1
        pins["freeze_seals"] = 1

    return sb


def pick(charter: dict, sb: dict, pins: dict, meta: dict) -> list[dict]:
    max_n = int(pins.get("max_tasks_override") or 0) or int(charter["max_tasks"])
    force = str(pins.get("force_goal") or "").strip()
    if force:
        picked = [g for g in charter["goals"] if g["id"] == force]
        if sb["bonsai_ok"] == 0 or sb["lane_ok"] == 0:
            kt = [g for g in charter["goals"] if g["id"] == "keep_teacher_alive"]
            picked = kt + picked
        return picked[:max_n]

    # Inject dynamic thresholds into env for when clauses
    env = dict(sb)
    env["threshold_backlog"] = float(meta.get("threshold_backlog") or 8)

    def rank_key(g: dict) -> tuple:
        p = int(g.get("priority", 99))
        gid = g["id"]
        bias = 0
        # project-driven
        top = sb.get("top_project") or ""
        if top == "jtc_lift" and gid == "peft_jtc":
            bias -= 2
        if top == "gap_backlog" and gid == "drain_open_gaps":
            bias -= 2
        if top == "real_miss_cut" and gid in ("drain_open_gaps", "peft_jtc"):
            bias -= 2
        if top == "web_knowledge" and gid == "verified_web":
            bias -= 1
        if top == "procedure_coverage" and gid == "procedure_curriculum":
            bias -= 1
        # hermes errors → peft/jtc
        if float(sb.get("hermes_err_rate") or 0) > 0.25 and gid == "peft_jtc":
            bias -= int(meta.get("w_hermes_err", 2))
        if sb.get("plateau") and gid == "coverage_curiosity":
            bias += 3
        if sb.get("plateau") and gid == "break_plateau":
            bias -= 2
        if sb.get("busy") and gid in ("peft_jtc", "structure_mine", "break_plateau"):
            bias += 3
        if sb.get("eval_veto") and gid in ("peft_jtc", "structure_mine", "break_plateau"):
            bias += 5  # push away unless override
        if sb.get("pending_outcome") and gid == "outcome_review":
            bias -= 2
        health = {
            "drain_open_gaps": sb.get("goal_health_drain", 0.5),
            "peft_jtc": sb.get("goal_health_peft", 0.5),
            "structure_mine": sb.get("goal_health_mine", 0.5),
            "coverage_curiosity": sb.get("goal_health_coverage", 0.5),
        }.get(gid, 0.5)
        # prefer slightly lower health (needs work) among equals
        return (p + bias, health)

    goals = sorted(charter["goals"], key=rank_key)
    picked: list[dict] = []
    for g in goals:
        if len(picked) >= max_n:
            break
        when = g.get("when") or "false"
        # rewrite backlog threshold dynamically
        when = when.replace("backlog_pressure >= 8", f"backlog_pressure >= {meta.get('threshold_backlog', 8)}")
        when = when.replace("real_miss_rate >= 0.15", f"real_miss_rate >= {meta.get('threshold_miss', 0.15)}")
        if eval_when(when, env):
            if g["id"] == "health_hold" and picked:
                continue
            # skip seal goals under veto
            if sb.get("eval_veto") and g["id"] in (
                "peft_jtc",
                "structure_mine",
                "break_plateau",
            ):
                continue
            picked.append(g)
            if g["id"] == "health_hold":
                break
    if not picked:
        for g in charter["goals"]:
            if g["id"] == "health_hold":
                return [g]
    return picked


def act(name: str, sb: dict, pins: dict, state: dict, meta: dict) -> bool:
    dry = os.environ.get("GOV_DRY") == "1"

    def run(cmd: str, t: int = 600) -> bool:
        if dry:
            return True
        r = sh(cmd, timeout=t)
        return r.returncode == 0

    if name == "ensure_bonsai":
        run("systemctl --user start bonsai-server.service")
        time.sleep(1)
        return (
            sh(f"curl -sf --max-time 4 {HTTP}/v1/models >/dev/null").returncode == 0
        )
    if name == "ensure_lane":
        run("systemctl --user start cnet-personal-ai-lane.service")
        time.sleep(1)
        return active("cnet-personal-ai-lane.service")
    if name == "inject_window_gaps":
        if pins.get("pause_inject"):
            return True
        win = Path(
            os.environ.get(
                "CNET_RESIDUAL_WINDOW", ROOT / "english_window_256_bonsai.txt"
            )
        )
        ids = [int(x) for x in win.read_text().split() if x.strip().isdigit()]
        if not ids:
            return False
        W = len(ids)
        k = 3
        n = min(int(meta.get("inject_n") or 4), len(ids))
        random.seed(int(time.time()) // 600)
        picks = random.sample(ids, n)
        inbox = Path(str(BASE) + ".inbox")
        if not dry:
            with inbox.open("a") as f:
                for tid in picks:
                    f.write(f"NO_PLAN 1 {W} 1 w_cur 1 {W} {k} tk{tid}q{tid}\n")
        return True
    if name == "nudge_lane_tick":
        return True
    if name == "run_cert_learn":
        if pins.get("freeze_seals") or sb.get("eval_veto"):
            return True  # skipped under veto — not failure
        ok = run(
            f"CNET_BASE_PATH={BASE} CNET_FAULT_DEDUPE=0 CNET_RESIDUAL_HTTP={HTTP} "
            f"CNET_RESIDUAL_WINDOW={os.environ.get('CNET_RESIDUAL_WINDOW', ROOT / 'english_window_256_bonsai.txt')} "
            f"timeout 600 {ROOT}/bin/cnet_cert_learn_tick >>{GOV}/muscle.log 2>&1"
        )
        if ok:
            state["last_peft_unix"] = time.time()
        return ok
    if name == "run_structure_mine":
        if pins.get("freeze_seals") or sb.get("eval_veto"):
            return True
        ok = run(
            f"CNET_BASE_PATH={BASE} CNET_RESIDUAL_HTTP={HTTP} CNET_STRUCTURE_EXPAND_N=32 "
            f"timeout 900 {ROOT}/bin/struct_mine_persist >>{GOV}/muscle.log 2>&1"
        )
        if ok:
            state["last_mine_unix"] = time.time()
        return ok
    if name == "seed_procedures":
        ok = run(
            f"bash {ROOT}/scripts/procedure_chunk_seal.sh >>{GOV}/muscle.log 2>&1", t=60
        )
        if ok:
            state["last_procedure_unix"] = time.time()
        return ok
    if name == "run_eval_probe":
        ok = run(
            f"bash {ROOT}/scripts/governor_hooks.sh eval >>{GOV}/muscle.log 2>&1", t=240
        )
        if ok:
            state["last_eval_unix"] = time.time()
        return ok
    if name == "safe_web":
        if pins.get("pause_web"):
            return True
        if int(sb.get("web_notes") or 0) >= int(meta.get("max_web_notes") or 80):
            return True
        ok = run(
            f"bash {ROOT}/scripts/governor_hooks.sh web >>{GOV}/muscle.log 2>&1", t=60
        )
        if ok:
            state["last_web_unix"] = time.time()
        return ok
    if name == "queue_research":
        return run(
            f"bash {ROOT}/scripts/governor_hooks.sh research >>{GOV}/muscle.log 2>&1",
            t=60,
        )
    if name == "close_outcome":
        # apply credit
        prev_b = float(state.get("pending_backlog") or sb.get("backlog_pressure") or 0)
        gh = dict(state.get("goal_health") or {})

        def bump(k: str, d: float) -> None:
            gh[k] = max(0.0, min(1.0, float(gh.get(k, 0.5)) + d))

        db = prev_b - float(sb.get("backlog_pressure") or 0)
        if db > 0.5:
            bump("drain_open_gaps", 0.06)
        elif db < -0.5:
            bump("drain_open_gaps", -0.05)
        if float(sb.get("d_eval_jtc") or 0) > 0:
            bump("peft_jtc", 0.06)
        elif float(sb.get("d_eval_jtc") or 0) < 0:
            bump("peft_jtc", -0.06)
        if int(sb.get("hermes_err_n") or 0) == 0:
            bump("peft_jtc", 0.02)
        state["goal_health"] = gh
        state["pending_outcome"] = 0
        return True
    if name in ("ensure_curiosity", "log_hold"):
        return True
    return False


def outcome_open(state: dict, sb: dict, picked: list) -> None:
    state["pending_outcome"] = 1
    state["pending_backlog"] = sb.get("backlog_pressure")
    state["pending_eval"] = sb.get("eval_jtc_delta")
    state["pending_since"] = time.time()
    state["pending_goals"] = [g["id"] for g in picked]
    if any(g["id"] == "drain_open_gaps" for g in picked):
        state["drain_streak"] = int(state.get("drain_streak") or 0) + 1
    else:
        state["drain_streak"] = 0


def persist(sb: dict, state: dict, picked: list, results: list, pins: dict, meta: dict) -> None:
    GOV.mkdir(parents=True, exist_ok=True)
    save_json(GOV / "scoreboard.json", sb)
    with (GOV / "scoreboard_history.jsonl").open("a") as f:
        f.write(
            json.dumps(
                {
                    k: sb.get(k)
                    for k in (
                        "ts",
                        "ts_unix",
                        "cycle",
                        "open_gaps",
                        "deferred_oracle",
                        "closed_gaps",
                        "fault_lines",
                        "units_proxy",
                        "d_closed_gaps",
                        "d_units_proxy",
                        "d_backlog_pressure",
                        "backlog_pressure",
                        "learning_velocity",
                        "plateau",
                        "real_miss_rate",
                        "hermes_err_rate",
                        "eval_jtc_delta",
                        "web_notes",
                        "busy",
                        "allow_heavy",
                        "eval_veto",
                        "top_project",
                    )
                }
            )
            + "\n"
        )
    save_json(
        GOV / "scoreboard_ewma.json",
        {
            "learning_velocity": sb["learning_velocity"],
            "teacher_uptime": sb["teacher_uptime"],
            "updated_ts": sb["ts"],
            "engine": "v3",
        },
    )
    decision = {
        "ts": sb["ts"],
        "engine": "governor_autonomous_v3",
        "goals": [{"id": g["id"], "priority": g.get("priority")} for g in picked],
        "actions": results,
        "scoreboard_focus": {
            k: sb.get(k)
            for k in (
                "cycle",
                "backlog_pressure",
                "real_miss_rate",
                "hermes_err_rate",
                "eval_jtc_delta",
                "d_eval_jtc",
                "web_notes",
                "busy",
                "allow_heavy",
                "plateau",
                "learning_velocity",
                "eval_veto",
                "top_project",
                "dt_h",
            )
        },
        "projects": sb.get("projects"),
        "meta": {
            k: meta.get(k)
            for k in (
                "w_backlog",
                "w_real_miss",
                "w_hermes_err",
                "w_eval",
                "threshold_backlog",
                "inject_n",
                "evolve_rate",
            )
        },
        "evolve_note": state.get("evolve_note"),
        "pins": pins,
        "goal_health": state.get("goal_health"),
    }
    save_json(GOV / "last_decision.json", decision)
    state["cycle"] = sb["cycle"]
    state["last_cycle_unix"] = time.time()
    state["last_goals"] = [g["id"] for g in picked]
    state["learning_velocity"] = sb["learning_velocity"]
    state["plateau"] = sb["plateau"]
    save_json(GOV / "state.json", state)


def self_test() -> int:
    env = {
        "bonsai_ok": 0,
        "lane_ok": 1,
        "backlog_pressure": 0,
        "real_miss_rate": 0,
        "allow_heavy": 1,
        "pause_web": 0,
        "web_notes": 0,
        "hours_since_web": 1,
        "project_web": 0,
        "pending_outcome": 0,
        "plateau": 0,
        "teacher_uptime": 1,
        "busy": 0,
        "fault_lines": 100,
        "hours_since_peft": 1,
        "eval_jtc_delta": 0.2,
        "hours_since_mine": 2,
        "hours_since_procedure": 1,
        "hermes_err_rate": 0.4,
        "eval_veto": 0,
        "freeze_seals": 0,
        "pause_inject": 0,
        "threshold_backlog": 8,
        "threshold_miss": 0.15,
    }
    assert eval_when("bonsai_ok == 0 or lane_ok == 0", env)
    env["bonsai_ok"] = 1
    env["backlog_pressure"] = 24
    assert eval_when("backlog_pressure >= 8", env)
    # hermes fuel present
    h = count_hermes_errors(24)
    assert "hermes_err_n" in h
    meta = load_meta()
    assert "w_eval" in meta
    # self evolve mutates
    st: dict[str, Any] = {"drain_streak": 5}
    sb = {"d_backlog_pressure": 1, "d_eval_jtc": -0.1, "hermes_err_rate": 0.5, "plateau": 1, "web_notes": 5, "dt_h": 0.01, "cycle": 1}
    m2 = self_evolve(dict(META_DEFAULTS), sb, st)
    assert m2["w_eval"] >= META_DEFAULTS["w_eval"]
    assert st.get("eval_veto") == 1
    print("GOVERNOR_V3_SELFTEST_PASS checks=5")
    return 0


def main() -> int:
    if "--test" in sys.argv:
        return self_test()
    dry = "--dry-run" in sys.argv
    if dry:
        os.environ["GOV_DRY"] = "1"

    GOV.mkdir(parents=True, exist_ok=True)
    state = load_json(GOV / "state.json", {})
    pins = parse_pins()
    meta = load_meta()
    charter = parse_charter()

    sb = collect(state, pins, meta)
    meta = self_evolve(meta, sb, state)
    # re-apply veto after evolve
    if state.get("eval_veto") and not pins.get("eval_veto_override"):
        sb["eval_veto"] = 1
        sb["freeze_seals"] = 1
        pins["freeze_seals"] = 1

    picked = pick(charter, sb, pins, meta)
    results = []
    seen = set()
    for g in picked:
        for a in g.get("actions") or []:
            if a in seen:
                continue
            seen.add(a)
            ok = act(a, sb, pins, state, meta)
            results.append({"action": a, "ok": bool(ok)})

    # action-based health
    gh = dict(state.get("goal_health") or {})
    ok_ratio = sum(1 for r in results if r["ok"]) / max(1, len(results))
    for g in picked:
        if g["id"] == "health_hold":
            continue
        cur = float(gh.get(g["id"], 0.5))
        gh[g["id"]] = max(0.0, min(1.0, cur + 0.08 * ok_ratio - 0.05 * (1 - ok_ratio)))
    state["goal_health"] = gh
    outcome_open(state, sb, picked)

    if not dry:
        persist(sb, state, picked, results, pins, meta)
    else:
        save_json(
            GOV / "last_decision.json",
            {
                "ts": sb["ts"],
                "dry_run": True,
                "engine": "governor_autonomous_v3",
                "goals": [{"id": g["id"]} for g in picked],
                "actions": results,
                "scoreboard_focus": {
                    k: sb.get(k)
                    for k in (
                        "backlog_pressure",
                        "real_miss_rate",
                        "hermes_err_rate",
                        "eval_jtc_delta",
                        "eval_veto",
                        "top_project",
                    )
                },
                "meta": {k: meta.get(k) for k in ("w_eval", "w_hermes_err", "inject_n")},
            },
        )

    print(
        "GOVERNOR_CYCLE_OK",
        json.dumps(
            {
                "engine": "v3",
                "goals": [g["id"] for g in picked],
                "actions": [(r["action"], r["ok"]) for r in results],
                "backlog": sb.get("backlog_pressure"),
                "miss": sb.get("real_miss_rate"),
                "hermes_err": sb.get("hermes_err_rate"),
                "eval_jtc": sb.get("eval_jtc_delta"),
                "eval_veto": sb.get("eval_veto"),
                "top_project": sb.get("top_project"),
                "evolve": state.get("evolve_note"),
                "meta_w_eval": meta.get("w_eval"),
            }
        ),
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
