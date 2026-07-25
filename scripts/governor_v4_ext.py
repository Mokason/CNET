"""v4 extensions: structured Hermes, goal graph, transfer, stable evolve, novel curriculum."""
from __future__ import annotations
import json, os, time, subprocess
from pathlib import Path
from typing import Any

def run_structured(root: Path, gov: Path) -> dict:
    env = os.environ.copy()
    env["CNET_GOVERNOR_DIR"] = str(gov)
    r = subprocess.run(
        ["python3", str(root / "scripts/governor_hermes_structured.py")],
        cwd=str(root), capture_output=True, text=True, env=env, timeout=60,
    )
    p = gov / "hermes_structured.json"
    try:
        return json.loads(p.read_text()) if p.exists() else {"hermes_task_fail_rate": 0.0, "noisy": True}
    except Exception:
        return {"hermes_task_fail_rate": 0.0, "noisy": True, "err": r.stderr[-200:]}


def score_goal_graph(sb: dict, graph_path: Path, meta: dict) -> list[dict]:
    g = {}
    try:
        g = json.loads(graph_path.read_text())
    except Exception:
        return []
    nodes = [n for n in g.get("nodes") or [] if n.get("kind") != "transfer"]
    transfers = [n for n in g.get("nodes") or [] if n.get("kind") == "transfer"]
    scored = []
    by_id = {}
    for n in nodes:
        metric = n.get("metric")
        target = float(n.get("target") or 0)
        weight = float(n.get("weight") or 1) * float(meta.get("w_eval", 1) if "eval" in (metric or "") or "jtc" in (metric or "") else 1)
        prefer_lower = bool(n.get("prefer_lower"))
        val = float(sb.get(metric) or 0)
        if prefer_lower:
            gap = max(0.0, val - target) / max(abs(target), 1e-6)
        else:
            gap = max(0.0, target - val) / max(abs(target), 1e-6)
        # deps: if dep unhealthy, boost urgency slightly (unblock)
        dep_pen = 0.0
        for d in n.get("deps") or []:
            if d in by_id and by_id[d]["urgency"] > 0.5:
                dep_pen += 0.15
        urgency = gap * weight + dep_pen
        item = {
            "id": n["id"],
            "urgency": round(urgency, 4),
            "metric": metric,
            "value": val,
            "target": target,
            "goals": n.get("goals") or [],
            "kind": n.get("kind"),
        }
        scored.append(item)
        by_id[n["id"]] = item
    # transfer edges: if from improving and to still bad, add transfer urgency
    credit = float(meta.get("transfer_credit") or 0.25)
    for tr in transfers:
        frm, to = tr.get("from"), tr.get("to")
        if frm in by_id and to in by_id:
            # from doing well (low urgency) and to bad (high urgency) → opportunity
            if by_id[frm]["urgency"] < 0.3 and by_id[to]["urgency"] > 0.4:
                by_id[to]["urgency"] = round(by_id[to]["urgency"] + credit * float(tr.get("weight") or 1), 4)
                by_id[to]["transfer_from"] = frm
                sb.setdefault("transfer_hits", []).append({"from": frm, "to": to})
    scored = sorted(by_id.values(), key=lambda x: -x["urgency"])
    return scored


def stable_evolve(meta: dict, sb: dict, state: dict, defaults: dict) -> dict:
    """Slower, clamped evolution requiring min cycles."""
    cycles = int(state.get("cycle") or 0)
    min_c = int(meta.get("evolve_min_cycles") or 4)
    rate = float(meta.get("evolve_rate") or 0.04)
    clamp = float(meta.get("evolve_clamp") or 0.15)
    if cycles < min_c:
        state["evolve_note"] = f"warmup_{cycles}/{min_c}"
        return meta

    def adj(key, delta):
        base = float(defaults.get(key, meta.get(key, 1)))
        cur = float(meta.get(key, base))
        nxt = cur + delta
        lo, hi = base * (1 - clamp * 3), base * (1 + clamp * 3)
        # absolute clamps for known keys
        bounds = {
            "w_eval": (1.0, 5.0),
            "w_real_miss": (1.0, 5.0),
            "w_hermes_err": (1.0, 5.0),
            "w_backlog": (1.0, 4.0),
            "threshold_backlog": (4.0, 16.0),
            "inject_n": (2, 6),
            "evolve_rate": (0.02, 0.08),
        }
        if key in bounds:
            lo, hi = bounds[key]
        meta[key] = max(lo, min(hi, nxt))

    # only evolve if dt decent
    if float(sb.get("dt_h") or 0) < float(meta.get("min_dt_h") or 0.05):
        state["evolve_note"] = "skip_evolve_small_dt"
        return meta

    note = []
    if float(sb.get("d_backlog_pressure") or 0) >= 0 and int(state.get("drain_streak") or 0) >= 3:
        adj("w_real_miss", rate)
        adj("threshold_backlog", -0.15)
        adj("inject_n", -1 if float(meta.get("inject_n", 4)) > 2 else 0)
        # inject_n is int
        meta["inject_n"] = int(round(float(meta["inject_n"])))
        note.append("backlog_stuck")
    if float(sb.get("d_eval_jtc") or 0) < -float(meta.get("threshold_eval_veto") or 0.05):
        adj("w_eval", rate * 1.5)
        state["eval_veto"] = 1
        note.append("eval_veto")
    elif float(sb.get("d_eval_jtc") or 0) > 0.02:
        adj("w_eval", -rate * 0.25)
        state["eval_veto"] = 0
        note.append("eval_ok")
    # structured hermes fail rate (not noisy log)
    hfr = float(sb.get("hermes_task_fail_rate") or 0)
    if hfr > 0.2 and not sb.get("hermes_noisy"):
        adj("w_hermes_err", rate)
        note.append("hermes_struct_fail")
    elif hfr < 0.08:
        adj("w_hermes_err", -rate * 0.2)
    if int(sb.get("plateau") or 0):
        adj("w_velocity", rate * 0.5)
        note.append("plateau")
    # transfer success: if transfer hit and hermes fail fell
    if sb.get("transfer_hits") and float(sb.get("d_hermes_task_fail_rate") or 0) < 0:
        adj("transfer_credit", rate * 0.5)
        note.append("transfer_worked")

    state["evolve_note"] = "+".join(note) if note else "stable"
    meta["updated_ts"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    meta["cycle"] = int(sb.get("cycle") or 0)
    return meta


def maybe_novel_curriculum(root: Path, gov: Path, sb: dict, meta: dict, state: dict) -> dict | None:
    """Bounded novel goal proposal from top gap — does not auto-run; logs proposal."""
    hours = float(meta.get("novel_curriculum_hours") or 8)
    last = float(state.get("last_novel_unix") or 0)
    if time.time() - last < hours * 3600:
        return None
    if float(sb.get("backlog_pressure") or 0) < 3 and float(sb.get("hermes_task_fail_rate") or 0) < 0.05:
        return None
    # propose one curriculum line
    prop = {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "reason": "elevated_miss_or_backlog",
        "proposal": {
            "id": "novel_focus_" + str(int(time.time()) % 10000),
            "suggest_goal": "peft_jtc" if float(sb.get("hermes_task_fail_rate") or 0) > 0.15 else "drain_open_gaps",
            "suggest_actions": ["run_eval_probe", "run_cert_learn"]
            if float(sb.get("hermes_task_fail_rate") or 0) > 0.15
            else ["inject_window_gaps", "ensure_lane"],
            "metric_focus": "hermes_task_fail_rate"
            if float(sb.get("hermes_task_fail_rate") or 0) > 0.15
            else "backlog_pressure",
        },
        "status": "proposed",  # human/charter can promote
    }
    path = gov / "novel_curriculum.jsonl"
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a") as f:
        f.write(json.dumps(prop) + "\n")
    state["last_novel_unix"] = time.time()
    state["last_novel"] = prop["proposal"]
    return prop
