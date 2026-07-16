#!/usr/bin/env python3
"""Ingest CNET evidence into an existing local SuggestionRegistry SQLite DB."""

from __future__ import annotations

import argparse
import hashlib
import json
import sqlite3
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

REQUIRED_COLUMNS = {
    "hash", "area", "title", "suggestion", "target_file", "priority",
    "source", "status", "notes", "created_at", "score", "meta",
}


def _digest(key: str) -> str:
    return hashlib.sha256(key.encode("utf-8")).hexdigest()


def _timestamp(value: str | None = None) -> str:
    if value:
        return f"{value}T00:00:00Z" if "T" not in value else value
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def _read_jsonl(path: Path) -> Iterable[dict[str, Any]]:
    with path.open(encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            if not line.strip():
                continue
            value = json.loads(line)
            if not isinstance(value, dict) or not value.get("id"):
                raise ValueError(f"invalid suggestion row at {path}:{line_number}")
            yield value


def _milestone_record(row: dict[str, Any]) -> dict[str, Any]:
    original_status = str(row.get("status", ""))
    status = "implemented" if original_status.startswith("implemented") else "proposed"
    priority = max(1, min(5, round(float(row.get("priority", 0.6)) * 5)))
    evidence_raw = row.get("evidence")
    evidence: dict[str, Any] = evidence_raw if isinstance(evidence_raw, dict) else {}
    meta = {
        "cnet_id": row["id"],
        "plan": row.get("plan"),
        "original_status": original_status,
        "evidence": evidence,
        "next_steps": row.get("next_steps", []),
        "tags": row.get("tags", []),
        "actionable": status == "proposed",
    }
    return {
        "hash": _digest(f"cnet:milestone:{row['id']}"),
        "area": "cnet-model-compression",
        "title": str(row.get("title") or row["id"]),
        "suggestion": str(row.get("description") or "CNET evidence milestone"),
        "target_file": str(row.get("plan") or ""),
        "priority": priority,
        "source": str(row.get("source") or "cnet"),
        "status": status,
        "notes": "Imported from CNET Phase 5 evidence export.",
        "created_at": _timestamp(str(evidence.get("date") or "") or None),
        "score": float(row.get("priority", 0.0)),
        "meta": json.dumps(meta, sort_keys=True, separators=(",", ":")),
    }


def _candidate_record(report: dict[str, Any]) -> dict[str, Any] | None:
    if report.get("overall_pass") is not True:
        raise ValueError("acceptance report is not a valid passing campaign")
    admission = report.get("admission") or {}
    if admission.get("admitted") is True:
        return None
    candidate = (report.get("artifacts") or {}).get("candidate") or {}
    digest = str(candidate.get("sha256") or "")
    reasons = admission.get("reasons") or []
    if len(digest) != 64 or not reasons:
        raise ValueError("quarantined candidate lacks digest or admission reasons")
    quality = report.get("quality") or {}
    delta = float(quality.get("quality_delta", 0.0))
    meta = {
        "candidate_sha256": digest,
        "candidate_path": candidate.get("path"),
        "admission_reasons": reasons,
        "campaign_verdict": report.get("verdict"),
        "quality": quality,
        "actionable": True,
    }
    return {
        "hash": _digest(f"cnet:candidate-quarantine:{digest}"),
        "area": "cnet-model-compression",
        "title": "Repair quarantined real-model compression candidate",
        "suggestion": (
            "Repair or replace the quarantined candidate and rerun the bounded CPU "
            f"acceptance campaign. Admission failed: {', '.join(map(str, reasons))}."
        ),
        "target_file": "tools/run_real_model_acceptance.py",
        "priority": 5,
        "source": "cnet_real_model_acceptance",
        "status": "proposed",
        "notes": "Generated only from a valid campaign with candidate admission denied.",
        "created_at": _timestamp(str(report.get("generated_at") or "") or None),
        "score": abs(delta),
        "meta": json.dumps(meta, sort_keys=True, separators=(",", ":")),
    }


def _validate_schema(connection: sqlite3.Connection) -> None:
    columns = {row[1] for row in connection.execute("PRAGMA table_info(suggestions)")}
    missing = REQUIRED_COLUMNS - columns
    if missing:
        raise ValueError(f"SuggestionRegistry schema missing columns: {sorted(missing)}")


def ingest(suggestions: Path, db: Path, acceptance_report: Path | None = None) -> dict[str, int]:
    records = [_milestone_record(row) for row in _read_jsonl(suggestions)]
    if acceptance_report is not None:
        report = json.loads(acceptance_report.read_text(encoding="utf-8"))
        followup = _candidate_record(report)
        if followup is not None:
            records.append(followup)

    inserted = 0
    with sqlite3.connect(db) as connection:
        _validate_schema(connection)
        for record in records:
            cursor = connection.execute(
                """INSERT OR IGNORE INTO suggestions
                (hash,area,title,suggestion,target_file,priority,source,status,notes,created_at,score,meta)
                VALUES (:hash,:area,:title,:suggestion,:target_file,:priority,:source,:status,:notes,:created_at,:score,:meta)""",
                record,
            )
            inserted += cursor.rowcount
        connection.commit()
        proposed = connection.execute(
            "SELECT COUNT(*) FROM suggestions WHERE source IN (?,?) AND status='proposed'",
            ("cnet_model_compression_phase_log", "cnet_real_model_acceptance"),
        ).fetchone()[0]
    return {"read": len(records), "inserted": inserted, "skipped": len(records) - inserted, "proposed": proposed}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--suggestions", type=Path, required=True)
    parser.add_argument("--db", type=Path, required=True)
    parser.add_argument("--acceptance-report", type=Path)
    args = parser.parse_args()
    try:
        result = ingest(args.suggestions, args.db, args.acceptance_report)
    except (OSError, ValueError, json.JSONDecodeError, sqlite3.Error) as exc:
        print(json.dumps({"status": "error", "reason": str(exc)}))
        return 1
    print(json.dumps({"status": "ok", **result}, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
