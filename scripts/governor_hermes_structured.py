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
    "traceback (most recent call last)",
    "exception:",
    "timeout waiting",
    "command failed",
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
)
# status values that are terminal failures / not-yet-outcomes
FAIL_STATUS = {"error", "blocked", "failed", "denied", "rejected"}
PENDING_STATUS = {"pending_approval", "pending", "awaiting_approval", "running"}


def classify(text: str, tool_name: str | None) -> str:
    """Classify one tool result as 'ok' | 'fail' | 'skip'.

    The Hermes tool envelope is {"output":..., "error":..., "exit_code":...} and
    the `error` key is present on EVERY terminal result, usually empty. The old
    rule (`"error" in text.lower()` -> fail) therefore flagged essentially every
    tool message as a failure — 470 "fails" vs 54 oks, a fixed ~0.90 rate whose
    stored samples were plainly successful runs. Decide on the envelope fields;
    fall back to text markers only for non-JSON payloads.
    """
    stripped = text.strip()
    obj = None
    if stripped[:1] in ("{", "["):
        try:
            obj = json.loads(stripped)
        except Exception:
            obj = None

    if isinstance(obj, dict):
        status = str(obj.get("status") or "").strip().lower()
        if status in PENDING_STATUS:
            return "skip"
        if status in FAIL_STATUS:
            return "fail"
        err = obj.get("error")
        if isinstance(err, str) and err.strip():
            return "fail"
        if err not in (None, "", [], {}, False) and not isinstance(err, str):
            return "fail"
        code = obj.get("exit_code")
        if code is not None:
            try:
                if int(code) != 0:
                    return "fail"
            except Exception:
                return "fail"
        return "ok"

    low = stripped.lower()
    if any(m in low for m in FAIL_MARKERS):
        return "fail"
    if any(m.lower() in low for m in OK_MARKERS):
        return "ok"
    # A returned tool payload with no error signal is a success.
    return "ok"


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

    fails = oks = skipped = 0
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
        role_l = str(role or "").lower()
        # Only tool results carry task outcomes. Assistant/user prose mentioning
        # "traceback" or "failed" is discussion, not a failed task.
        if role_l != "tool":
            continue
        verdict = classify(text, tool_name)
        if verdict == "skip":
            skipped += 1
            continue
        if verdict == "fail":
            fails += 1
            tn = tool_name or "unknown"
            tool_fail[tn] = tool_fail.get(tn, 0) + 1
            if len(samples) < 6:
                samples.append(text[:160].replace("\n", " "))
        else:
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
        "pending_skipped": skipped,
        "hermes_task_fail_rate": round(rate, 4),
        "tool_fail_top": sorted(tool_fail.items(), key=lambda x: -x[1])[:8],
        "samples": samples,
        "sufficient": total >= 8,
        "noisy": noisy,
        "classifier": "envelope_v2",
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
