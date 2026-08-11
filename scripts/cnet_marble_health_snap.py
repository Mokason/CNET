#!/usr/bin/env python3
"""Write logs/marble_24_7/status.json — Hermes-independent health."""
from __future__ import annotations

import json
import os
import subprocess
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / "logs" / "marble_24_7"
OUT = OUT_DIR / "status.json"

# Core stack (no Hermes)
UNITS = [
    "cnet-marble.target",
    "cnet-personal-ai-lane.service",
    "bonsai-server.service",
    "cnet-autoteach.timer",
    "cnet-governor.timer",
    "cnet-janitor.timer",
    "cnet-personal-ai-ops.timer",
    "roe-evolve-tick.timer",
    "cnet-marble-health.timer",
    "marble-heartbeat.service",
    "marble-embeddings.service",
    # optional observe only
    "hermes-gateway.service",
    "ollama.service",
]


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def sysctl(*args: str) -> str:
    try:
        p = subprocess.run(
            ["systemctl", "--user", *args],
            capture_output=True,
            text=True,
            timeout=15,
        )
        return (p.stdout or p.stderr or "").strip()
    except (OSError, subprocess.SubprocessError) as e:
        return f"err:{e}"


def unit_state(name: str) -> dict:
    active = sysctl("is-active", name) or "unknown"
    enabled = sysctl("is-enabled", name) or "unknown"
    return {"active": active, "enabled": enabled}


def port_open(port: int) -> bool:
    try:
        p = subprocess.run(
            ["bash", "-c", f"echo >/dev/tcp/127.0.0.1/{port}"],
            capture_output=True,
            timeout=3,
        )
        return p.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False


def main() -> int:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    units = {u: unit_state(u) for u in UNITS}
    # core without hermes
    core_names = [
        "cnet-personal-ai-lane.service",
        "bonsai-server.service",
        "roe-evolve-tick.timer",
        "cnet-autoteach.timer",
    ]
    core_ok = sum(1 for n in core_names if units.get(n, {}).get("active") in ("active", "waiting"))
    hermes = units.get("hermes-gateway.service", {})
    report = {
        "ts": utc_now(),
        "repo": str(ROOT),
        "hermes_independent": True,
        "linger": None,
        "core_active_count": core_ok,
        "core_names": core_names,
        "hermes_gateway": hermes,
        "hermes_required": False,
        "note": "CNET/Marble must stay up if Hermes updates or restarts",
        "ports": {
            "bonsai_8080": port_open(8080),
            "ollama_11434": port_open(11434),
            "marble_embed_8791": port_open(8791),
        },
        "paths": {
            "packs": str(ROOT / "artifacts" / "roe_daily_packs"),
            "pack_personal": (ROOT / "artifacts" / "roe_daily_packs" / "pack_personal").is_dir(),
            "pack_soul": (ROOT / "artifacts" / "roe_daily_packs" / "pack_soul_marble").is_dir(),
            "evolve_report": (ROOT / "artifacts" / "roe_daily_packs" / "EVOLVE_TICK.json").is_file(),
            "base_cnb": (ROOT / "soul_gemma4v2_final.cnb").is_file(),
        },
        "units": units,
    }
    # linger via loginctl
    try:
        user = os.environ.get("USER") or "marble"
        p = subprocess.run(
            ["loginctl", "show-user", user, "-p", "Linger"],
            capture_output=True,
            text=True,
            timeout=5,
        )
        report["linger"] = (p.stdout or "").strip()
    except (OSError, subprocess.SubprocessError):
        report["linger"] = "unknown"

    OUT.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    # also append jsonl history
    with (OUT_DIR / "status.jsonl").open("a", encoding="utf-8") as f:
        f.write(json.dumps({"ts": report["ts"], "core": core_ok, "hermes": hermes.get("active")}) + "\n")

    print(f"wrote {OUT}")
    print(f"core_active={core_ok}/{len(core_names)} hermes={hermes.get('active')} linger={report.get('linger')}")
    print("CNET_MARBLE_HEALTH_OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
