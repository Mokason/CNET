#!/usr/bin/env python3
"""Zen reflect-before-commit gate for CNET governor.

Sit still: re-check scoreboard + persona affect before heavy muscles fire.
Non-attachment: drop thrash goals when stuck.
Engaged: never delay keep_teacher_alive / clear infra fires.

Does not override pins or eval_veto — only softens/redirects agenda.
"""
from __future__ import annotations

import json
import os
import time
from pathlib import Path
from typing import Any

ROOT = Path(os.environ.get("CNET_ROOT", Path(__file__).resolve().parents[1]))
GOV = Path(os.environ.get("CNET_GOVERNOR_DIR", ROOT / "logs/governor"))
OUT = GOV / "zen_reflect.json"

HEAVY = {
    "run_cert_learn",
    "run_structure_mine",
    "inject_window_gaps",
    "break_plateau",
}
ALWAYS_COMMIT = {"ensure_bonsai", "ensure_lane", "close_outcome", "log_hold", "run_eval_probe"}


def reflect(
    picked: list[dict],
    sb: dict[str, Any],
    state: dict[str, Any],
    persona: dict[str, Any] | None = None,
) -> tuple[list[dict], dict[str, Any]]:
    """Return possibly revised goal list + reflect log."""
    persona = persona or {}
    affect = persona.get("affect") or sb.get("persona_affect") or {}
    traits = persona.get("traits") or sb.get("persona_traits") or {}
    vigilance = float(affect.get("vigilance") or sb.get("affect_vigilance") or 0.5)
    frustration = float(affect.get("frustration") or sb.get("affect_frustration") or 0.5)
    caution = float(traits.get("caution") or sb.get("persona_caution") or 0.5)
    patience = float(traits.get("patience") or 0.5)

    principles: list[str] = []
    actions_note: list[str] = []
    revised = list(picked)

    # 1) Infra fire — move without hesitation
    if int(sb.get("bonsai_ok") or 0) == 0 or int(sb.get("lane_ok") or 0) == 0:
        principles.append("engaged_action: teacher/lane down — commit keep_alive immediately")
        # ensure keep_teacher first if present in charter picks or inject
        ids = [g.get("id") for g in revised]
        if "keep_teacher_alive" not in ids:
            revised.insert(0, {"id": "keep_teacher_alive", "priority": 0, "actions": ["ensure_bonsai", "ensure_lane"], "when": "true"})
        log = _log(principles, actions_note, revised, "commit_infra")
        return revised, log

    # 2) Eval veto / freeze — non-attachment to seal plans
    if int(sb.get("eval_veto") or 0) or int(sb.get("freeze_seals") or 0):
        principles.append("non_attachment: eval_veto/freeze — drop seal-heavy goals")
        before = [g.get("id") for g in revised]
        revised = [
            g
            for g in revised
            if g.get("id")
            not in ("peft_jtc", "structure_mine", "break_plateau")
        ]
        if not revised:
            revised = [{"id": "outcome_review", "priority": 3, "actions": ["close_outcome", "run_eval_probe"], "when": "true"}]
        actions_note.append(f"dropped_seals:{before}->{[g.get('id') for g in revised]}")

    # 3) Attachment / thrash detection
    streak = int(state.get("same_goals_streak") or 0)
    last = state.get("last_goals") or []
    now_ids = [g.get("id") for g in revised]
    if last == now_ids:
        streak += 1
    else:
        streak = 1
    state["same_goals_streak"] = streak
    d_backlog = float(sb.get("d_backlog_pressure") or 0)
    if streak >= 4 and d_backlog >= 0 and "outcome_review" not in now_ids:
        principles.append("beginners_mind: attached to same goals without backlog improvement — force review")
        revised = [
            {"id": "outcome_review", "priority": 2, "actions": ["close_outcome", "run_eval_probe"], "when": "true"},
            {"id": "health_hold", "priority": 9, "actions": ["log_hold"], "when": "true"},
        ]
        actions_note.append("attachment_break")

    # 4) High vigilance / caution — sit still on heavy thrash
    vig_hold = 0.72
    if (vigilance >= vig_hold or caution >= 0.78) and int(sb.get("busy") or 0):
        principles.append("zazen: high vigilance+busy — demote heavy explore/break")
        revised = [
            g
            for g in revised
            if g.get("id") not in ("break_plateau", "coverage_curiosity", "verified_web")
        ]
        if not revised:
            revised = [{"id": "health_hold", "priority": 9, "actions": ["log_hold"], "when": "true"}]
        actions_note.append("vigilance_hold")

    # 5) Frustration without patience → thoroughness: prefer review over inject spam
    if frustration >= 0.65 and patience >= 0.55 and any(g.get("id") == "drain_open_gaps" for g in revised):
        if streak >= 3:
            principles.append("simplicity: inject thrash — insert outcome_review before more gaps")
            if not any(g.get("id") == "outcome_review" for g in revised):
                revised.insert(0, {"id": "outcome_review", "priority": 2, "actions": ["close_outcome", "run_eval_probe"], "when": "true"})
                revised = revised[:3]
            actions_note.append("frustration_review")

    # 6) Present moment: strip empty
    revised = [g for g in revised if g.get("id")]
    if not revised:
        principles.append("present_moment: nothing left — health_hold")
        revised = [{"id": "health_hold", "priority": 9, "actions": ["log_hold"], "when": "true"}]

    if not principles:
        principles.append("clear_see: agenda accepted — move without hesitation")

    log = _log(principles, actions_note, revised, "ok")
    return revised, log


def filter_actions(actions: list[str], sb: dict[str, Any], zen_log: dict[str, Any]) -> list[str]:
    """Optional second gate on action names."""
    if zen_log.get("mode") == "commit_infra":
        return actions
    if int(sb.get("eval_veto") or 0) or int(sb.get("freeze_seals") or 0):
        return [a for a in actions if a not in ("run_cert_learn", "run_structure_mine") or a in ALWAYS_COMMIT]
    return actions


def _log(principles: list[str], notes: list[str], goals: list[dict], mode: str) -> dict[str, Any]:
    log = {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "mode": mode,
        "principles": principles,
        "notes": notes,
        "goals_after": [g.get("id") for g in goals],
        "engine": "zen_reflect_v1",
    }
    GOV.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(log, indent=2) + "\n")
    return log


def self_test() -> int:
    sb = {"bonsai_ok": 1, "lane_ok": 1, "eval_veto": 0, "freeze_seals": 0, "busy": 0, "d_backlog_pressure": 0}
    st: dict[str, Any] = {"last_goals": ["drain_open_gaps"], "same_goals_streak": 3}
    picked = [{"id": "drain_open_gaps", "actions": ["inject_window_gaps"]}]
    # attachment
    st["same_goals_streak"] = 3
    st["last_goals"] = ["drain_open_gaps"]
    r, log = reflect(picked, {**sb, "d_backlog_pressure": 1}, st, {"affect": {"vigilance": 0.4, "frustration": 0.7}, "traits": {"caution": 0.6, "patience": 0.7}})
    # after reflect streak becomes 4
    assert "outcome_review" in [g["id"] for g in r] or log.get("notes")
    # infra fire
    r2, log2 = reflect([], {"bonsai_ok": 0, "lane_ok": 1}, {}, {})
    assert r2[0]["id"] == "keep_teacher_alive"
    assert log2["mode"] == "commit_infra"
    # veto drops seals
    r3, _ = reflect(
        [{"id": "peft_jtc", "actions": ["run_cert_learn"]}],
        {**sb, "eval_veto": 1},
        {},
        {},
    )
    assert all(g["id"] != "peft_jtc" for g in r3)
    print("ZEN_REFLECT_SELFTEST_PASS checks=3")
    return 0


if __name__ == "__main__":
    import sys

    if "--test" in sys.argv:
        raise SystemExit(self_test())
    print("usage: import reflect() from governor")
