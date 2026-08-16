#!/usr/bin/env python3
"""Continuity workspace — imitate continuity of control, NOT consciousness.

Binds (0 LLM tokens):
  body   = neuromod DA/5HT/ADO + persona affect
  mind   = token-free thought chain
  place  = route / packs / LOCAL|MISS
  story  = who / have / miss / law
  sleep  = ADO consolidate flag

Law:
  not_conscious: true
  not_agi: true
  never_self_cert: true
  continuity ≠ CERT (never promote from this alone)

  python3 scripts/cnet_continuity.py --query "who are you"
  python3 scripts/cnet_continuity.py --test
  make cnet_continuity
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path
from typing import Any

ROOT = Path(os.environ.get("CNET_ROOT", Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(ROOT / "scripts"))

GOV = Path(os.environ.get("CNET_GOVERNOR_DIR", ROOT / "logs/governor"))
OUT_LAST = GOV / "continuity_last.json"
OUT_LOG = GOV / "continuity.jsonl"
OUT_LINE = GOV / "continuity_line.txt"
PERSONA = GOV / "personality_state.json"
NM = GOV / "neuromod_state.json"
THOUGHT = GOV / "thought_last.json"
GATE = GOV / "schedule_gate.json"
FD_BIAS = GOV / "front_door_bias.json"
AUTO = ROOT / "logs" / "marble_24_7" / "AUTONOMOUS_CYCLE.json"
SOUL_MD = ROOT / "artifacts" / "roe_daily_packs" / "pack_soul_marble" / "SOUL.md"
PACK_PERSONAL = ROOT / "artifacts" / "roe_daily_packs" / "pack_personal" / "catalog.jsonl"


def _load(p: Path, default: Any = None) -> Any:
    if not p.is_file():
        return default
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return default


def _count_lines(p: Path) -> int:
    if not p.is_file():
        return 0
    try:
        return sum(1 for _ in p.open("r", encoding="utf-8", errors="ignore") if _.strip())
    except OSError:
        return 0


def _soul_oneline() -> str:
    if SOUL_MD.is_file():
        for line in SOUL_MD.read_text(encoding="utf-8", errors="replace").splitlines():
            if "Marble:" in line or line.strip().startswith("Marble"):
                return line.strip()[:120]
    return "Marble: zen spine — sit, see, then ship."


def snapshot(
    query: str = "",
    *,
    source: str = "",
    skill: str = "",
    pack: str = "",
    run_thought: bool = True,
    persist: bool = True,
) -> dict[str, Any]:
    """Build one continuity frame (token-free)."""
    # optional fresh thought
    thought = _load(THOUGHT, {}) or {}
    if run_thought and query:
        try:
            import cnet_thought_process as tp  # type: ignore

            thought = tp.think(
                query,
                source=source or None,
                skill=skill or None,
                persist=True,
            )
        except Exception:
            pass

    persona = _load(PERSONA, {}) or {}
    nm = _load(NM, {}) or {}
    levels = nm.get("levels") or {}
    affect = persona.get("affect") or {}
    gate = _load(GATE, {}) or {}
    bias = _load(FD_BIAS, {}) or {}
    auto = _load(AUTO, {}) or {}
    kpi = auto.get("kpi") or {}

    da = float(levels.get("dopamine") or affect.get("reward") or 0.5)
    srt = float(levels.get("serotonin") or 0.5)
    ado = float(levels.get("adenosine") or 0.35)
    control = float(nm.get("control") or srt)
    impulse = float(nm.get("impulsivity") or (1.0 - srt))
    profile = persona.get("profile") or persona.get("title") or "marble"

    now = time.time()
    da_local = bool(bias.get("prefer_local")) and float(bias.get("expires_ts") or 0) > now
    tsteps = (thought or {}).get("steps") or {}
    intent = tsteps.get("INTEND") or "hold"
    route = tsteps.get("ROUTE") or {}
    if isinstance(route, dict):
        rpack = pack or route.get("pack") or "always_on"
        rskill = skill or route.get("skill") or ""
    else:
        rpack, rskill = pack or "always_on", skill or ""

    n_personal = _count_lines(PACK_PERSONAL)
    local_hit = float(kpi.get("local_hit") or 0.0)
    promoted = int(kpi.get("promoted") or 0)

    # HAVE / MISS (inventory language — not feelings-as-truth)
    have = []
    if local_hit >= 0.5:
        have.append(f"local_hit={local_hit:.2f}")
    if n_personal:
        have.append(f"personal_skills={n_personal}")
    if rskill:
        have.append(f"skill={rskill}")
    if da_local:
        have.append("da_prefer_local")
    if not have:
        have.append("baseline_packs")

    miss = []
    if source in ("LLM", "ASK_USER", "ABSTAIN") or (source and source != "LOCAL"):
        miss.append(f"source={source or 'unknown'}")
    if gate.get("pause_grow_probes"):
        miss.append("grow_paused_ado")
    if local_hit < 0.5 and kpi:
        miss.append("local_hit_low")
    if not miss:
        miss.append("none_open")

    sleep = "wake"
    if ado >= 0.58 or gate.get("pause_grow_probes"):
        sleep = "consolidate"
    elif ado >= 0.50:
        sleep = "drowsy"

    ctrl_label = "ctrl>imp" if control >= impulse else "imp>ctrl"

    # Primary one-liner for UI / logs
    continuity_line = (
        f"continuity: {profile} | DA{da:.2f} 5HT{srt:.2f} ADO{ado:.2f} | {ctrl_label} | "
        f"doing: {intent} @ {rpack}"
        + (f"/{rskill}" if rskill else "")
        + f" | have: {','.join(have[:3])} | miss: {','.join(miss[:3])} | "
        f"sleep: {sleep} | law: never_self_cert · not_conscious · not_agi"
    )

    body = {
        "dopamine": round(da, 3),
        "serotonin": round(srt, 3),
        "adenosine": round(ado, 3),
        "control": round(control, 3),
        "impulsivity": round(impulse, 3),
        "affect": {
            "reward": affect.get("reward"),
            "vigilance": affect.get("vigilance"),
            "calm": affect.get("calm"),
            "frustration": affect.get("frustration"),
        },
        "da_prefer_local": da_local,
        "nm_actions": list(nm.get("actions") or [])[:8],
    }
    mind = {
        "engine": (thought or {}).get("engine") or "thought_process_v1_tokenfree",
        "chain": (thought or {}).get("chain") or "",
        "intent": intent,
        "tokens": 0,
        "llm": False,
    }
    place = {
        "query": (query or "")[:200],
        "pack": rpack,
        "skill": rskill,
        "source": source or (tsteps.get("OBSERVE") or {}).get("source") or "",
        "route": route if isinstance(route, dict) else {},
    }
    story = {
        "who": _soul_oneline(),
        "profile": profile,
        "have": have,
        "miss": miss,
        "promoted_last": promoted,
        "personal_skills": n_personal,
    }

    frame = {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "engine": "continuity_workspace_v1",
        "tokens": 0,
        "llm": False,
        "body": body,
        "mind": mind,
        "place": place,
        "story": story,
        "sleep": sleep,
        "continuity_line": continuity_line,
        "thought_chain": mind.get("chain") or "",
        "law": {
            "not_conscious": True,
            "not_agi": True,
            "never_self_cert": True,
            "continuity_is_not_cert": True,
            "imitates": "continuity_of_control_and_self_report",
            "does_not_instantiate": "phenomenal_consciousness",
        },
    }

    if persist:
        GOV.mkdir(parents=True, exist_ok=True)
        OUT_LAST.write_text(json.dumps(frame, indent=2) + "\n", encoding="utf-8")
        OUT_LINE.write_text(continuity_line + "\n", encoding="utf-8")
        with OUT_LOG.open("a", encoding="utf-8") as f:
            f.write(
                json.dumps(
                    {
                        "ts": frame["ts"],
                        "line": continuity_line,
                        "intent": intent,
                        "da": da,
                        "srt": srt,
                        "ado": ado,
                        "sleep": sleep,
                    }
                )
                + "\n"
            )
    return frame


def selftest() -> int:
    failures = 0

    def check(ok: bool, m: str) -> None:
        nonlocal failures
        print(f"  {m:56} {'PASS' if ok else 'FAIL'}")
        if not ok:
            failures += 1

    print("=== continuity workspace ===")
    f = snapshot("who are you", persist=True)
    check(f["tokens"] == 0 and f["llm"] is False, "zero tokens")
    check(f["law"]["not_conscious"] is True, "not_conscious")
    check(f["law"]["not_agi"] is True, "not_agi")
    check(f["law"]["never_self_cert"] is True, "never_self_cert")
    check(f["law"]["continuity_is_not_cert"] is True, "continuity≠cert")
    check("continuity:" in f["continuity_line"], "continuity_line format")
    check("not_conscious" in f["continuity_line"], "line states not_conscious")
    check(f["mind"]["intent"] in ("identity_local", "prefer_local_cert", "controlled_local_first", "serve_pack:pack_soul_marble") or "soul" in str(f["mind"]["intent"]) or "identity" in str(f["mind"]["intent"]) or f["place"]["pack"] == "pack_soul_marble", "identity path")
    check(OUT_LAST.is_file() and OUT_LINE.is_file(), "persists last + line")
    check("body" in f and "mind" in f and "place" in f and "story" in f, "five blocks")
    # must not claim consciousness
    blob = json.dumps(f).lower()
    check("i am conscious" not in blob and "sentient" not in blob, "no consciousness claim")
    f2 = snapshot("", run_thought=False, persist=False)
    check("continuity:" in f2["continuity_line"], "works without query")

    print(f"\nfailures={failures}")
    if failures:
        print("CONTINUITY_FAIL")
        return 1
    print("CONTINUITY_PASS")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--query", default="")
    ap.add_argument("--source", default="")
    ap.add_argument("--skill", default="")
    ap.add_argument("--pack", default="")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--line-only", action="store_true")
    ap.add_argument("--test", action="store_true")
    ap.add_argument("--no-thought", action="store_true")
    ap.add_argument("--no-persist", action="store_true")
    args = ap.parse_args()
    if args.test:
        return selftest()
    frame = snapshot(
        args.query,
        source=args.source,
        skill=args.skill,
        pack=args.pack,
        run_thought=not args.no_thought,
        persist=not args.no_persist,
    )
    if args.line_only:
        print(frame["continuity_line"])
    elif args.json:
        print(json.dumps(frame, indent=2))
    else:
        print(frame["continuity_line"])
        if frame.get("thought_chain"):
            print("thought:", frame["thought_chain"])
        print(
            f"body DA={frame['body']['dopamine']:.2f} 5HT={frame['body']['serotonin']:.2f} "
            f"ADO={frame['body']['adenosine']:.2f} sleep={frame['sleep']}"
        )
        print(f"story who={frame['story']['who'][:80]}")
        print(f"have={frame['story']['have']} miss={frame['story']['miss']}")
    print("CONTINUITY_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
