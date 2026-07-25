#!/usr/bin/env python3
"""Homeostatic personality organ for CNET governor.

Affect channels (dopamine-like) update from real scoreboard outcomes.
Traits EWMA toward affect-driven targets, then homeostasis pulls to baseline.
All values hard-clamped — no extremes.

Does NOT touch certify/PEFT seal logic. Only provides persona bias + state.
"""
from __future__ import annotations

import json
import os
import re
import time
from pathlib import Path
from typing import Any

ROOT = Path(os.environ.get("CNET_ROOT", Path(__file__).resolve().parents[1]))
GOV = Path(os.environ.get("CNET_GOVERNOR_DIR", ROOT / "logs/governor"))
CFG = Path(os.environ.get("CNET_PERSONALITY_CONFIG", ROOT / "config/personality.yaml"))
STATE_PATH = GOV / "personality_state.json"
HIST_PATH = GOV / "personality_history.jsonl"

AFFECT_KEYS = (
    "reward",       # DA-like: progress / eval up
    "frustration",  # blocked progress
    "calm",         # satiety / healthy hold
    "vigilance",    # threat / busy / teacher down
    "integrity",    # charter/pins honored
    "correction",   # veto / user pin / failed outcome
)

TRAIT_KEYS = (
    "curiosity",
    "caution",
    "thoroughness",
    "loyalty_to_charter",
    "boldness",
    "sociability",
    "patience",
)


def _clamp(x: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, float(x)))


def load_yaml_lite(path: Path) -> dict[str, Any]:
    """Minimal YAML subset for our personality file (no full PyYAML required)."""
    if not path.exists():
        return {}
    text = path.read_text()
    # Prefer PyYAML if present
    try:
        import yaml  # type: ignore

        return yaml.safe_load(text) or {}
    except Exception:
        pass
    # Fallback: json-incompatible; use crude parse via python if pyyaml missing
    # Ship a JSON sidecar generator — for robustness write defaults
    return {"active_profile": "memory_witness", "clamp_min": 0.15, "clamp_max": 0.85,
            "max_trait_delta": 0.04, "homeostasis": 0.08, "bias_strength": 1.0, "frozen": 0,
            "profiles": {}}


def parse_personality_config(path: Path) -> dict[str, Any]:
    """Parse personality.yaml with a small hand parser (profiles nested)."""
    if not path.exists():
        return {}
    try:
        import yaml  # type: ignore
        data = yaml.safe_load(path.read_text()) or {}
        if data.get("profiles"):
            return data
    except Exception:
        pass

    # Hand parser sufficient for our schema
    cfg: dict[str, Any] = {
        "profiles": {},
        "clamp_min": 0.15,
        "clamp_max": 0.85,
        "max_trait_delta": 0.04,
        "homeostasis": 0.08,
        "bias_strength": 1.0,
        "frozen": 0,
        "active_profile": "memory_witness",
    }
    lines = path.read_text().splitlines()
    i = 0
    cur_profile = None
    section = None  # baseline | prefer | avoid | voice
    while i < len(lines):
        raw = lines[i]
        line = raw.split("#")[0].rstrip()
        if not line.strip():
            i += 1
            continue
        if re.match(r"^(active_profile|clamp_min|clamp_max|max_trait_delta|homeostasis|bias_strength|frozen):", line):
            k, v = line.split(":", 1)
            v = v.strip().strip('"')
            if k in ("clamp_min", "clamp_max", "max_trait_delta", "homeostasis", "bias_strength"):
                cfg[k] = float(v)
            elif k == "frozen":
                cfg[k] = int(v) if v.isdigit() else 0
            else:
                cfg[k] = v
            i += 1
            continue
        m = re.match(r"^  ([a-z_]+):\s*$", line)
        if line.strip() == "profiles:":
            i += 1
            continue
        if m and not line.startswith("    "):
            # profile name at 2-space indent under profiles
            cur_profile = m.group(1)
            cfg["profiles"][cur_profile] = {
                "title": cur_profile,
                "baseline": {},
                "prefer_goals": [],
                "avoid_goals": [],
                "voice": {},
            }
            section = None
            i += 1
            continue
        if cur_profile is None:
            i += 1
            continue
        prof = cfg["profiles"][cur_profile]
        if re.match(r"^    title:", line):
            prof["title"] = line.split(":", 1)[1].strip().strip('"')
        elif re.match(r"^    blurb:", line):
            prof["blurb"] = line.split(":", 1)[1].strip().strip('"')
        elif line.strip() == "baseline:":
            section = "baseline"
        elif line.strip() == "prefer_goals:":
            section = "prefer"
        elif line.strip() == "avoid_goals:":
            section = "avoid"
        elif line.strip() == "voice:":
            section = "voice"
        elif section == "baseline" and re.match(r"^      [a-z_]+:", line):
            k, v = line.strip().split(":", 1)
            prof["baseline"][k] = float(v.strip())
        elif section == "prefer" and line.strip().startswith("- "):
            prof["prefer_goals"].append(line.strip()[2:].strip())
        elif section == "avoid" and line.strip().startswith("- "):
            prof["avoid_goals"].append(line.strip()[2:].strip())
        elif section == "voice" and re.match(r"^      tone:", line):
            prof["voice"]["tone"] = line.split(":", 1)[1].strip()
        i += 1
    return cfg


def default_state(cfg: dict) -> dict[str, Any]:
    name = cfg.get("active_profile") or "memory_witness"
    prof = (cfg.get("profiles") or {}).get(name) or {}
    base = dict(prof.get("baseline") or {k: 0.5 for k in TRAIT_KEYS})
    lo = float(cfg.get("clamp_min") or 0.15)
    hi = float(cfg.get("clamp_max") or 0.85)
    traits = {k: _clamp(base.get(k, 0.5), lo, hi) for k in TRAIT_KEYS}
    affect = {k: 0.5 for k in AFFECT_KEYS}
    return {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "profile": name,
        "title": prof.get("title") or name,
        "baseline": {k: _clamp(base.get(k, 0.5), lo, hi) for k in TRAIT_KEYS},
        "traits": traits,
        "affect": affect,
        "consistency": 1.0,
        "trait_drift": 0.0,
        "charter_alignment": float(traits.get("loyalty_to_charter", 0.7)),
        "cycles": 0,
        "last_bias": {},
        "engine": "personality_homeostatic_v1",
    }


def load_state(cfg: dict) -> dict[str, Any]:
    st = default_state(cfg)
    if STATE_PATH.exists():
        try:
            prev = json.loads(STATE_PATH.read_text())
            st.update({k: prev[k] for k in prev if k in st or k in ("traits", "affect", "baseline", "profile", "cycles", "consistency", "trait_drift")})
            # ensure keys
            for k in TRAIT_KEYS:
                st["traits"][k] = float(st.get("traits", {}).get(k, st["baseline"].get(k, 0.5)))
                st["baseline"][k] = float(st.get("baseline", {}).get(k, 0.5))
            for k in AFFECT_KEYS:
                st["affect"][k] = float(st.get("affect", {}).get(k, 0.5))
        except Exception:
            pass
    # profile switch resets baseline from config
    name = cfg.get("active_profile") or st.get("profile")
    if name != st.get("profile"):
        st = default_state(cfg)
    return st


def affect_from_scoreboard(sb: dict[str, Any], prev_affect: dict[str, Any], lo: float, hi: float) -> dict[str, float]:
    """Map competence scoreboard → affect channels (prediction-error style)."""
    a = {k: float(prev_affect.get(k, 0.5)) for k in AFFECT_KEYS}

    # signals
    d_backlog = float(sb.get("d_backlog_pressure") or 0)
    d_eval = float(sb.get("d_eval_jtc") or 0)
    eval_jtc = float(sb.get("eval_jtc_delta") or 0)
    hermes_fail = float(sb.get("hermes_task_fail_rate") or sb.get("hermes_err_rate") or 0)
    plateau = int(sb.get("plateau") or 0)
    busy = int(sb.get("busy") or 0)
    teacher = float(sb.get("teacher_uptime") or (1 if sb.get("bonsai_ok") else 0))
    veto = int(sb.get("eval_veto") or 0)
    velocity = float(sb.get("learning_velocity") or 0)

    # reward: progress
    reward_pulse = 0.5
    if d_backlog < -0.5 or d_eval > 0.01 or velocity > 1.0:
        reward_pulse = 0.72
    if d_backlog > 1 or d_eval < -0.02:
        reward_pulse = 0.28
    a["reward"] = 0.7 * a["reward"] + 0.3 * reward_pulse

    # frustration
    fr = 0.35 + 0.25 * plateau + 0.2 * (1 if d_backlog > 0.5 else 0) + 0.15 * min(1.0, hermes_fail)
    a["frustration"] = 0.7 * a["frustration"] + 0.3 * fr

    # calm: healthy idle-ish
    calm = 0.55
    if int(sb.get("backlog_pressure") or 0) < 8 and hermes_fail < 0.15 and not plateau:
        calm = 0.75
    if plateau or hermes_fail > 0.4:
        calm = 0.30
    a["calm"] = 0.7 * a["calm"] + 0.3 * calm

    # vigilance
    vig = 0.3 + 0.35 * busy + 0.35 * (1.0 - teacher) + 0.2 * veto
    a["vigilance"] = 0.7 * a["vigilance"] + 0.3 * min(0.85, vig)

    # integrity: loyalty signal
    integ = 0.6 + 0.2 * (1 if not veto else -0.3) + 0.1 * float(sb.get("goal_health_drain") or 0.5)
    a["integrity"] = 0.75 * a["integrity"] + 0.25 * integ

    # correction
    corr = 0.3 + 0.4 * veto + 0.2 * (1 if d_eval < -0.02 else 0)
    a["correction"] = 0.7 * a["correction"] + 0.3 * corr

    return {k: _clamp(a[k], lo, hi) for k in AFFECT_KEYS}


def traits_from_affect(
    baseline: dict[str, float],
    traits: dict[str, float],
    affect: dict[str, float],
    cfg: dict[str, Any],
) -> dict[str, float]:
    """Affect nudges traits; homeostasis pulls to baseline; clamp deltas."""
    lo = float(cfg.get("clamp_min") or 0.15)
    hi = float(cfg.get("clamp_max") or 0.85)
    max_d = float(cfg.get("max_trait_delta") or 0.04)
    home = float(cfg.get("homeostasis") or 0.08)
    frozen = int(cfg.get("frozen") or 0)

    if frozen:
        return {k: _clamp(float(baseline.get(k, 0.5)), lo, hi) for k in TRAIT_KEYS}

    # desired direction from affect
    desire = {
        "curiosity": 0.5 + 0.25 * (affect["reward"] - 0.5) - 0.2 * (affect["vigilance"] - 0.5) - 0.15 * (affect["frustration"] - 0.5),
        "caution": 0.5 + 0.35 * (affect["vigilance"] - 0.5) + 0.2 * (affect["correction"] - 0.5) - 0.1 * (affect["reward"] - 0.5),
        "thoroughness": 0.5 + 0.25 * (affect["frustration"] - 0.5) + 0.2 * (affect["correction"] - 0.5) + 0.1 * (affect["integrity"] - 0.5),
        "loyalty_to_charter": 0.5 + 0.4 * (affect["integrity"] - 0.5) + 0.15 * (affect["correction"] - 0.5),
        "boldness": 0.5 + 0.25 * (affect["reward"] - 0.5) - 0.35 * (affect["vigilance"] - 0.5) - 0.15 * (affect["frustration"] - 0.5),
        "sociability": 0.5 + 0.15 * (affect["calm"] - 0.5) - 0.1 * (affect["vigilance"] - 0.5),
        "patience": 0.5 + 0.3 * (affect["calm"] - 0.5) - 0.2 * (affect["frustration"] - 0.5),
    }

    out = {}
    for k in TRAIT_KEYS:
        base = float(baseline.get(k, 0.5))
        cur = float(traits.get(k, base))
        target = _clamp(desire.get(k, 0.5), lo, hi)
        # move toward affect target
        stepped = cur + _clamp(target - cur, -max_d, max_d) * 0.5
        # homeostasis toward baseline
        stepped = stepped + home * (base - stepped)
        out[k] = _clamp(stepped, lo, hi)
    return out


def compute_bias(profile: dict[str, Any], traits: dict[str, float], cfg: dict[str, Any]) -> dict[str, float]:
    """Goal-id → rank bias (negative = prefer). Magnitude scaled by traits."""
    strength = float(cfg.get("bias_strength") or 1.0)
    prefer = profile.get("prefer_goals") or []
    avoid = profile.get("avoid_goals") or []
    bias: dict[str, float] = {}
    # higher loyalty → stronger prefer on outcome_review / charter goals
    loy = float(traits.get("loyalty_to_charter", 0.5))
    cur = float(traits.get("curiosity", 0.5))
    cau = float(traits.get("caution", 0.5))
    bold = float(traits.get("boldness", 0.5))
    thr = float(traits.get("thoroughness", 0.5))

    for g in prefer:
        bias[g] = bias.get(g, 0) - strength * (0.6 + 0.4 * loy)
    for g in avoid:
        # avoid less when calm/curious high — still soft
        bias[g] = bias.get(g, 0) + strength * (0.5 + 0.3 * cau - 0.2 * cur)

    # trait-specific soft leans
    bias["verified_web"] = bias.get("verified_web", 0) - 0.4 * strength * (cur - 0.5)
    bias["peft_jtc"] = bias.get("peft_jtc", 0) - 0.5 * strength * (thr - 0.45)
    bias["break_plateau"] = bias.get("break_plateau", 0) - 0.5 * strength * (bold - 0.5) + 0.5 * strength * (cau - 0.5)
    bias["health_hold"] = bias.get("health_hold", 0) - 0.3 * strength * (float(traits.get("patience", 0.5)) - 0.5)
    bias["outcome_review"] = bias.get("outcome_review", 0) - 0.6 * strength * loy

    # clamp bias magnitude
    for k in list(bias.keys()):
        bias[k] = _clamp(bias[k], -2.5, 2.5)
    return bias


def consistency_score(traits: dict[str, float], baseline: dict[str, float]) -> tuple[float, float]:
    drifts = [abs(float(traits[k]) - float(baseline.get(k, 0.5))) for k in TRAIT_KEYS]
    drift = sum(drifts) / max(1, len(drifts))
    # consistency high when drift low
    cons = _clamp(1.0 - drift * 2.5, 0.15, 1.0)
    return cons, drift


def tick(scoreboard: dict[str, Any] | None = None) -> dict[str, Any]:
    """One personality cycle. Returns state dict for governor."""
    cfg = parse_personality_config(CFG)
    if not cfg.get("profiles"):
        # last resort defaults
        cfg = {
            "active_profile": "memory_witness",
            "clamp_min": 0.15,
            "clamp_max": 0.85,
            "max_trait_delta": 0.04,
            "homeostasis": 0.08,
            "bias_strength": 1.0,
            "frozen": 0,
            "profiles": {
                "memory_witness": {
                    "title": "Memory-Witness",
                    "baseline": {k: 0.5 for k in TRAIT_KEYS},
                    "prefer_goals": ["outcome_review", "peft_jtc"],
                    "avoid_goals": [],
                }
            },
        }

    lo = float(cfg.get("clamp_min") or 0.15)
    hi = float(cfg.get("clamp_max") or 0.85)
    st = load_state(cfg)
    name = cfg.get("active_profile") or "memory_witness"
    prof = (cfg.get("profiles") or {}).get(name) or {}
    sb = scoreboard or {}

    affect = affect_from_scoreboard(sb, st.get("affect") or {}, lo, hi)
    traits = traits_from_affect(st.get("baseline") or {}, st.get("traits") or {}, affect, cfg)
    # safety: high vigilance suppresses boldness/curiosity hard
    if affect["vigilance"] > 0.7:
        traits["boldness"] = min(traits["boldness"], 0.55)
        traits["curiosity"] = min(traits["curiosity"], 0.60)
    if affect["correction"] > 0.7:
        traits["loyalty_to_charter"] = max(traits["loyalty_to_charter"], 0.70)
        traits["boldness"] = min(traits["boldness"], 0.50)

    cons, drift = consistency_score(traits, st.get("baseline") or {})
    bias = compute_bias(prof, traits, cfg)

    out = {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "profile": name,
        "title": prof.get("title") or name,
        "blurb": prof.get("blurb") or "",
        "baseline": st.get("baseline"),
        "traits": traits,
        "affect": affect,
        "consistency": round(cons, 4),
        "trait_drift": round(drift, 4),
        "charter_alignment": round(float(traits.get("loyalty_to_charter", 0.7)), 4),
        "cycles": int(st.get("cycles") or 0) + 1,
        "last_bias": bias,
        "voice": prof.get("voice") or {},
        "engine": "personality_homeostatic_v1",
        "clamps": {"min": lo, "max": hi},
    }

    GOV.mkdir(parents=True, exist_ok=True)
    STATE_PATH.write_text(json.dumps(out, indent=2) + "\n")
    with HIST_PATH.open("a") as f:
        f.write(
            json.dumps(
                {
                    "ts": out["ts"],
                    "profile": name,
                    "affect": affect,
                    "traits": traits,
                    "consistency": out["consistency"],
                    "trait_drift": out["trait_drift"],
                    "reward": affect["reward"],
                    "frustration": affect["frustration"],
                }
            )
            + "\n"
        )
    return out


def self_test() -> int:
    cfg = parse_personality_config(CFG)
    assert cfg.get("active_profile")
    st0 = default_state(cfg)
    assert 0.15 <= st0["traits"]["caution"] <= 0.85
    sb = {
        "d_backlog_pressure": -2.0,
        "d_eval_jtc": 0.05,
        "eval_jtc_delta": 0.44,
        "hermes_task_fail_rate": 0.1,
        "plateau": 0,
        "busy": 0,
        "teacher_uptime": 1.0,
        "eval_veto": 0,
        "learning_velocity": 2.0,
        "backlog_pressure": 5,
        "bonsai_ok": 1,
    }
    # use temp state dir if sandbox
    a = affect_from_scoreboard(sb, {k: 0.5 for k in AFFECT_KEYS}, 0.15, 0.85)
    assert 0.15 <= a["reward"] <= 0.85
    assert a["reward"] > 0.5  # progress should lift reward
    sb_bad = dict(sb, d_backlog_pressure=3.0, plateau=1, eval_veto=1, d_eval_jtc=-0.1)
    a2 = affect_from_scoreboard(sb_bad, a, 0.15, 0.85)
    assert a2["frustration"] >= a["frustration"] - 0.05
    assert a2["vigilance"] >= 0.15
    # no extremes after many steps
    tr = {k: 0.5 for k in TRAIT_KEYS}
    base = {k: 0.5 for k in TRAIT_KEYS}
    cfg2 = {"clamp_min": 0.15, "clamp_max": 0.85, "max_trait_delta": 0.04, "homeostasis": 0.08, "frozen": 0}
    for _ in range(50):
        tr = traits_from_affect(base, tr, a2, cfg2)
    for k, v in tr.items():
        assert 0.15 <= v <= 0.85, (k, v)
    bias = compute_bias({"prefer_goals": ["peft_jtc"], "avoid_goals": ["break_plateau"]}, tr, {"bias_strength": 1.0})
    assert bias.get("peft_jtc", 0) <= 0
    print("PERSONALITY_SELFTEST_PASS checks=6")
    return 0


if __name__ == "__main__":
    import sys

    if "--test" in sys.argv:
        raise SystemExit(self_test())
    # optional: load last scoreboard
    sb = {}
    sp = GOV / "scoreboard.json"
    if sp.exists():
        try:
            sb = json.loads(sp.read_text())
        except Exception:
            pass
    out = tick(sb)
    print(
        "PERSONALITY_TICK_OK",
        json.dumps(
            {
                "profile": out["profile"],
                "reward": out["affect"]["reward"],
                "frustration": out["affect"]["frustration"],
                "caution": out["traits"]["caution"],
                "consistency": out["consistency"],
            }
        ),
    )
