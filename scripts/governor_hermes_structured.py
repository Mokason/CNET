#!/usr/bin/env python3
"""Structured Hermes task outcomes from state.db messages (not noisy log tails)."""
from __future__ import annotations

import json
import os
import sqlite3
import time
from pathlib import Path

OUT = Path(os.environ.get("CNET_GOVERNOR_DIR", "logs/governor")) / "hermes_structured.json"
HOURS = float(os.environ.get("HERMES_STRUCT_HOURS", "12"))
DB = Path(os.environ.get("HERMES_STATE_DB", Path.home() / ".hermes/state.db"))

FAIL_MARKERS = (
    "tool error",
    "tool_call failed",
    "traceback",
    "exception:",
    "timeout waiting",
    "command failed",
    "exit code 1",
    '"ok": false',
    '"ok":false',
    "error running tool",
    "failed after",
)
OK_MARKERS = (
    '"ok": true',
    '"ok":true',
    "completed successfully",
    "GOVERNOR_QUALITY_PASS",
    "VERIFY_FAST_PASS",
    "exit code 0",
)


def main() -> int:
    if not DB.exists() or DB.stat().st_size < 1000:
        rep = {
            "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "db": str(DB),
            "fails": 0,
            "oks": 0,
            "hermes_task_fail_rate": 0.0,
            "noisy": True,
            "reason": "db_missing_or_empty",
        }
        OUT.parent.mkdir(parents=True, exist_ok=True)
        OUT.write_text(json.dumps(rep, indent=2) + "\n")
        print("HERMES_STRUCTURED_OK", json.dumps(rep))
        return 0

    cutoff = time.time() - HOURS * 3600
    conn = sqlite3.connect(f"file:{DB}?mode=ro", uri=True)
    # timestamp may be unix float or iso string
    rows = conn.execute(
        """
        SELECT role, content, tool_name, timestamp
        FROM messages
        WHERE active = 1
        ORDER BY id DESC
        LIMIT 2500
        """
    ).fetchall()
    conn.close()

    fails = oks = 0
    samples = []
    tool_fail = {}
    for role, content, tool_name, ts in rows:
        # time filter
        keep = True
        if ts is not None:
            try:
                tsf = float(ts)
                if tsf > 1e12:  # ms
                    tsf /= 1000.0
                if tsf < cutoff:
                    keep = False
            except Exception:
                # iso-ish: keep recent scan window only (already limited)
                keep = True
        if not keep:
            continue
        text = str(content or "")
        low = text.lower()
        role_l = str(role or "").lower()
        if role_l == "system":
            continue
        is_fail = any(m in low for m in FAIL_MARKERS)
        is_ok = any(m in low for m in OK_MARKERS)
        if role_l == "tool" and tool_name and "error" in low:
            is_fail = True
        if is_fail:
            fails += 1
            tn = tool_name or "unknown"
            tool_fail[tn] = tool_fail.get(tn, 0) + 1
            if len(samples) < 6:
                samples.append(text[:160].replace("\n", " "))
        elif is_ok:
            oks += 1

    total = fails + oks
    if total >= 8:
        rate = fails / total
        noisy = False
    elif fails == 0:
        rate = 0.0
        noisy = True
    else:
        rate = min(0.25, fails / 20.0)
        noisy = True

    rep = {
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "db": str(DB),
        "hours": HOURS,
        "fails": fails,
        "oks": oks,
        "hermes_task_fail_rate": round(rate, 4),
        "tool_fail_top": sorted(tool_fail.items(), key=lambda x: -x[1])[:8],
        "samples": samples,
        "sufficient": total >= 8,
        "noisy": noisy,
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(rep, indent=2) + "\n")
    print(
        "HERMES_STRUCTURED_OK",
        json.dumps(
            {
                "fail_rate": rep["hermes_task_fail_rate"],
                "fails": fails,
                "oks": oks,
                "noisy": noisy,
            }
        ),
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
