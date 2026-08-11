#!/usr/bin/env python3
"""CNET autonomous cycle — do work without Hermes or human go/accept.

Not AGI. One tick:
  1) health snap
  2) probe curriculum queries through roe_front_door (LOCAL or MISS)
  3) optional teacher fill on MISS (Ollama cloud)
  4) evolve_tick (gold | multi_stable + reviewer → pack_personal)
  5) optional light autoteach if script present
  6) write AUTONOMOUS_CYCLE.json KPI

Law:
  - never self-CERT from one LLM shot
  - reviewer gate on non-gold promotes
  - persona packs not auto-rewritten
  - hermes not required

  python3 scripts/cnet_autonomous_cycle.py
  make cnet_autonomous
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))

OUT_DIR = ROOT / "logs" / "marble_24_7"
REPORT = OUT_DIR / "AUTONOMOUS_CYCLE.json"
CURRIC = ROOT / "config" / "autonomous_curriculum.jsonl"
_MIN = os.environ.get("CNET_MINIMAL_ROOT", "").strip()
_minp = Path(_MIN) if _MIN else None
PACKS = Path(
    os.environ.get("CNET_PACKS_ROOT")
    or (
        str(_minp / "data" / "roe_daily_packs")
        if _minp and (_minp / "data" / "roe_daily_packs").is_dir()
        else ROOT / "artifacts" / "roe_daily_packs"
    )
)
MISS = PACKS / "miss_log.jsonl"
BIN_FD = Path(
    os.environ.get("CNET_FRONT_DOOR_BIN")
    or (
        str(_minp / "bin" / "roe_front_door")
        if _minp and (_minp / "bin" / "roe_front_door").is_file()
        else ROOT / "bin" / "roe_front_door"
    )
)


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def run(cmd: list[str], timeout: int = 300, env: dict | None = None) -> tuple[int, str]:
    try:
        p = subprocess.run(
            cmd,
            cwd=str(ROOT),
            capture_output=True,
            text=True,
            timeout=timeout,
            env=env or os.environ.copy(),
        )
        out = (p.stdout or "") + (p.stderr or "")
        return p.returncode, out
    except (OSError, subprocess.SubprocessError) as e:
        return 1, str(e)


def ensure_front_door() -> bool:
    if BIN_FD.is_file() and os.access(BIN_FD, os.X_OK):
        return True
    rc, out = run(
        [
            "gcc",
            "-std=c11",
            "-Wall",
            "-O2",
            "-D_POSIX_C_SOURCE=200809L",
            "-Iinclude",
            "-o",
            str(BIN_FD),
            "src/cnet_roe_asi.c",
            "src/cnet_roe_net.c",
            "src/cnet_asi_improve.c",
            "tools/roe_front_door.c",
            "-lm",
            "-lcurl",
        ],
        timeout=120,
    )
    return rc == 0 and BIN_FD.is_file()


def default_curriculum() -> list[dict]:
    """Built-in probes so autonomy works with zero Hermes traffic."""
    return [
        {"q": "who are you", "want": "LOCAL", "tag": "soul"},
        {"q": "never self-cert rule", "want": "LOCAL", "tag": "law"},
        {"q": "use tools not guess file state", "want": "LOCAL", "tag": "toolcall"},
        {"q": "how should you talk", "want": "LOCAL", "tag": "soul"},
        {"q": "vigilance high mode", "want": "LOCAL", "tag": "soul"},
        {"q": "local first ocr router", "want": "LOCAL", "tag": "ocr"},
        {"q": "format-truncation werror path join", "want": "LOCAL", "tag": "coding"},
        {"q": "roe front door selective load", "want": "any", "tag": "roe"},
        {"q": "pack personal evolve tick", "want": "any", "tag": "evolve"},
        {"q": "teacher rate under ten percent warm", "want": "any", "tag": "kpi"},
        # deliberate soft OOD — should miss/abstain, feed evolve
        {"q": "autonomous cycle probe novel fact alpha-seven", "want": "MISS", "tag": "grow"},
    ]


def load_curriculum() -> list[dict]:
    rows = default_curriculum()
    if CURRIC.is_file():
        for line in CURRIC.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    # de-dupe by q
    seen = set()
    out = []
    for r in rows:
        q = (r.get("q") or r.get("query") or "").strip()
        if not q or q in seen:
            continue
        seen.add(q)
        out.append({"q": q, "want": r.get("want", "any"), "tag": r.get("tag", "")})
    return out[:40]


def parse_fd(out: str) -> dict:
    src = "UNKNOWN"
    miss = 0
    skill = ""
    ans = ""
    for line in out.splitlines():
        if "source=LOCAL" in line or "src=LOCAL" in line:
            src = "LOCAL"
        if "source=ASK_USER" in line or "src=ASK_USER" in line:
            src = "ASK_USER"
        if "source=LLM" in line or "src=LLM" in line:
            src = "LLM"
        if "source=ABSTAIN" in line:
            src = "ABSTAIN"
        if "miss=1" in line:
            miss = 1
        if "miss=0" in line:
            miss = 0
        m = re.search(r"skill=([A-Za-z0-9_]+)", line)
        if m:
            skill = m.group(1)
        if line.startswith("A:"):
            ans = line[2:].strip()
    if src == "UNKNOWN" and "LOCAL" in out:
        src = "LOCAL"
    return {"source": src, "miss": miss, "skill": skill, "answer": ans}


def fd_env(live: bool) -> dict:
    env = os.environ.copy()
    # load envs
    for rel in (
        "config/roe-teacher-ollama-cloud.env",
        "config/roe-reviewer-ollama-cloud.env",
    ):
        p = ROOT / rel
        if not p.is_file():
            continue
        for line in p.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            env.setdefault(k.strip(), v.strip().strip('"'))
    if live:
        env["ROE_LIVE"] = "1"
        env["ROE_LLM"] = "1"
        env["ROE_LOOKUP"] = env.get("ROE_LOOKUP", "0")
        env["ROE_LLM_THINK"] = env.get("ROE_LLM_THINK", "0")
    else:
        env["ROE_LIVE"] = "0"
        env["ROE_LLM"] = "0"
    return env


def is_probe_query(q: str) -> bool:
    """Curriculum/soak probes — never call Teacher (short-circuit)."""
    nq = (q or "").lower()
    sigs = (
        "novel fact",
        "mystic ooze",
        "zz99",
        "zzqq",
        "zz unknown",
        "zz_ood",
        "brand new teacher only",
        "say pong only",
        "say hello teacher",
        "autonomous cycle probe",
        "deploy probe unique",
        "teacher only query",
        "quantum flute",
        "galactic overmind",
        "orbital printer",
        "alien haskell",
        "banjo merge",
        "soup kitchen narrative",
    )
    # optional config overlay
    cfg = ROOT / "config" / "probe_shortcircuit.txt"
    extra = []
    if cfg.is_file():
        for line in cfg.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                extra.append(line.lower())
    for s in list(sigs) + extra:
        if s and s in nq:
            return True
    return False


def probe(q: str, live: bool) -> dict:
    env = fd_env(live=live)
    # Force fail-closed on probe signatures even if caller requested live teacher
    sc = is_probe_query(q)
    if sc:
        live = False
        env = fd_env(live=False)
    cmd = [str(BIN_FD), "ask", q]
    if PACKS.is_dir():
        cmd.extend(["--root", str(PACKS)])
    rc, out = run(cmd, timeout=180 if live else 30, env=env)
    info = parse_fd(out)
    info["rc"] = rc
    info["q"] = q
    if sc:
        info["shortcircuit"] = True
        info["teacher"] = False
    # append miss_log if miss and has answer from teacher
    if info["source"] in ("LLM", "ASK_USER", "ABSTAIN") or info["miss"]:
        MISS.parent.mkdir(parents=True, exist_ok=True)
        row = {
            "ts": utc_now(),
            "query": q,
            "answer": info.get("answer") or "",
            "source": info["source"],
            "via": "autonomous_cycle",
        }
        if sc:
            row["shortcircuit"] = True
            row["teacher"] = False
            row["probe_pat"] = "curriculum_probe"
        with MISS.open("a", encoding="utf-8") as f:
            f.write(json.dumps(row, ensure_ascii=False) + "\n")
    return info


def seed_gold_for_stable_novel() -> None:
    """If we have a fixed curriculum grow item with gold, ensure gold file exists."""
    gold_dir = PACKS / "gold"
    gold_dir.mkdir(parents=True, exist_ok=True)
    # optional curriculum gold lines: {"q":..., "gold":...}
    if not CURRIC.is_file():
        return
    import hashlib

    for line in CURRIC.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            r = json.loads(line)
        except json.JSONDecodeError:
            continue
        g = r.get("gold")
        q = r.get("q") or r.get("query")
        if not g or not q:
            continue
        h = hashlib.sha1(re.sub(r"\s+", " ", q.strip().lower()).encode()).hexdigest()[:16]
        p = gold_dir / f"{h}.txt"
        if not p.exists():
            p.write_text(str(g).strip() + "\n", encoding="utf-8")


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    report: dict = {
        "ts": utc_now(),
        "engine": "cnet_autonomous_cycle",
        "hermes_required": False,
        "agi": False,
        "steps": [],
        "probes": [],
        "kpi": {},
        "ok": False,
    }

    # 0) autonomy charter — freedom + budgets
    charter_state = {}
    try:
        import cnet_autonomy_charter as ac  # type: ignore

        charter_state = ac.begin_tick()
        report["autonomy"] = {
            "charter": charter_state.get("charter"),
            "law": charter_state.get("law"),
            "counters": charter_state.get("counters"),
            "budgets": charter_state.get("budgets"),
        }
        # apply env ceilings for evolve/teacher children
        os.environ.update(
            {k: v for k, v in (charter_state.get("env_hints") or {}).items() if v}
        )
        ac.apply_env_from_charter(os.environ)
        report["steps"].append({"step": "autonomy_charter", "rc": 0})
    except Exception as e:
        report["steps"].append({"step": "autonomy_charter", "rc": 1, "err": str(e)[:120]})

    # 1) health
    rc, out = run([sys.executable, str(ROOT / "scripts" / "cnet_marble_health_snap.py")], 60)
    report["steps"].append({"step": "health", "rc": rc})
    health = {}
    hp = OUT_DIR / "status.json"
    if hp.is_file():
        try:
            health = json.loads(hp.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            health = {}
    report["health_core"] = health.get("core_active_count")

    # 2) front door binary
    if not ensure_front_door():
        report["error"] = "roe_front_door build failed"
        REPORT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print("CNET_AUTONOMOUS_FAIL front_door")
        return 1
    report["steps"].append({"step": "front_door_ready", "rc": 0})

    seed_gold_for_stable_novel()
    curriculum = load_curriculum()
    # charter probe budget
    try:
        import cnet_autonomy_charter as ac  # type: ignore

        cap_p = int((ac.charter().get("budgets") or {}).get("probes_per_tick", 40))
        if len(curriculum) > cap_p:
            curriculum = curriculum[:cap_p]
            report["steps"].append({"step": "charter_probe_cap", "kept": len(curriculum)})
    except Exception:
        pass

    # Neuromod schedule gate (ADO pause grow; 5HT control/impulse; DA handled in front_door)
    gate = {}
    gp = ROOT / "logs" / "governor" / "schedule_gate.json"
    if gp.is_file():
        try:
            gate = json.loads(gp.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            gate = {}
    pause_grow = bool(gate.get("pause_grow_probes"))
    pause_teacher = bool(gate.get("pause_teacher"))
    impulse = bool(gate.get("impulsivity_mode"))
    control = bool(gate.get("control_mode"))
    allow_fast_teacher = bool(gate.get("allow_teacher_faster"))
    report["schedule_gate"] = {
        "pause_grow_probes": pause_grow,
        "pause_teacher": pause_teacher,
        "control_mode": control,
        "impulsivity_mode": impulse,
    }

    if pause_grow:
        # Adenosine high: only stable LOCAL tags (soul/law/toolcall) — no grow/MISS probes
        curriculum = [
            c
            for c in curriculum
            if c.get("tag") in ("soul", "law", "toolcall", "ocr", "coding")
            and c.get("want") != "MISS"
        ]
        report["steps"].append({"step": "ado_pause_grow", "kept": len(curriculum)})

    live = os.environ.get("CNET_AUTO_TEACHER", "1") in ("1", "true", "yes")
    if pause_teacher and not impulse:
        live = False
    if impulse and allow_fast_teacher:
        live = True  # low 5HT impulsivity: allow teacher even when ADO mid
    # control mode: still allow teacher on miss unless pause_teacher
    if control and pause_teacher:
        live = False
    # charter: teacher allowed?
    try:
        import cnet_autonomy_charter as ac  # type: ignore

        tr = ac.check_action("teacher_on_miss")
        if not tr.get("ok"):
            live = False
            report["steps"].append({"step": "charter_teacher_deny", "reason": tr.get("reason")})
    except Exception:
        pass

    local_n = miss_n = llm_n = 0
    teacher_used = 0
    thoughts = []
    for item in curriculum:
        q = item["q"]
        # token-free thought BEFORE act
        try:
            import cnet_thought_process as tp  # type: ignore

            th = tp.think(q, persist=True)
            thoughts.append({"q": q[:60], "chain": th.get("chain"), "intent": th["steps"].get("INTEND")})
        except Exception:
            th = None
        info = probe(q, live=False)
        if info["source"] != "LOCAL" and live and not is_probe_query(q):
            # budget consume per teacher call
            allow_t = True
            try:
                import cnet_autonomy_charter as ac  # type: ignore

                tr = ac.check_action(
                    "teacher",
                    tick_teacher=teacher_used,
                    consume=True,
                )
                allow_t = bool(tr.get("ok"))
                if not allow_t:
                    report.setdefault("charter_denies", []).append(
                        {"action": "teacher", "reason": tr.get("reason"), "q": q[:40]}
                    )
            except Exception:
                pass
            if allow_t:
                info2 = probe(q, live=True)
                teacher_used += 1
                if info2.get("answer") or info2["source"] == "LLM":
                    info = info2
        elif info["source"] != "LOCAL" and is_probe_query(q):
            report.setdefault("probe_shortcircuits", []).append(q[:50])
        if th and info.get("source"):
            try:
                import cnet_thought_process as tp  # type: ignore

                tp.think(
                    q,
                    source=info.get("source"),
                    skill=info.get("skill"),
                    persist=True,
                )
            except Exception:
                pass
        if info["source"] == "LOCAL":
            local_n += 1
        elif info["source"] == "LLM":
            llm_n += 1
            miss_n += 1
        else:
            miss_n += 1
        report["probes"].append(
            {
                "q": q[:80],
                "tag": item.get("tag"),
                "source": info["source"],
                "skill": info.get("skill"),
                "want": item.get("want"),
                "thought_intent": (th or {}).get("steps", {}).get("INTEND") if th else None,
            }
        )
    report["thoughts_n"] = len(thoughts)
    report["thought_sample"] = thoughts[:3]
    report["kpi"]["teacher_calls_tick"] = teacher_used

    # Continuity workspace (bind body/mind/place/story — not consciousness)
    try:
        import cnet_continuity as cont  # type: ignore

        sample_q = thoughts[0]["q"] if thoughts else "status"
        cf = cont.snapshot(sample_q, run_thought=False, persist=True)
        report["continuity_line"] = cf.get("continuity_line")
        report["continuity"] = {
            "sleep": cf.get("sleep"),
            "intent": (cf.get("mind") or {}).get("intent"),
            "not_conscious": (cf.get("law") or {}).get("not_conscious"),
        }
    except Exception:
        pass

    n = max(1, len(curriculum))
    report["kpi"]["probe_n"] = n
    report["kpi"]["local_n"] = local_n
    report["kpi"]["miss_n"] = miss_n
    report["kpi"]["llm_n"] = llm_n
    report["kpi"]["local_hit"] = round(local_n / n, 3)

    # 3) evolve tick (reviewer on by default via env file + charter)
    env = fd_env(live=False)
    env["ROE_EVOLVE_REVIEWER"] = env.get("ROE_EVOLVE_REVIEWER", "1")
    try:
        import cnet_autonomy_charter as ac  # type: ignore

        env = ac.apply_env_from_charter(env)
        # deny evolve promote burst if day cap already hit
        pr = ac.check_action("promote")
        if not pr.get("ok"):
            report["steps"].append({"step": "evolve_skipped_charter", "reason": pr.get("reason")})
            env["ROE_EVOLVE_MAX_PROMOTES"] = "0"
    except Exception:
        pass
    if os.environ.get("CNET_AUTO_HERMETIC_REVIEW", "0") in ("1", "true", "yes"):
        env["ROE_REVIEW_HERMETIC"] = "1"
    rc, out = run(
        [sys.executable, str(ROOT / "tools" / "roe_evolve_tick.py")],
        timeout=600,
        env=env,
    )
    report["steps"].append({"step": "evolve_tick", "rc": rc})
    # parse evolve report + consume promote budget
    er = PACKS / "EVOLVE_TICK.json"
    if er.is_file():
        try:
            ev = json.loads(er.read_text(encoding="utf-8"))
            n_prom = len(ev.get("promoted") or [])
            report["kpi"]["promoted"] = n_prom
            report["kpi"]["evolve_skipped"] = len(ev.get("skipped") or [])
            report["kpi"]["reviewer_enabled"] = ev.get("reviewer_enabled")
            try:
                import cnet_autonomy_charter as ac  # type: ignore

                for _ in range(n_prom):
                    ac.check_action("promote", consume=True)
            except Exception:
                pass
        except json.JSONDecodeError:
            pass

    # 3b) neuromod personality tick (DA / 5HT / ADO)
    nm_path = ROOT / "scripts" / "cnet_neuromod.py"
    if nm_path.is_file():
        rc, out = run([sys.executable, str(nm_path), "--tick"], timeout=60)
        report["steps"].append({"step": "neuromod", "rc": rc})
        try:
            nm = json.loads((ROOT / "logs" / "governor" / "neuromod_state.json").read_text())
            report["neuromod"] = {
                "levels": nm.get("levels"),
                "actions": nm.get("actions"),
            }
            report["kpi"]["dopamine"] = (nm.get("levels") or {}).get("dopamine")
            report["kpi"]["serotonin"] = (nm.get("levels") or {}).get("serotonin")
            report["kpi"]["adenosine"] = (nm.get("levels") or {}).get("adenosine")
        except (OSError, json.JSONDecodeError):
            pass

    # 4) light autoteach optional (bounded, no hang forever)
    if os.environ.get("CNET_AUTO_AUTOTEACH", "0") in ("1", "true", "yes"):
        at = ROOT / "scripts" / "cnet_autoteach_tick.sh"
        if at.is_file():
            rc, out = run(["bash", str(at)], timeout=900)
            report["steps"].append({"step": "autoteach", "rc": rc})

    # 5) optional governor one-shot (already on timer; skip by default)
    if os.environ.get("CNET_AUTO_GOVERNOR", "0") in ("1", "true", "yes"):
        gov = ROOT / "scripts" / "governor_autonomous.py"
        if gov.is_file():
            rc, out = run([sys.executable, str(gov)], timeout=900)
            report["steps"].append({"step": "governor", "rc": rc})

    report["duration_s"] = round(time.time() - t0, 2)
    # success: cycle completed; LOCAL hit can be low on first cold start
    report["ok"] = True
    report["kpi"]["teacher_on_miss"] = live
    report["note"] = (
        "Autonomous = probe+evolve under policy; not AGI; Hermes optional client"
    )
    REPORT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    with (OUT_DIR / "autonomous_cycle.jsonl").open("a", encoding="utf-8") as f:
        f.write(
            json.dumps(
                {
                    "ts": report["ts"],
                    "local_hit": report["kpi"].get("local_hit"),
                    "promoted": report["kpi"].get("promoted", 0),
                    "miss_n": miss_n,
                    "duration_s": report["duration_s"],
                }
            )
            + "\n"
        )

    print(f"autonomous_cycle local_hit={report['kpi'].get('local_hit')} "
          f"promoted={report['kpi'].get('promoted', 0)} "
          f"miss={miss_n} dur={report['duration_s']}s")
    print(f"report → {REPORT}")
    print("CNET_AUTONOMOUS_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
