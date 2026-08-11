#!/usr/bin/env python3
"""CNET neuromod organ — biological metaphor, not AGI.

Channels (homeostatic, clamped; NEVER seal/CERT path):
  dopamine  (DA)  — task-done-right reward; reinforce promote/LOCAL success
  serotonin (5HT) — remember vs sort/queue; stable order under load
  adenosine (ADO) — need to process / reorganize / consolidate WM+context

Law:
  - biases scheduling & consolidation ONLY
  - never self-CERT, never lowers floors
  - homeostasis pulls to baseline each tick

  python3 scripts/cnet_neuromod.py --test
  python3 scripts/cnet_neuromod.py --tick   # one cycle from latest KPIs
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

# baselines (Marble-ish calm competence)
DEFAULT = {
    "dopamine": 0.50,
    "serotonin": 0.55,
    "adenosine": 0.35,
}

ACTIONS = {
    # when DA high: prefer reinforce / celebrate competence (still no seal)
    "high_da": ["prefer_local_skills", "voice_warm", "bias_outcome_review"],
    # when 5HT high: sort queue, pin memory, stable order
    "high_5ht": ["sort_task_queue", "prefer_pin_memory", "bias_thoroughness"],
    # when ADO high: consolidate / sleep / reorganize context
    "high_ado": ["run_sleep_consolidate", "compress_context", "defer_new_explore"],
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

    # --- Serotonin: remember vs sort queue ---
    # rises when calm + need order (queue pressure without panic)
    # high vigilance/frustration lowers 5HT (chaotic mode)
    srt_pulse = 0.50
    srt_pulse += 0.25 * sig["calm"]
    srt_pulse += 0.20 * sig["queue_pressure"]  # need to sort
    srt_pulse -= 0.25 * sig["vigilance"]
    srt_pulse -= 0.15 * sig["frustration"]
    srt = 0.70 * srt + 0.30 * srt_pulse

    # --- Adenosine: need consolidate / reorganize ---
    # accumulates with WM load, miss backlog, time-on-task proxy (cycles without sleep)
    ado_pulse = 0.25
    ado_pulse += 0.45 * sig["wm_load"]
    ado_pulse += 0.25 * sig["queue_pressure"]
    ado_pulse += 0.10 * min(1.0, sig["miss_n"] / 10.0)
    # successful consolidate would clear ADO externally; here success lowers slightly via DA
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


def decide_actions(levels: dict[str, float], thr: dict[str, float] | None = None) -> list[str]:
    thr = thr or {"da": 0.62, "5ht": 0.60, "ado": 0.58}
    acts: list[str] = []
    if levels["dopamine"] >= thr["da"]:
        acts.extend(ACTIONS["high_da"])
    if levels["serotonin"] >= thr["5ht"]:
        acts.extend(ACTIONS["high_5ht"])
    if levels["adenosine"] >= thr["ado"]:
        acts.extend(ACTIONS["high_ado"])
    # mutual inhibition: very high ADO suppresses explore-ish DA actions
    if levels["adenosine"] >= 0.70:
        acts = [a for a in acts if a not in ("voice_warm",)]
        if "run_sleep_consolidate" not in acts:
            acts.append("run_sleep_consolidate")
    # high 5HT + high queue → force sort first
    if levels["serotonin"] >= 0.65 and "sort_task_queue" not in acts:
        acts.insert(0, "sort_task_queue")
    # de-dupe preserve order
    seen = set()
    out = []
    for a in acts:
        if a not in seen:
            seen.add(a)
            out.append(a)
    return out


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
                # Prefer real sleep consolidate test binary if present; else mark intent
                bin_path = ROOT / "bin" / "test_cnet_sleep_consolidate"
                if not bin_path.is_file():
                    # try make target lightly
                    r["detail"] = "sleep_consolidate_intent"
                    r["ok"] = True
                    # write a consolidate request for governor/ops
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
                # Rewrite curriculum order: LOCAL-first tags first in a queue snapshot
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
                            "reason": "serotonin_high",
                            "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                        },
                        indent=2,
                    )
                    + "\n"
                )
                r["ok"] = True
                r["detail"] = "pin_pref_on"
            elif a == "compress_context":
                # Request context compression (ops reads this)
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
                # soft flags for voice / governor bias consumers
                fpath = GOV / "neuromod_soft_bias.json"
                prev = _load_json(fpath, {}) or {}
                flags = set(prev.get("flags") or [])
                flags.add(a)
                fpath.parent.mkdir(parents=True, exist_ok=True)
                fpath.write_text(
                    json.dumps(
                        {
                            "flags": sorted(flags),
                            "ts": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                        },
                        indent=2,
                    )
                    + "\n"
                )
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
        # allow governor to inject
        for k, v in scoreboard.items():
            if isinstance(v, (int, float)):
                sig[k] = float(v)

    levels = update_levels(st["levels"], st["baseline"], sig)
    actions = decide_actions(levels)
    results = apply_actions(actions, dry_run=dry_run)

    # successful consolidate request slightly clears ADO for next cycle
    if any(r.get("action") == "run_sleep_consolidate" and r.get("ok") for r in results):
        levels["adenosine"] = _clamp(levels["adenosine"] - 0.08)

    out = {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "engine": "neuromod_v1",
        "levels": levels,
        "baseline": st["baseline"],
        "signals": {k: round(float(v), 4) if isinstance(v, float) else v for k, v in sig.items()},
        "actions": actions,
        "action_results": results,
        "cycles": int(st.get("cycles") or 0) + 1,
        "law": {
            "never_self_cert": True,
            "seal_path": "forbidden",
            "biases": "schedule_and_consolidate_only",
            "not_agi": True,
        },
        "map": {
            "dopamine": "task_done_right_reward",
            "serotonin": "remember_vs_sort_queue",
            "adenosine": "process_reorganize_consolidate_wm",
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

    print("=== neuromod selftest ===")
    # reward → DA up
    st = {
        "levels": dict(DEFAULT),
        "baseline": dict(DEFAULT),
    }
    hi = update_levels(
        st["levels"],
        st["baseline"],
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

    # queue → 5HT sort
    mid = update_levels(
        dict(DEFAULT),
        dict(DEFAULT),
        {
            "local_hit": 0.5,
            "promoted": 0,
            "miss_n": 4,
            "reward_events": 0,
            "fail_pressure": 0.4,
            "wm_load": 0.3,
            "queue_pressure": 0.8,
            "da_affect": 0.5,
            "vigilance": 0.35,
            "calm": 0.65,
            "frustration": 0.3,
        },
    )
    acts = decide_actions(mid)
    check(mid["serotonin"] >= 0.5, "serotonin responds to queue pressure")
    check(
        "sort_task_queue" in acts or mid["serotonin"] < 0.60,
        "sort_queue offered when 5HT high enough or below thr",
    )

    # WM load → ADO consolidate
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
    check("run_sleep_consolidate" in acts2 or tired["adenosine"] < 0.58, "consolidate when ADO high")

    # clamps
    for k, v in tired.items():
        check(0.15 <= v <= 0.85, f"{k} clamped")

    # dry tick
    out = tick(dry_run=True)
    check(out.get("law", {}).get("never_self_cert") is True, "never_self_cert law")
    check(out.get("law", {}).get("not_agi") is True, "not_agi law")
    check(set(out["levels"]) == {"dopamine", "serotonin", "adenosine"}, "three channels")

    # live tick write
    out2 = tick(dry_run=False)
    check(STATE_PATH.is_file(), "state written")
    check(out2["cycles"] >= 1, "cycles increment")

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
            f"actions={out['actions']}"
        )
        print(f"state → {STATE_PATH}")
    print("NEUROMOD_TICK_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
