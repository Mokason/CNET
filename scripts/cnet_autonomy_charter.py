#!/usr/bin/env python3
"""Autonomy charter — freedom tiers + hard budgets for Autonomous-ASI.

  python3 scripts/cnet_autonomy_charter.py --show
  python3 scripts/cnet_autonomy_charter.py --check promote
  python3 scripts/cnet_autonomy_charter.py --consume teacher
  python3 scripts/cnet_autonomy_charter.py --test
  make cnet_autonomy_charter

Law from config/autonomy_charter.yaml always wins over schedule freedom.
"""
from __future__ import annotations

import argparse
import json
import os
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(os.environ.get("CNET_ROOT", Path(__file__).resolve().parents[1]))
CFG_PATH = Path(
    os.environ.get("CNET_AUTONOMY_CHARTER", ROOT / "config" / "autonomy_charter.yaml")
)
GOV = Path(os.environ.get("CNET_GOVERNOR_DIR", ROOT / "logs/governor"))


def _utc() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _day_key() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%d")


def _hour_key() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H")


def load_yaml(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {}
    try:
        import yaml  # type: ignore

        return yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    except Exception:
        pass
    # minimal fallback for our flat-ish file
    data: dict[str, Any] = {"law": {}, "budgets": {}, "packs": {}, "paths": {}}
    section = None
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#")[0].rstrip()
        if not line.strip():
            continue
        if re_sec := __import__("re").match(r"^([a-z_]+):\s*$", line):
            section = re_sec.group(1)
            if section not in data:
                data[section] = {}
            continue
        if section == "law" or section == "budgets" or section == "packs":
            m = __import__("re").match(r"^\s+([a-z0-9_]+):\s*(.+)$", line)
            if not m:
                continue
            k, v = m.group(1), m.group(2).strip()
            if v in ("true", "false"):
                data[section][k] = v == "true"
            else:
                try:
                    data[section][k] = int(v)
                except ValueError:
                    try:
                        data[section][k] = float(v)
                    except ValueError:
                        data[section][k] = v.strip("\"'")
    return data


def default_charter() -> dict[str, Any]:
    return {
        "version": 1,
        "name": "fallback",
        "law": {
            "never_self_cert": True,
            "not_agi": True,
            "not_conscious": True,
            "reviewer_required_non_gold": True,
            "soul_auto_edit": False,
            "floor_auto_change": False,
        },
        "budgets": {
            "promotes_per_day": 30,
            "promotes_per_tick": 10,
            "teacher_calls_per_hour": 24,
            "teacher_calls_per_tick": 8,
            "probes_per_tick": 40,
            "evolve_max_promotes_env": 20,
        },
        "packs": {
            "auto_promote_target": "pack_personal",
            "allow_new_domain_pack": False,
            "allow_soul_mutate": False,
        },
        "paths": {
            "state": "logs/governor/autonomy_state.json",
            "log": "logs/governor/autonomy_charter.jsonl",
            "counters": "logs/governor/autonomy_counters.json",
        },
        "never_free": [
            "self_cert_from_llm",
            "edit_pack_soul",
            "disable_reviewer",
            "lower_floors",
            "claim_consciousness",
        ],
    }


def charter() -> dict[str, Any]:
    c = load_yaml(CFG_PATH)
    if not c:
        c = default_charter()
    # merge defaults for missing keys
    d = default_charter()
    for k in ("law", "budgets", "packs", "paths"):
        base = dict(d.get(k) or {})
        base.update(c.get(k) or {})
        c[k] = base
    c.setdefault("never_free", d["never_free"])
    c.setdefault("always_free", [])
    c.setdefault("policy_free", [])
    return c


def _paths(c: dict[str, Any]) -> tuple[Path, Path, Path]:
    p = c.get("paths") or {}
    state = ROOT / p.get("state", "logs/governor/autonomy_state.json")
    log = ROOT / p.get("log", "logs/governor/autonomy_charter.jsonl")
    counters = ROOT / p.get("counters", "logs/governor/autonomy_counters.json")
    return state, log, counters


def load_counters(c: dict[str, Any]) -> dict[str, Any]:
    _, _, cp = _paths(c)
    if cp.is_file():
        try:
            return json.loads(cp.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            pass
    return {
        "day": _day_key(),
        "hour": _hour_key(),
        "promotes_day": 0,
        "teacher_hour": 0,
        "ticks": 0,
        "last_tick_ts": None,
        "last_deny": [],
    }


def save_counters(c: dict[str, Any], ctr: dict[str, Any]) -> None:
    _, _, cp = _paths(c)
    cp.parent.mkdir(parents=True, exist_ok=True)
    # roll windows
    if ctr.get("day") != _day_key():
        ctr["day"] = _day_key()
        ctr["promotes_day"] = 0
    if ctr.get("hour") != _hour_key():
        ctr["hour"] = _hour_key()
        ctr["teacher_hour"] = 0
    cp.write_text(json.dumps(ctr, indent=2) + "\n", encoding="utf-8")


def log_event(c: dict[str, Any], event: dict[str, Any]) -> None:
    _, lp, _ = _paths(c)
    lp.parent.mkdir(parents=True, exist_ok=True)
    event = {"ts": _utc(), **event}
    with lp.open("a", encoding="utf-8") as f:
        f.write(json.dumps(event) + "\n")


def is_never_free(c: dict[str, Any], action: str) -> bool:
    return action in set(c.get("never_free") or [])


def check_action(
    action: str,
    *,
    tick_promotes: int = 0,
    tick_teacher: int = 0,
    tick_probes: int = 0,
    consume: bool = False,
) -> dict[str, Any]:
    """Return {ok, reason, budgets, law}."""
    c = charter()
    law = c.get("law") or {}
    bud = c.get("budgets") or {}
    ctr = load_counters(c)

    # roll
    if ctr.get("day") != _day_key():
        ctr["day"] = _day_key()
        ctr["promotes_day"] = 0
    if ctr.get("hour") != _hour_key():
        ctr["hour"] = _hour_key()
        ctr["teacher_hour"] = 0

    result: dict[str, Any] = {
        "ok": True,
        "action": action,
        "reason": "allow",
        "law": {
            "never_self_cert": bool(law.get("never_self_cert", True)),
            "not_agi": bool(law.get("not_agi", True)),
            "reviewer_required_non_gold": bool(law.get("reviewer_required_non_gold", True)),
            "soul_auto_edit": bool(law.get("soul_auto_edit", False)),
        },
        "packs": c.get("packs") or {},
        "budgets": {
            "promotes_day": ctr.get("promotes_day", 0),
            "promotes_per_day": int(bud.get("promotes_per_day", 30)),
            "teacher_hour": ctr.get("teacher_hour", 0),
            "teacher_calls_per_hour": int(bud.get("teacher_calls_per_hour", 24)),
            "promotes_per_tick": int(bud.get("promotes_per_tick", 10)),
            "teacher_calls_per_tick": int(bud.get("teacher_calls_per_tick", 8)),
            "probes_per_tick": int(bud.get("probes_per_tick", 40)),
            "evolve_max_promotes_env": int(bud.get("evolve_max_promotes_env", 20)),
        },
    }

    if is_never_free(c, action):
        result["ok"] = False
        result["reason"] = f"never_free:{action}"
        log_event(c, {"event": "deny", **result})
        return result

    # law hard blocks
    if action in ("self_cert_from_llm", "promote_without_reviewer_or_gold"):
        if law.get("never_self_cert", True):
            result["ok"] = False
            result["reason"] = "law:never_self_cert"
            log_event(c, {"event": "deny", **result})
            return result
    if action == "edit_pack_soul" and not law.get("soul_auto_edit", False):
        result["ok"] = False
        result["reason"] = "law:soul_auto_edit_false"
        log_event(c, {"event": "deny", **result})
        return result
    if action == "lower_floors" and not law.get("floor_auto_change", False):
        result["ok"] = False
        result["reason"] = "law:floor_auto_change_false"
        log_event(c, {"event": "deny", **result})
        return result

    # budget checks
    if action in ("promote", "promote_gold", "promote_multi_stable_plus_reviewer"):
        cap_day = int(bud.get("promotes_per_day", 30))
        cap_tick = int(bud.get("promotes_per_tick", 10))
        if ctr.get("promotes_day", 0) >= cap_day:
            result["ok"] = False
            result["reason"] = f"budget:promotes_per_day>={cap_day}"
        elif tick_promotes >= cap_tick:
            result["ok"] = False
            result["reason"] = f"budget:promotes_per_tick>={cap_tick}"
        elif consume and result["ok"]:
            ctr["promotes_day"] = int(ctr.get("promotes_day", 0)) + 1
            save_counters(c, ctr)
            result["budgets"]["promotes_day"] = ctr["promotes_day"]
            log_event(c, {"event": "consume_promote", "promotes_day": ctr["promotes_day"]})

    if action in ("teacher", "teacher_on_miss"):
        cap_h = int(bud.get("teacher_calls_per_hour", 24))
        cap_t = int(bud.get("teacher_calls_per_tick", 8))
        if ctr.get("teacher_hour", 0) >= cap_h:
            result["ok"] = False
            result["reason"] = f"budget:teacher_calls_per_hour>={cap_h}"
        elif tick_teacher >= cap_t:
            result["ok"] = False
            result["reason"] = f"budget:teacher_calls_per_tick>={cap_t}"
        elif consume and result["ok"]:
            ctr["teacher_hour"] = int(ctr.get("teacher_hour", 0)) + 1
            save_counters(c, ctr)
            result["budgets"]["teacher_hour"] = ctr["teacher_hour"]
            log_event(c, {"event": "consume_teacher", "teacher_hour": ctr["teacher_hour"]})

    if action == "probe":
        cap = int(bud.get("probes_per_tick", 40))
        if tick_probes >= cap:
            result["ok"] = False
            result["reason"] = f"budget:probes_per_tick>={cap}"

    if action == "new_domain_pack":
        if not (c.get("packs") or {}).get("allow_new_domain_pack", False):
            result["ok"] = False
            result["reason"] = "packs:allow_new_domain_pack=false"

    if not result["ok"]:
        den = list(ctr.get("last_deny") or [])[-20:]
        den.append({"ts": _utc(), "action": action, "reason": result["reason"]})
        ctr["last_deny"] = den
        save_counters(c, ctr)
        log_event(c, {"event": "deny", **result})

    return result


def begin_tick() -> dict[str, Any]:
    c = charter()
    ctr = load_counters(c)
    ctr["ticks"] = int(ctr.get("ticks") or 0) + 1
    ctr["last_tick_ts"] = _utc()
    save_counters(c, ctr)
    sp, _, _ = _paths(c)
    state = {
        "ts": _utc(),
        "charter": c.get("name"),
        "version": c.get("version"),
        "law": c.get("law"),
        "budgets": c.get("budgets"),
        "packs": c.get("packs"),
        "counters": {
            "promotes_day": ctr.get("promotes_day"),
            "teacher_hour": ctr.get("teacher_hour"),
            "ticks": ctr.get("ticks"),
        },
        "env_hints": {
            "ROE_EVOLVE_MAX_PROMOTES": str(
                min(
                    int((c.get("budgets") or {}).get("evolve_max_promotes_env", 20)),
                    int((c.get("budgets") or {}).get("promotes_per_tick", 10)),
                )
            ),
            "ROE_EVOLVE_REVIEWER": "1"
            if (c.get("law") or {}).get("reviewer_required_non_gold", True)
            else "0",
        },
    }
    sp.parent.mkdir(parents=True, exist_ok=True)
    sp.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    log_event(c, {"event": "tick_begin", "ticks": ctr["ticks"]})
    return state


def apply_env_from_charter(env: Any = None) -> dict:
    """Mutate env mapping with charter ceilings for child processes."""
    c = charter()
    e = dict(os.environ) if env is None else env
    bud = c.get("budgets") or {}
    law = c.get("law") or {}
    e["ROE_EVOLVE_MAX_PROMOTES"] = str(
        min(
            int(bud.get("evolve_max_promotes_env", 20)),
            int(bud.get("promotes_per_tick", 10)),
        )
    )
    if law.get("reviewer_required_non_gold", True):
        e["ROE_EVOLVE_REVIEWER"] = "1"
    if law.get("fail_closed_on_reviewer_error", True):
        e["ROE_REVIEW_FAIL_CLOSED"] = "1"
    e["CNET_AUTONOMY_PROMOTE_TARGET"] = str(
        (c.get("packs") or {}).get("auto_promote_target") or "pack_personal"
    )
    e["CNET_AUTONOMY_CHARTER"] = str(CFG_PATH)
    return e  # type: ignore[return-value]


def selftest() -> int:
    failures = 0

    def check(ok: bool, m: str) -> None:
        nonlocal failures
        print(f"  {m:56} {'PASS' if ok else 'FAIL'}")
        if not ok:
            failures += 1

    print("=== autonomy charter ===")
    c = charter()
    check(bool(c.get("law", {}).get("never_self_cert")), "never_self_cert")
    check(bool(c.get("law", {}).get("not_agi")), "not_agi")
    check(not c.get("law", {}).get("soul_auto_edit", True), "soul_auto_edit false")
    r = check_action("edit_pack_soul")
    check(r["ok"] is False, "deny soul edit")
    r2 = check_action("self_cert_from_llm")
    check(r2["ok"] is False, "deny self_cert")
    r3 = check_action("new_domain_pack")
    check(r3["ok"] is False, "deny new domain pack by default")
    st = begin_tick()
    check("env_hints" in st, "tick state written")
    e = apply_env_from_charter({})
    check(e.get("ROE_EVOLVE_REVIEWER") == "1", "env forces reviewer")
    check(int(e.get("ROE_EVOLVE_MAX_PROMOTES", "0")) <= 20, "env promote ceiling")
    # budget exhaust simulate
    c2 = charter()
    ctr = load_counters(c2)
    ctr["promotes_day"] = int((c2.get("budgets") or {}).get("promotes_per_day", 30))
    save_counters(c2, ctr)
    r4 = check_action("promote")
    check(r4["ok"] is False and "promotes_per_day" in r4["reason"], "day promote cap")
    # reset counter for normal ops
    ctr["promotes_day"] = 0
    save_counters(c2, ctr)
    r5 = check_action("promote", consume=True)
    check(r5["ok"] is True, "promote allow under cap")
    check(
        load_counters(c2).get("promotes_day", 0) >= 1,
        "consume increments counter",
    )

    print(f"\nfailures={failures}")
    if failures:
        print("AUTONOMY_CHARTER_FAIL")
        return 1
    print("AUTONOMY_CHARTER_PASS")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--show", action="store_true")
    ap.add_argument("--check", metavar="ACTION", default="")
    ap.add_argument("--consume", metavar="ACTION", default="")
    ap.add_argument("--begin-tick", action="store_true")
    ap.add_argument("--test", action="store_true")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    if args.test:
        return selftest()
    c = charter()
    if args.begin_tick:
        st = begin_tick()
        print(json.dumps(st, indent=2) if args.json else f"tick ok counters={st.get('counters')}")
        print("AUTONOMY_TICK_OK")
        return 0
    if args.check:
        r = check_action(args.check)
        print(json.dumps(r, indent=2) if args.json else f"ok={r['ok']} reason={r['reason']}")
        return 0 if r["ok"] else 1
    if args.consume:
        r = check_action(args.consume, consume=True)
        print(json.dumps(r, indent=2) if args.json else f"ok={r['ok']} reason={r['reason']}")
        return 0 if r["ok"] else 1
    # show
    if args.json:
        print(json.dumps(c, indent=2))
    else:
        print(f"charter: {c.get('name')} v{c.get('version')} @ {CFG_PATH}")
        print("law:", json.dumps(c.get("law"), indent=2))
        print("budgets:", json.dumps(c.get("budgets"), indent=2))
        print("packs:", json.dumps(c.get("packs"), indent=2))
        print("never_free:", c.get("never_free"))
        ctr = load_counters(c)
        print("counters:", {k: ctr.get(k) for k in ("day", "hour", "promotes_day", "teacher_hour", "ticks")})
    print("AUTONOMY_CHARTER_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
