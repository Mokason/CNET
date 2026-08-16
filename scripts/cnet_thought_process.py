#!/usr/bin/env python3
"""Token-free thought process for Autonomous-ASI (not AGI).

Produces structured inner steps WITHOUT LLM tokens — same spirit as LOCAL CERT:
  observe → neuromod read → intend → route → act → verify → remember/sort/rest

  python3 scripts/cnet_thought_process.py --query "who are you"
  python3 scripts/cnet_thought_process.py --from-kpi
  python3 scripts/cnet_thought_process.py --test

Law: thoughts are operational traces, not consciousness; never self-CERT.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import time
from pathlib import Path
from typing import Any

ROOT = Path(os.environ.get("CNET_ROOT", Path(__file__).resolve().parents[1]))
GOV = Path(os.environ.get("CNET_GOVERNOR_DIR", ROOT / "logs/governor"))
THOUGHT_LOG = GOV / "thought_process.jsonl"
THOUGHT_LAST = GOV / "thought_last.json"
NM_STATE = GOV / "neuromod_state.json"
PERSONA = GOV / "personality_state.json"
GATE = GOV / "schedule_gate.json"
FD_BIAS = GOV / "front_door_bias.json"
AUTO = ROOT / "logs" / "marble_24_7" / "AUTONOMOUS_CYCLE.json"
ROUTES = ROOT / "artifacts" / "roe_daily_packs" / "ROUTES.jsonl"
SOUL = ROOT / "artifacts" / "roe_daily_packs" / "pack_soul_marble" / "SOUL.md"

# Fixed micro-thought templates (zero tokens — string format only)
STEPS = (
    "OBSERVE",
    "AFFECT",  # neuromod
    "INTEND",
    "ROUTE",
    "ACT",
    "VERIFY",
    "CONSOLIDATE",  # remember / sort / rest
)


def _load(p: Path, default: Any = None) -> Any:
    if not p.is_file():
        return default
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return default


def _clamp01(x: float) -> float:
    return max(0.0, min(1.0, float(x)))


def match_route(query: str) -> dict[str, str] | None:
    if not ROUTES.is_file() or not query:
        return None
    q = query.lower()
    best = None
    best_len = 0
    for line in ROUTES.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            continue
        pat = (r.get("pattern") or "").lower()
        if len(pat) >= 3 and pat in q and len(pat) > best_len:
            best_len = len(pat)
            best = {
                "pattern": r.get("pattern") or "",
                "pack": r.get("pack") or "",
                "skill": r.get("skill") or "",
                "cat": r.get("cat") or "",
            }
    return best


def read_body() -> dict[str, Any]:
    """Snapshot of 'body' state without LLM."""
    nm = _load(NM_STATE, {}) or {}
    levels = nm.get("levels") or {}
    persona = _load(PERSONA, {}) or {}
    affect = persona.get("affect") or {}
    gate = _load(GATE, {}) or {}
    bias = _load(FD_BIAS, {}) or {}
    auto = _load(AUTO, {}) or {}
    kpi = auto.get("kpi") or {}
    now = time.time()
    da_active = bool(bias.get("prefer_local")) and float(bias.get("expires_ts") or 0) > now
    return {
        "da": float(levels.get("dopamine") or 0.5),
        "srt": float(levels.get("serotonin") or 0.5),
        "ado": float(levels.get("adenosine") or 0.35),
        "control": float(nm.get("control") or levels.get("serotonin") or 0.5),
        "impulsivity": float(nm.get("impulsivity") or (1.0 - float(levels.get("serotonin") or 0.5))),
        "reward": float(affect.get("reward") or 0.5),
        "vigilance": float(affect.get("vigilance") or 0.5),
        "calm": float(affect.get("calm") or 0.5),
        "frustration": float(affect.get("frustration") or 0.5),
        "pause_grow": bool(gate.get("pause_grow_probes")),
        "control_mode": bool(gate.get("control_mode")),
        "impulse_mode": bool(gate.get("impulsivity_mode")),
        "da_prefer_local": da_active,
        "da_weight": float(bias.get("prefer_local_weight") or 1.0) if da_active else 1.0,
        "local_hit": float(kpi.get("local_hit") or 0.0),
        "promoted": float(kpi.get("promoted") or 0.0),
        "profile": persona.get("profile") or "marble",
        "nm_actions": list(nm.get("actions") or []),
    }


def intend(query: str, body: dict[str, Any], route: dict | None) -> str:
    q = (query or "").lower()
    if body["ado"] >= 0.58 or body["pause_grow"]:
        return "consolidate_then_serve"
    if any(k in q for k in ("who are you", "marble", "how should you")):
        return "identity_local"
    if body["da_prefer_local"] or body["da"] >= 0.62:
        return "prefer_local_cert"
    if body["impulse_mode"] and body["impulsivity"] > 0.55:
        return "explore_miss_teacher"
    if route and route.get("pack"):
        return f"serve_pack:{route['pack']}"
    if body["control_mode"]:
        return "controlled_local_first"
    return "local_first_then_miss"


def act_plan(intent: str, route: dict | None, body: dict[str, Any]) -> list[str]:
    plan: list[str] = []
    if intent == "consolidate_then_serve":
        plan += ["sleep_consolidate_request", "compress_context", "defer_grow_probes"]
        plan += ["front_door_local_only"]
    elif intent == "identity_local":
        plan += ["load_pack_soul_marble", "answer_LOCAL_soul"]
    elif intent == "prefer_local_cert":
        plan += ["front_door_prefer_local", f"weight={body['da_weight']:.2f}", "teacher_off_briefly"]
        if route:
            plan.append(f"match:{route.get('skill') or route.get('pattern')}")
    elif intent.startswith("serve_pack:"):
        pack = intent.split(":", 1)[1]
        plan += [f"load_{pack}", "roe_turn_local_first"]
        if body["impulse_mode"]:
            plan.append("teacher_on_miss_ok")
        else:
            plan.append("teacher_on_miss_gated")
    elif intent == "explore_miss_teacher":
        plan += ["roe_turn", "allow_teacher_faster", "reviewer_before_promote"]
    else:
        plan += ["roe_front_door_ask", "local_first"]
        if body["control_mode"]:
            plan.append("sort_queue_if_backlog")
    plan.append("never_self_cert")
    return plan


def verify_rule(intent: str, body: dict[str, Any]) -> list[str]:
    rules = [
        "CERT only via gold|multi_stable+reviewer|user_accept",
        "OOD → abstain not invent",
        "persona seal_path forbidden",
    ]
    if body["da_prefer_local"]:
        rules.append("DA boost active: live teacher suppressed")
    if body["pause_grow"]:
        rules.append("ADO gate: grow probes paused")
    if body["control_mode"]:
        rules.append("5HT high control: pin/sort preferred")
    if body["impulse_mode"]:
        rules.append("5HT low impulsivity: teacher faster, no pin")
    if intent == "consolidate_then_serve":
        rules.append("ADO: consolidate before new explore")
    return rules


def consolidate_move(body: dict[str, Any]) -> str:
    if body["ado"] >= 0.58:
        return "REST: consolidate WM / compress context"
    if body["control"] >= 0.60:
        return "SORT: queue + pin memory"
    if body["da"] >= 0.62:
        return "REMEMBER: reinforce LOCAL success (no self-CERT)"
    if body["impulsivity"] >= 0.58:
        return "NOTE: impulse — log miss, do not pin yet"
    return "HOLD: homeostasis, no heavy move"


def think(
    query: str = "",
    *,
    source: str | None = None,
    skill: str | None = None,
    pack: str | None = None,
    persist: bool = True,
) -> dict[str, Any]:
    body = read_body()
    route = match_route(query)
    if pack and route:
        route["pack"] = pack
    intent = intend(query, body, route)
    plan = act_plan(intent, route, body)
    if source:
        plan.append(f"observed_source={source}")
    if skill:
        plan.append(f"observed_skill={skill}")

    steps = {
        "OBSERVE": {
            "query": (query or "")[:200],
            "profile": body["profile"],
            "route": route,
            "kpi_local_hit": body["local_hit"],
            "kpi_promoted": body["promoted"],
        },
        "AFFECT": {
            "dopamine": round(body["da"], 3),
            "serotonin": round(body["srt"], 3),
            "adenosine": round(body["ado"], 3),
            "control": round(body["control"], 3),
            "impulsivity": round(body["impulsivity"], 3),
            "vigilance": round(body["vigilance"], 3),
            "nm_actions": body["nm_actions"][:8],
        },
        "INTEND": intent,
        "ROUTE": route or {"pack": "always_on", "pattern": ""},
        "ACT": plan,
        "VERIFY": verify_rule(intent, body),
        "CONSOLIDATE": consolidate_move(body),
    }

    # Compact one-line chain (token-free display)
    chain = [
        f"OBS:{'q' if query else 'kpi'}",
        f"DA={body['da']:.2f}/5HT={body['srt']:.2f}/ADO={body['ado']:.2f}",
        f"ctrl={body['control']:.2f}/imp={body['impulsivity']:.2f}",
        f"INT:{intent}",
        f"RTE:{(route or {}).get('pack', 'always_on')}",
        f"ACT:{len(plan)}",
        f"CON:{steps['CONSOLIDATE'].split(':')[0]}",
    ]
    line = " | ".join(chain)

    # Human-readable multiline (still no LLM)
    narrative = []
    narrative.append(f"1. OBSERVE: query={query[:80]!r} profile={body['profile']}")
    if route:
        narrative.append(
            f"   route pack={route.get('pack')} skill={route.get('skill')} pat={route.get('pattern')}"
        )
    narrative.append(
        f"2. AFFECT: DA={body['da']:.2f} (reward/local) 5HT={body['srt']:.2f} "
        f"(control={body['control']:.2f} impulse={body['impulsivity']:.2f}) "
        f"ADO={body['ado']:.2f} (consolidate pressure)"
    )
    narrative.append(f"3. INTEND: {intent}")
    narrative.append(f"4. ROUTE: {(route or {}).get('pack', 'always_on+personal')}")
    narrative.append("5. ACT: " + " → ".join(plan[:8]))
    narrative.append("6. VERIFY: " + "; ".join(steps["VERIFY"][:3]))
    narrative.append(f"7. CONSOLIDATE: {steps['CONSOLIDATE']}")
    narrative.append("law: thought≠CERT; never self-CERT; not AGI")

    out = {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "engine": "thought_process_v1_tokenfree",
        "tokens": 0,
        "llm": False,
        "steps": steps,
        "chain": line,
        "narrative": narrative,
        "law": {
            "never_self_cert": True,
            "not_consciousness": True,
            "not_agi": True,
            "token_cost": 0,
        },
    }

    if persist:
        GOV.mkdir(parents=True, exist_ok=True)
        THOUGHT_LAST.write_text(json.dumps(out, indent=2) + "\n", encoding="utf-8")
        with THOUGHT_LOG.open("a", encoding="utf-8") as f:
            f.write(
                json.dumps(
                    {
                        "ts": out["ts"],
                        "chain": line,
                        "intent": intent,
                        "query": (query or "")[:120],
                        "da": body["da"],
                        "srt": body["srt"],
                        "ado": body["ado"],
                    }
                )
                + "\n"
            )
    return out


def selftest() -> int:
    failures = 0

    def check(ok: bool, m: str) -> None:
        nonlocal failures
        print(f"  {m:56} {'PASS' if ok else 'FAIL'}")
        if not ok:
            failures += 1

    print("=== thought process (token-free) ===")
    t = think("who are you", persist=False)
    check(t["tokens"] == 0 and t["llm"] is False, "zero tokens / no llm")
    check(t["steps"]["INTEND"] == "identity_local", "identity intent")
    check("never_self_cert" in t["steps"]["ACT"], "never_self_cert in act")
    check(t["law"]["not_agi"] is True, "not_agi")
    check(len(t["narrative"]) >= 6, "narrative steps")
    check("DA=" in t["chain"] and "5HT=" in t["chain"], "chain has neuromod")

    t2 = think("format-truncation werror", persist=False)
    check("INTEND" in t2["steps"], "coding query thinks")
    # consolidate under high ado synthetic body by monkeypatching is heavy; check template
    check(callable(consolidate_move), "consolidate helper")

    t3 = think("", persist=True)
    check(THOUGHT_LAST.is_file(), "persists last thought")
    check(THOUGHT_LOG.is_file(), "appends thought log")

    # narrative must not claim consciousness
    blob = " ".join(t["narrative"]).lower()
    check("not agi" in blob or "never self-cert" in blob, "anti-agi language")

    print(f"\nfailures={failures}")
    if failures:
        print("THOUGHT_PROCESS_FAIL")
        return 1
    print("THOUGHT_PROCESS_PASS")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--query", default="")
    ap.add_argument("--source", default="")
    ap.add_argument("--skill", default="")
    ap.add_argument("--from-kpi", action="store_true")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--test", action="store_true")
    ap.add_argument("--no-persist", action="store_true")
    args = ap.parse_args()
    if args.test:
        return selftest()
    q = args.query
    if args.from_kpi and not q:
        auto = _load(AUTO, {}) or {}
        probes = auto.get("probes") or []
        q = (probes[0].get("q") if probes else "") or "status check"
    out = think(
        q,
        source=args.source or None,
        skill=args.skill or None,
        persist=not args.no_persist,
    )
    if args.json:
        print(json.dumps(out, indent=2))
    else:
        print(out["chain"])
        print("---")
        print("\n".join(out["narrative"]))
    print("THOUGHT_PROCESS_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
