#!/usr/bin/env python3
"""CNET neuromod organ — biological metaphor, not AGI.

Channels (homeostatic, clamped; NEVER seal/CERT path):
  dopamine  (DA)  — task-done-right reward; brief front-door prefer-LOCAL boost
  serotonin (5HT) — LOW = impulsivity; HIGH = control (sort/pin/stable)
  adenosine (ADO) — need to process / reorganize / consolidate WM+context;
                    high ADO pauses grow probes for a cycle

Law:
  - biases scheduling & consolidation ONLY
  - never self-CERT, never lowers floors
  - homeostasis pulls to baseline each tick

  python3 scripts/cnet_neuromod.py --test
  python3 scripts/cnet_neuromod.py --tick
"""
from __future__ import annotations

import argparse
import json
import os
import time
from pathlib import Path
from typing import Any

ROOT = Path(os.environ.get("CNET_ROOT", Path(__file__).resolve().parents[1]))
GOV = Path(os.environ.get("CNET_GOVERNOR_DIR", ROOT / "logs/governor"))
STATE_PATH = GOV / "neuromod_state.json"
HIST_PATH = GOV / "neuromod_history.jsonl"
AUTO_KPI = ROOT / "logs" / "marble_24_7" / "AUTONOMOUS_CYCLE.json"
PERSONA = GOV / "personality_state.json"
EVOLVE = ROOT / "artifacts" / "roe_daily_packs" / "EVOLVE_TICK.json"
MISS = ROOT / "artifacts" / "roe_daily_packs" / "miss_log.jsonl"
# Tight coupling artifacts (consumed by front_door + autonomous cycle)
FRONT_DOOR_BIAS = GOV / "front_door_bias.json"
SCHEDULE_GATE = GOV / "schedule_gate.json"
SOFT_BIAS = GOV / "neuromod_soft_bias.json"

DEFAULT = {
    "dopamine": 0.50,
    "serotonin": 0.55,
    "adenosine": 0.35,
}

# DA boost TTL (seconds) — "briefly"
DA_LOCAL_BOOST_TTL_S = int(os.environ.get("CNET_DA_LOCAL_TTL_S", "600"))  # 10 min
DA_LOCAL_WEIGHT = float(os.environ.get("CNET_DA_LOCAL_WEIGHT", "1.75"))

ACTIONS = {
    "high_da": [
        "prefer_local_skills",
        "front_door_prefer_local_boost",  # brief weight for LOCAL path
        "voice_warm",
        "bias_outcome_review",
    ],
    # High 5HT = high control
    "high_5ht": [
        "sort_task_queue",
        "prefer_pin_memory",
        "bias_thoroughness",
        "control_mode",
    ],
    # Low 5HT = high impulsivity
    "low_5ht": [
        "impulsivity_mode",
        "allow_teacher_faster",
        "bias_boldness",
    ],
    "high_ado": [
        "run_sleep_consolidate",
        "compress_context",
        "defer_new_explore",
        "pause_grow_probes",  # tighter: skip MISS/grow curriculum one cycle
    ],
}


def _clamp(x: float, lo: float = 0.15, hi: float = 0.85) -> float:
    return max(lo, min(hi, float(x)))


def _load_json(p: Path, default: Any = None) -> Any:
    if not p.is_file():
        return default
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return default


def load_state() -> dict[str, Any]:
    st = _load_json(STATE_PATH, None)
    if not isinstance(st, dict):
        st = {
            "engine": "neuromod_v1",
            "levels": dict(DEFAULT),
            "baseline": dict(DEFAULT),
            "cycles": 0,
            "last_actions": [],
            "wm_load": 0.0,
            "queue_pressure": 0.0,
            "last_reward_events": 0,
        }
    st.setdefault("levels", dict(DEFAULT))
    st.setdefault("baseline", dict(DEFAULT))
    for k, v in DEFAULT.items():
        st["levels"].setdefault(k, v)
        st["baseline"].setdefault(k, v)
    return st


def gather_signals() -> dict[str, float]:
    """Map real system KPIs → neuromod drive signals."""
    auto = _load_json(AUTO_KPI, {}) or {}
    kpi = auto.get("kpi") or {}
    persona = _load_json(PERSONA, {}) or {}
    affect = persona.get("affect") or {}
    evolve = _load_json(EVOLVE, {}) or {}

    local_hit = float(kpi.get("local_hit") or 0.0)
    promoted = float(kpi.get("promoted") or 0.0)
    miss_n = float(kpi.get("miss_n") or 0.0)
    probe_n = float(kpi.get("probe_n") or 1.0)
    skipped = float(kpi.get("evolve_skipped") or len(evolve.get("skipped") or []))

    # miss log length as rough WM/context pressure
    miss_lines = 0
    if MISS.is_file():
        try:
            miss_lines = sum(1 for _ in MISS.open("r", encoding="utf-8", errors="ignore"))
        except OSError:
            miss_lines = 0

    reward_events = promoted + (10.0 * local_hit if local_hit >= 0.7 else 3.0 * local_hit)
    fail_pressure = miss_n / max(1.0, probe_n) + 0.05 * min(50, skipped)

    # working-memory / context load proxy
    wm_load = _clamp(miss_lines / 200.0, 0.0, 1.0)  # 200 misses ≈ full
    queue_pressure = _clamp(fail_pressure, 0.0, 1.0)

    # map legacy affect if present
    da_affect = float(affect.get("reward") or 0.5)
    vig = float(affect.get("vigilance") or 0.5)
    calm = float(affect.get("calm") or 0.5)
    frust = float(affect.get("frustration") or 0.5)

    return {
        "local_hit": local_hit,
        "promoted": promoted,
        "miss_n": miss_n,
        "reward_events": reward_events,
        "fail_pressure": fail_pressure,
        "wm_load": wm_load,
        "queue_pressure": queue_pressure,
        "da_affect": da_affect,
        "vigilance": vig,
        "calm": calm,
        "frustration": frust,
        "miss_lines": float(miss_lines),
    }


def update_levels(
    prev: dict[str, float],
    baseline: dict[str, float],
    sig: dict[str, float],
    home: float = 0.08,
) -> dict[str, float]:
    """EWMA neuromod update + homeostasis."""
    da, srt, ado = prev["dopamine"], prev["serotonin"], prev["adenosine"]

    # --- Dopamine: task done right ---
    # pulse up on local_hit / promotes; down on fail_pressure
    da_pulse = 0.45
    da_pulse += 0.35 * sig["local_hit"]
    da_pulse += min(0.25, 0.05 * sig["promoted"])
    da_pulse -= 0.25 * sig["fail_pressure"]
    da_pulse = 0.5 * da_pulse + 0.5 * sig["da_affect"]
    da = 0.65 * da + 0.35 * da_pulse

    # --- Serotonin: HIGH control / LOW impulsivity ---
    # high calm + moderate queue → control (sort/pin)
    # high vigilance/frustration → low 5HT (impulsive)
    srt_pulse = 0.50
    srt_pulse += 0.28 * sig["calm"]
    srt_pulse += 0.18 * sig["queue_pressure"]  # need orderly queue under load
    srt_pulse -= 0.30 * sig["vigilance"]
    srt_pulse -= 0.22 * sig["frustration"]
    srt = 0.70 * srt + 0.30 * srt_pulse

    # --- Adenosine: need consolidate / reorganize ---
    ado_pulse = 0.25
    ado_pulse += 0.45 * sig["wm_load"]
    ado_pulse += 0.25 * sig["queue_pressure"]
    ado_pulse += 0.10 * min(1.0, sig["miss_n"] / 10.0)
    ado_pulse -= 0.10 * max(0.0, sig["local_hit"] - 0.5)
    ado = 0.75 * ado + 0.25 * ado_pulse

    # homeostasis toward baseline
    da = da + home * (baseline["dopamine"] - da)
    srt = srt + home * (baseline["serotonin"] - srt)
    ado = ado + home * (baseline["adenosine"] - ado)

    return {
        "dopamine": _clamp(da),
        "serotonin": _clamp(srt),
        "adenosine": _clamp(ado),
    }


def control_impulsivity(levels: dict[str, float]) -> dict[str, float]:
    """Derived axes: control ∝ 5HT, impulsivity ∝ (1-5HT)."""
    s = float(levels.get("serotonin") or 0.5)
    return {
        "control": _clamp(s),
        "impulsivity": _clamp(1.0 - s),
    }


def decide_actions(levels: dict[str, float], thr: dict[str, float] | None = None) -> list[str]:
    thr = thr or {"da": 0.62, "5ht_hi": 0.60, "5ht_lo": 0.42, "ado": 0.58}
    acts: list[str] = []
    if levels["dopamine"] >= thr["da"]:
        acts.extend(ACTIONS["high_da"])
    # High 5HT = control; Low 5HT = impulsivity (mutually exclusive bands)
    if levels["serotonin"] >= thr["5ht_hi"]:
        acts.extend(ACTIONS["high_5ht"])
    elif levels["serotonin"] <= thr["5ht_lo"]:
        acts.extend(ACTIONS["low_5ht"])
    if levels["adenosine"] >= thr["ado"]:
        acts.extend(ACTIONS["high_ado"])
    # mutual inhibition: very high ADO suppresses warm/impulse celebration
    if levels["adenosine"] >= 0.70:
        acts = [a for a in acts if a not in ("voice_warm", "allow_teacher_faster", "bias_boldness")]
        if "run_sleep_consolidate" not in acts:
            acts.append("run_sleep_consolidate")
        if "pause_grow_probes" not in acts:
            acts.append("pause_grow_probes")
    # high control + queue → sort first
    if levels["serotonin"] >= 0.65 and "sort_task_queue" not in acts:
        acts.insert(0, "sort_task_queue")
    # low control (impulse) should not pin memory
    if levels["serotonin"] <= thr["5ht_lo"]:
        acts = [a for a in acts if a not in ("prefer_pin_memory", "control_mode")]
    seen: set[str] = set()
    out: list[str] = []
    for a in acts:
        if a not in seen:
            seen.add(a)
            out.append(a)
    return out


def write_coupling(levels: dict[str, float], actions: list[str]) -> dict[str, Any]:
    """Tight coupling files for front_door + autonomous cycle."""
    GOV.mkdir(parents=True, exist_ok=True)
    now = time.time()
    ci = control_impulsivity(levels)
    coupling: dict[str, Any] = {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "levels": levels,
        "control": ci["control"],
        "impulsivity": ci["impulsivity"],
    }

    # DA → brief front-door prefer LOCAL weight
    if "front_door_prefer_local_boost" in actions or levels["dopamine"] >= 0.62:
        bias = {
            "prefer_local": True,
            "prefer_local_weight": DA_LOCAL_WEIGHT,
            "disable_live_teacher": True,  # weight as: LOCAL path preferred, no live LLM
            "reason": "dopamine_task_done_right",
            "created_ts": now,
            "expires_ts": now + DA_LOCAL_BOOST_TTL_S,
            "ttl_s": DA_LOCAL_BOOST_TTL_S,
        }
        FRONT_DOOR_BIAS.write_text(json.dumps(bias, indent=2) + "\n", encoding="utf-8")
        coupling["front_door_bias"] = bias
    else:
        # expire stale boost if DA low and file expired
        old = _load_json(FRONT_DOOR_BIAS, {}) or {}
        if old and float(old.get("expires_ts") or 0) < now:
            try:
                FRONT_DOOR_BIAS.unlink(missing_ok=True)  # type: ignore[arg-type]
            except TypeError:
                if FRONT_DOOR_BIAS.is_file():
                    FRONT_DOOR_BIAS.unlink()
            coupling["front_door_bias"] = None
        else:
            coupling["front_door_bias"] = old or None

    # ADO → pause grow probes; 5HT axes for schedule (level-based, not stale actions)
    gate = {
        "pause_grow_probes": levels["adenosine"] >= 0.58,
        "pause_teacher": levels["adenosine"] >= 0.58 and levels["serotonin"] >= 0.55,
        "control_mode": levels["serotonin"] >= 0.60,
        "impulsivity_mode": levels["serotonin"] <= 0.42,
        "allow_teacher_faster": levels["serotonin"] <= 0.42,
        "reason": [],
        "expires_ts": now + 1200,  # ~20 min cycle
        "created_ts": now,
    }
    if gate["pause_grow_probes"]:
        gate["reason"].append("adenosine_consolidate")
    if gate["control_mode"]:
        gate["reason"].append("serotonin_high_control")
    if gate["impulsivity_mode"]:
        gate["reason"].append("serotonin_low_impulsivity")
    if "pause_grow_probes" in actions and not gate["pause_grow_probes"]:
        # action requested this tick but level already eased — keep one soft cycle
        gate["pause_grow_probes"] = True
        gate["reason"].append("action_linger")
    SCHEDULE_GATE.write_text(json.dumps(gate, indent=2) + "\n", encoding="utf-8")
    coupling["schedule_gate"] = gate

    soft = {
        "flags": sorted(set(actions)),
        "control": ci["control"],
        "impulsivity": ci["impulsivity"],
        "ts": coupling["ts"],
    }
    SOFT_BIAS.write_text(json.dumps(soft, indent=2) + "\n", encoding="utf-8")
    coupling["soft_bias"] = soft
    return coupling


def apply_actions(actions: list[str], dry_run: bool = False) -> list[dict[str, Any]]:
    """Execute safe side-effects only (no CERT seal)."""
    results = []
    for a in actions:
        r: dict[str, Any] = {"action": a, "ok": False, "detail": ""}
        if dry_run:
            r["ok"] = True
            r["detail"] = "dry_run"
            results.append(r)
            continue
        try:
            if a == "run_sleep_consolidate":
                bin_path = ROOT / "bin" / "test_cnet_sleep_consolidate"
                if not bin_path.is_file():
                    r["detail"] = "sleep_consolidate_intent"
                    r["ok"] = True
                    req = GOV / "consolidate_request.json"
                    GOV.mkdir(parents=True, exist_ok=True)
                    req.write_text(
                        json.dumps(
                            {
                                "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                                "reason": "adenosine_high",
                                "action": "sleep_consolidate",
                            },
                            indent=2,
                        )
                        + "\n",
                        encoding="utf-8",
                    )
                else:
                    import subprocess

                    p = subprocess.run(
                        [str(bin_path)],
                        cwd=str(ROOT),
                        capture_output=True,
                        text=True,
                        timeout=120,
                    )
                    r["ok"] = p.returncode == 0
                    r["detail"] = (p.stdout or p.stderr or "")[:200]
            elif a == "sort_task_queue":
                qpath = GOV / "task_queue_sorted.json"
                auto = _load_json(AUTO_KPI, {}) or {}
                probes = list(auto.get("probes") or [])
                probes.sort(
                    key=lambda p: (
                        0 if p.get("source") == "LOCAL" else 1,
                        0 if p.get("tag") in ("law", "soul", "toolcall") else 1,
                        p.get("q") or "",
                    )
                )
                GOV.mkdir(parents=True, exist_ok=True)
                qpath.write_text(json.dumps({"ts": time.time(), "probes": probes}, indent=2) + "\n")
                r["ok"] = True
                r["detail"] = f"sorted_{len(probes)}"
            elif a == "prefer_pin_memory":
                pin = GOV / "memory_pin_pref.json"
                GOV.mkdir(parents=True, exist_ok=True)
                pin.write_text(
                    json.dumps(
                        {
                            "prefer_pin": True,
                            "reason": "serotonin_high_control",
                            "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                        },
                        indent=2,
                    )
                    + "\n"
                )
                r["ok"] = True
                r["detail"] = "pin_pref_on"
            elif a in ("control_mode", "impulsivity_mode"):
                r["ok"] = True
                r["detail"] = a
            elif a == "allow_teacher_faster":
                r["ok"] = True
                r["detail"] = "impulse_teacher_ok"
            elif a == "bias_boldness":
                r["ok"] = True
                r["detail"] = "impulse_bold"
            elif a in ("front_door_prefer_local_boost", "pause_grow_probes"):
                r["ok"] = True
                r["detail"] = "coupling_file"
            elif a == "compress_context":
                cpath = GOV / "context_compress_request.json"
                GOV.mkdir(parents=True, exist_ok=True)
                cpath.write_text(
                    json.dumps(
                        {
                            "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                            "reason": "adenosine_wm_load",
                            "action": "compress_context",
                        },
                        indent=2,
                    )
                    + "\n"
                )
                r["ok"] = True
                r["detail"] = "compress_requested"
            elif a == "defer_new_explore":
                dpath = GOV / "explore_defer.json"
                GOV.mkdir(parents=True, exist_ok=True)
                dpath.write_text(
                    json.dumps({"defer_explore": True, "reason": "adenosine_high"}, indent=2)
                    + "\n"
                )
                r["ok"] = True
                r["detail"] = "explore_deferred"
            elif a in (
                "prefer_local_skills",
                "voice_warm",
                "bias_outcome_review",
                "bias_thoroughness",
            ):
                r["ok"] = True
                r["detail"] = "soft_flag"
            else:
                r["detail"] = "unknown_noop"
                r["ok"] = True
        except Exception as e:  # noqa: BLE001
            r["detail"] = str(e)[:200]
            r["ok"] = False
        results.append(r)
    return results


def tick(scoreboard: dict[str, Any] | None = None, dry_run: bool = False) -> dict[str, Any]:
    st = load_state()
    sig = gather_signals()
    if scoreboard:
        for k, v in scoreboard.items():
            if isinstance(v, (int, float)):
                sig[k] = float(v)

    levels = update_levels(st["levels"], st["baseline"], sig)
    actions = decide_actions(levels)
    results = apply_actions(actions, dry_run=dry_run)
    coupling = {} if dry_run else write_coupling(levels, actions)
    ci = control_impulsivity(levels)

    if any(r.get("action") == "run_sleep_consolidate" and r.get("ok") for r in results):
        levels["adenosine"] = _clamp(levels["adenosine"] - 0.08)
        # recompute actions after ADO relief so gate does not stick forever
        actions = decide_actions(levels)
        if not dry_run:
            coupling = write_coupling(levels, actions)

    out = {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "engine": "neuromod_v2_tight",
        "levels": levels,
        "baseline": st["baseline"],
        "control": ci["control"],
        "impulsivity": ci["impulsivity"],
        "signals": {k: round(float(v), 4) if isinstance(v, float) else v for k, v in sig.items()},
        "actions": actions,
        "action_results": results,
        "coupling": {
            "front_door_bias": str(FRONT_DOOR_BIAS),
            "schedule_gate": str(SCHEDULE_GATE),
            "da_local_ttl_s": DA_LOCAL_BOOST_TTL_S,
            "da_local_weight": DA_LOCAL_WEIGHT,
            "active": coupling,
        },
        "cycles": int(st.get("cycles") or 0) + 1,
        "law": {
            "never_self_cert": True,
            "seal_path": "forbidden",
            "biases": "schedule_and_consolidate_only",
            "not_agi": True,
        },
        "map": {
            "dopamine": "task_done_right + brief front_door prefer_LOCAL",
            "serotonin_high": "high_control sort/pin",
            "serotonin_low": "high_impulsivity faster teacher/bold",
            "adenosine": "consolidate WM + pause_grow_probes",
        },
    }

    GOV.mkdir(parents=True, exist_ok=True)
    if not dry_run:
        STATE_PATH.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
        with HIST_PATH.open("a", encoding="utf-8") as f:
            f.write(
                json.dumps(
                    {
                        "ts": out["ts"],
                        "levels": levels,
                        "control": ci["control"],
                        "impulsivity": ci["impulsivity"],
                        "actions": actions,
                        "local_hit": sig.get("local_hit"),
                        "promoted": sig.get("promoted"),
                    }
                )
                + "\n"
            )
    return out


def selftest() -> int:
    failures = 0

    def check(ok: bool, msg: str) -> None:
        nonlocal failures
        print(f"  {msg:56} {'PASS' if ok else 'FAIL'}")
        if not ok:
            failures += 1

    print("=== neuromod selftest (tight coupling) ===")
    hi = update_levels(
        dict(DEFAULT),
        dict(DEFAULT),
        {
            "local_hit": 0.95,
            "promoted": 3,
            "miss_n": 0,
            "reward_events": 5,
            "fail_pressure": 0.0,
            "wm_load": 0.1,
            "queue_pressure": 0.1,
            "da_affect": 0.8,
            "vigilance": 0.3,
            "calm": 0.7,
            "frustration": 0.2,
        },
    )
    check(hi["dopamine"] > DEFAULT["dopamine"], "dopamine rises on task-done-right")
    acts_da = decide_actions({**DEFAULT, "dopamine": 0.75, "serotonin": 0.55, "adenosine": 0.3})
    check("front_door_prefer_local_boost" in acts_da, "DA enables front_door LOCAL boost")

    # High 5HT = control
    hi5 = {**DEFAULT, "serotonin": 0.72, "dopamine": 0.5, "adenosine": 0.3}
    acts_c = decide_actions(hi5)
    ci = control_impulsivity(hi5)
    check(ci["control"] > ci["impulsivity"], "high 5HT → control > impulsivity")
    check("control_mode" in acts_c or "sort_task_queue" in acts_c, "high 5HT control actions")

    # Low 5HT = impulsivity
    lo5 = {**DEFAULT, "serotonin": 0.30, "dopamine": 0.5, "adenosine": 0.3}
    acts_i = decide_actions(lo5)
    ci2 = control_impulsivity(lo5)
    check(ci2["impulsivity"] > ci2["control"], "low 5HT → impulsivity > control")
    check("impulsivity_mode" in acts_i or "allow_teacher_faster" in acts_i, "low 5HT impulse actions")
    check("prefer_pin_memory" not in acts_i, "impulse mode does not pin memory")

    tired = update_levels(
        dict(DEFAULT),
        dict(DEFAULT),
        {
            "local_hit": 0.4,
            "promoted": 0,
            "miss_n": 8,
            "reward_events": 0,
            "fail_pressure": 0.5,
            "wm_load": 0.9,
            "queue_pressure": 0.6,
            "da_affect": 0.4,
            "vigilance": 0.5,
            "calm": 0.3,
            "frustration": 0.5,
        },
    )
    acts2 = decide_actions(tired)
    check(tired["adenosine"] > DEFAULT["adenosine"], "adenosine rises with WM load")
    check("pause_grow_probes" in acts2 or tired["adenosine"] < 0.58, "ADO pauses grow probes")

    for k, v in tired.items():
        check(0.15 <= v <= 0.85, f"{k} clamped")

    out = tick(dry_run=False)
    check(out.get("law", {}).get("never_self_cert") is True, "never_self_cert law")
    check(FRONT_DOOR_BIAS.is_file() or out["levels"]["dopamine"] < 0.62, "DA bias file when high DA")
    check(SCHEDULE_GATE.is_file(), "schedule_gate written")
    gate = _load_json(SCHEDULE_GATE, {}) or {}
    check("pause_grow_probes" in gate, "schedule_gate has pause_grow_probes")
    check("control" in out and "impulsivity" in out, "control/impulsivity axes present")

    # force DA high coupling write
    c = write_coupling({"dopamine": 0.8, "serotonin": 0.55, "adenosine": 0.3}, ["front_door_prefer_local_boost"])
    fb = c.get("front_door_bias") or {}
    check(float(fb.get("prefer_local_weight") or 0) >= 1.5, "prefer_local_weight boosted")
    check(bool(fb.get("disable_live_teacher")), "DA disables live teacher briefly")
    check(float(fb.get("expires_ts") or 0) > time.time(), "DA boost has future expiry")

    print(f"\nchecks done failures={failures}")
    if failures:
        print("NEUROMOD_FAIL")
        return 1
    print("NEUROMOD_PASS")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--test", action="store_true")
    ap.add_argument("--tick", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    if args.test:
        return selftest()
    out = tick(dry_run=args.dry_run)
    if args.json:
        print(json.dumps(out, indent=2))
    else:
        lv = out["levels"]
        print(
            f"DA={lv['dopamine']:.3f} 5HT={lv['serotonin']:.3f} ADO={lv['adenosine']:.3f} "
            f"control={out['control']:.3f} impulse={out['impulsivity']:.3f}"
        )
        print(f"actions={out['actions']}")
        print(f"state → {STATE_PATH}")
        print(f"front_door_bias → {FRONT_DOOR_BIAS}")
        print(f"schedule_gate → {SCHEDULE_GATE}")
    print("NEUROMOD_TICK_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
