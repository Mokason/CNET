#!/usr/bin/env python3
"""Bounded, resumable consumer for evidence-backed CNET registry rows.

The tool processes at most one CNET real-model row per invocation. It never
runs a scheduler, mutates source code, invokes git, or weakens acceptance
policy. Existing acceptance/recovery reports are the only completion evidence.
"""

from __future__ import annotations

import argparse
import json
import sqlite3
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

AREA = "cnet-model-compression"
SOURCE = "cnet_real_model_acceptance"
MAX_QUALITY_DELTA = 0.0
VALID_RECOVERY_VERDICT = "QWYTHOS_RECOVERY_AND_HERMES_ACCEPTANCE_PASS"
VALID_ACCEPTANCE_VERDICT = "PASS_CANDIDATE_ADMITTED"


class ActivationInterrupted(RuntimeError):
    """Testable interruption after the durable in-progress checkpoint."""


class EvidenceUnavailable(RuntimeError):
    """Evidence could not be read; row remains resumable in-progress."""


def _timestamp() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def _connect(db: Path) -> sqlite3.Connection:
    connection = sqlite3.connect(db, timeout=30.0)
    connection.row_factory = sqlite3.Row
    return connection


def _actionable_query(limit: int = 1) -> str:
    return f"""
        SELECT * FROM suggestions
        WHERE area=? AND source=?
          AND status IN ('in_progress','proposed')
          AND json_valid(meta)
          AND json_extract(meta, '$.actionable') = 1
        ORDER BY CASE status WHEN 'in_progress' THEN 0 ELSE 1 END,
                 priority DESC, created_at ASC, id ASC
        LIMIT {int(limit)}
    """


def select_actionable(db: Path, limit: int = 1) -> list[sqlite3.Row]:
    if limit < 1:
        raise ValueError("limit must be positive")
    with _connect(db) as connection:
        return list(connection.execute(_actionable_query(limit), (AREA, SOURCE)))


def _claim_one(db: Path) -> tuple[sqlite3.Row | None, bool]:
    with _connect(db) as connection:
        connection.execute("BEGIN IMMEDIATE")
        row = connection.execute(_actionable_query(1), (AREA, SOURCE)).fetchone()
        if row is None:
            connection.commit()
            return None, False
        resumed = row["status"] == "in_progress"
        meta = json.loads(row["meta"])
        activation = dict(meta.get("activation") or {})
        activation["attempt_count"] = int(activation.get("attempt_count", 0)) + 1
        activation["state"] = "in_progress"
        activation["claimed_at"] = _timestamp()
        activation["resumed"] = resumed
        activation["policy"] = {"max_quality_delta": MAX_QUALITY_DELTA}
        meta["activation"] = activation
        connection.execute(
            "UPDATE suggestions SET status='in_progress', updated_at=?, meta=? WHERE id=?",
            (_timestamp(), json.dumps(meta, sort_keys=True, separators=(",", ":")), row["id"]),
        )
        connection.commit()
    claimed = select_actionable(db, 1)
    return (claimed[0] if claimed else None), resumed


def _load_report(path: Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise EvidenceUnavailable(f"{label}_unavailable:{exc}") from exc
    if not isinstance(value, dict):
        raise EvidenceUnavailable(f"{label}_not_object")
    return value


def _as_float(value: Any, default: float) -> float:
    if isinstance(value, (int, float, str)):
        try:
            return float(value)
        except ValueError:
            pass
    return default


def validate_evidence(
    row: sqlite3.Row,
    recovery_report: Path,
    acceptance_report: Path,
) -> tuple[list[str], dict[str, Any]]:
    recovery = _load_report(recovery_report, "recovery_report")
    acceptance = _load_report(acceptance_report, "acceptance_report")
    reasons: list[str] = []
    meta = json.loads(row["meta"])

    task_sha = str(meta.get("candidate_sha256") or "")
    recovered_from_sha = str((recovery.get("root_cause") or {}).get("quarantined_sha256") or "")
    replacement_sha = str((recovery.get("replacement") or {}).get("sha256") or "")
    accepted_sha = str(((acceptance.get("artifacts") or {}).get("candidate") or {}).get("sha256") or "")

    if len(task_sha) != 64 or recovered_from_sha != task_sha:
        reasons.append("quarantined_sha256_mismatch")
    if len(replacement_sha) != 64 or accepted_sha != replacement_sha:
        reasons.append("replacement_sha256_mismatch")

    execution = acceptance.get("execution") or {}
    if execution.get("cpu_only") is not True:
        reasons.append("acceptance_not_cpu_only")
    policy_delta = _as_float(execution.get("max_quality_delta"), float("nan"))
    if policy_delta != MAX_QUALITY_DELTA:
        reasons.append("max_quality_delta_not_zero")

    admission = acceptance.get("admission") or {}
    quality_delta = _as_float(admission.get("quality_delta"), float("-inf"))
    if admission.get("admitted") is not True:
        reasons.append("candidate_not_admitted")
    if admission.get("selected_role") != "candidate":
        reasons.append("candidate_not_selected")
    if quality_delta < -MAX_QUALITY_DELTA:
        reasons.append("quality_regression")
    if admission.get("reasons") not in ([], None):
        reasons.append("admission_reasons_present")

    qgkp = ((acceptance.get("artifacts") or {}).get("qgkp") or {}).get("round_trip") or {}
    if qgkp.get("byte_identical") is not True:
        reasons.append("qgkp_round_trip_failed")
    restart = acceptance.get("restart_integrity") or {}
    if restart.get("responses_identical") is not True:
        reasons.append("restart_responses_changed")
    if restart.get("quality_preserved") is not True:
        reasons.append("restart_quality_regressed")
    if acceptance.get("overall_pass") is not True:
        reasons.append("acceptance_campaign_failed")
    if acceptance.get("verdict") != VALID_ACCEPTANCE_VERDICT:
        reasons.append("acceptance_verdict_invalid")

    recovery_acceptance = recovery.get("acceptance") or {}
    if recovery_acceptance.get("qgkp_byte_identical") is not True:
        reasons.append("recovery_qgkp_failed")
    if recovery_acceptance.get("restart_responses_identical") is not True:
        reasons.append("recovery_restart_changed")
    if recovery_acceptance.get("restart_quality_preserved") is not True:
        reasons.append("recovery_restart_quality_regressed")
    if recovery.get("verdict") != VALID_RECOVERY_VERDICT:
        reasons.append("recovery_verdict_invalid")

    hermes = recovery.get("hermes") or {}
    if not (
        hermes.get("status") == "pass"
        and hermes.get("matched") is True
        and hermes.get("returncode") == 0
        and hermes.get("cpu_only") is True
        and hermes.get("loopback_only") is True
    ):
        reasons.append("hermes_acceptance_failed")

    evidence = {
        "task_candidate_sha256": task_sha,
        "replacement_sha256": replacement_sha,
        "acceptance_quality_delta": quality_delta,
        "max_quality_delta": policy_delta,
        "recovery_report": str(recovery_report),
        "acceptance_report": str(acceptance_report),
    }
    return list(dict.fromkeys(reasons)), evidence


def _finalize(
    db: Path,
    row_id: int,
    status: str,
    reasons: list[str],
    evidence: dict[str, Any],
) -> None:
    now = _timestamp()
    with _connect(db) as connection:
        connection.execute("BEGIN IMMEDIATE")
        row = connection.execute("SELECT meta,notes,status FROM suggestions WHERE id=?", (row_id,)).fetchone()
        if row is None:
            raise sqlite3.IntegrityError(f"row {row_id} disappeared")
        if row["status"] != "in_progress":
            raise sqlite3.IntegrityError(f"row {row_id} is not in_progress")
        meta = json.loads(row["meta"])
        meta["actionable"] = False
        activation = dict(meta.get("activation") or {})
        activation.update(
            {
                "state": status,
                "completed_at": now,
                "reasons": reasons,
                "evidence": evidence,
            }
        )
        meta["activation"] = activation
        note = (
            "Bounded CNET activation verified existing recovery evidence."
            if status == "verified"
            else "Bounded CNET activation archived the row because evidence failed: " + ", ".join(reasons)
        )
        old_notes = str(row["notes"] or "").strip()
        notes = f"{old_notes}\n{note}".strip()
        connection.execute(
            """UPDATE suggestions
               SET status=?, updated_at=?, verified_at=?, notes=?, meta=?
               WHERE id=?""",
            (
                status,
                now,
                now if status == "verified" else None,
                notes,
                json.dumps(meta, sort_keys=True, separators=(",", ":")),
                row_id,
            ),
        )
        connection.commit()


def activate(
    db: Path,
    recovery_report: Path,
    acceptance_report: Path,
    *,
    dry_run: bool = False,
    crash_after_claim: bool = False,
) -> dict[str, Any]:
    preview = select_actionable(db, 1)
    if not preview:
        return {"status": "idle", "processed": 0}
    if dry_run:
        row = preview[0]
        return {
            "status": "dry_run",
            "processed": 0,
            "would_process": int(row["id"]),
            "resumable": row["status"] == "in_progress",
        }

    row, resumed = _claim_one(db)
    if row is None:
        return {"status": "idle", "processed": 0}
    if crash_after_claim:
        raise ActivationInterrupted(f"interrupted after durable claim of row {row['id']}")

    try:
        reasons, evidence = validate_evidence(row, recovery_report, acceptance_report)
    except EvidenceUnavailable as exc:
        return {
            "status": "evidence_unavailable",
            "processed": 0,
            "row_id": int(row["id"]),
            "resumed": resumed,
            "reason": str(exc),
        }

    final_status = "archived" if reasons else "verified"
    _finalize(db, int(row["id"]), final_status, reasons, evidence)
    return {
        "status": final_status,
        "processed": 1,
        "row_id": int(row["id"]),
        "resumed": resumed,
        "reasons": reasons,
        "evidence": evidence,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--db", type=Path, required=True)
    parser.add_argument("--recovery-report", type=Path, required=True)
    parser.add_argument("--acceptance-report", type=Path, required=True)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    try:
        result = activate(
            args.db,
            args.recovery_report,
            args.acceptance_report,
            dry_run=args.dry_run,
        )
    except (OSError, ValueError, sqlite3.Error) as exc:
        result = {"status": "error", "reason": str(exc)}
        print(json.dumps(result, sort_keys=True))
        return 2
    print(json.dumps(result, sort_keys=True))
    if result["status"] in {"verified", "idle", "dry_run"}:
        return 0
    if result["status"] == "evidence_unavailable":
        return 2
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
