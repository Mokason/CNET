#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
import sqlite3
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "activate_cnet_suggestions.py"
spec = importlib.util.spec_from_file_location("activate_cnet_suggestions", MODULE_PATH)
assert spec is not None and spec.loader is not None
activation = importlib.util.module_from_spec(spec)
spec.loader.exec_module(activation)

SCHEMA = """
CREATE TABLE suggestions (
 id INTEGER PRIMARY KEY AUTOINCREMENT, hash TEXT UNIQUE NOT NULL,
 area TEXT NOT NULL, title TEXT NOT NULL, suggestion TEXT NOT NULL,
 target_file TEXT DEFAULT '', priority INTEGER DEFAULT 3,
 source TEXT DEFAULT 'research', status TEXT DEFAULT 'proposed',
 original_id INTEGER, notes TEXT DEFAULT '', created_at TEXT NOT NULL,
 updated_at TEXT, verified_at TEXT, score REAL DEFAULT 0.0,
 meta TEXT DEFAULT '{}'
);
"""
OLD_SHA = "a" * 64
NEW_SHA = "b" * 64


class BoundedActivationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.db = self.root / "registry.db"
        self.recovery = self.root / "recovery.json"
        self.acceptance = self.root / "acceptance.json"
        with sqlite3.connect(self.db) as connection:
            connection.executescript(SCHEMA)
        self._write_reports()

    def tearDown(self) -> None:
        self.temp.cleanup()

    def _insert(
        self,
        *,
        status: str = "proposed",
        actionable: bool = True,
        priority: int = 5,
        source: str = "cnet_real_model_acceptance",
        old_sha: str = OLD_SHA,
    ) -> int:
        meta = json.dumps(
            {
                "actionable": actionable,
                "candidate_sha256": old_sha,
                "admission_reasons": ["quality_regression"],
            }
        )
        with sqlite3.connect(self.db) as connection:
            cursor = connection.execute(
                """INSERT INTO suggestions
                (hash,area,title,suggestion,target_file,priority,source,status,notes,created_at,meta)
                VALUES (?,?,?,?,?,?,?,?,?,?,?)""",
                (
                    f"hash-{source}-{status}-{actionable}-{priority}-{connection.total_changes}",
                    "cnet-model-compression",
                    "Repair quarantined real-model compression candidate",
                    "repair",
                    "tools/run_real_model_acceptance.py",
                    priority,
                    source,
                    status,
                    "",
                    "2026-07-16T00:00:00Z",
                    meta,
                ),
            )
            assert cursor.lastrowid is not None
            return int(cursor.lastrowid)

    def _write_reports(
        self,
        *,
        admitted: bool = True,
        quality_delta: float = 1 / 3,
        max_quality_delta: float = 0.0,
        old_sha: str = OLD_SHA,
    ) -> None:
        self.recovery.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "root_cause": {"quarantined_sha256": old_sha},
                    "replacement": {"sha256": NEW_SHA},
                    "acceptance": {
                        "verdict": "PASS_CANDIDATE_ADMITTED" if admitted else "PASS_CANDIDATE_QUARANTINED",
                        "quality_delta": quality_delta,
                        "qgkp_byte_identical": True,
                        "restart_responses_identical": True,
                        "restart_quality_preserved": True,
                    },
                    "hermes": {
                        "status": "pass",
                        "matched": True,
                        "returncode": 0,
                        "cpu_only": True,
                        "loopback_only": True,
                    },
                    "verdict": "QWYTHOS_RECOVERY_AND_HERMES_ACCEPTANCE_PASS" if admitted else "RECOVERY_FAILED",
                }
            )
        )
        self.acceptance.write_text(
            json.dumps(
                {
                    "schema_version": 2,
                    "execution": {"cpu_only": True, "max_quality_delta": max_quality_delta},
                    "artifacts": {
                        "candidate": {"sha256": NEW_SHA},
                        "qgkp": {"round_trip": {"byte_identical": True}},
                    },
                    "admission": {
                        "admitted": admitted,
                        "selected_role": "candidate" if admitted else "reference",
                        "quality_delta": quality_delta,
                        "reasons": [] if admitted else ["quality_regression"],
                    },
                    "restart_integrity": {"responses_identical": True, "quality_preserved": True},
                    "overall_pass": True,
                    "verdict": "PASS_CANDIDATE_ADMITTED" if admitted else "PASS_CANDIDATE_QUARANTINED",
                }
            )
        )

    def _row(self, row_id: int) -> sqlite3.Row:
        with sqlite3.connect(self.db) as connection:
            connection.row_factory = sqlite3.Row
            row = connection.execute("SELECT * FROM suggestions WHERE id=?", (row_id,)).fetchone()
        assert row is not None
        return row

    def test_selects_only_actionable_cnet_proposed_or_recoverable_rows(self) -> None:
        wanted = self._insert()
        self._insert(actionable=False, priority=4)
        self._insert(source="research", priority=3)
        rows = activation.select_actionable(self.db)
        self.assertEqual([row["id"] for row in rows], [wanted])

    def test_dry_run_is_pure(self) -> None:
        row_id = self._insert()
        result = activation.activate(self.db, self.recovery, self.acceptance, dry_run=True)
        self.assertEqual(result["status"], "dry_run")
        self.assertEqual(self._row(row_id)["status"], "proposed")

    def test_valid_evidence_verifies_exactly_one_row(self) -> None:
        first = self._insert(priority=5)
        second = self._insert(priority=4)
        result = activation.activate(self.db, self.recovery, self.acceptance)
        self.assertEqual(result["status"], "verified")
        self.assertEqual(result["processed"], 1)
        self.assertEqual(self._row(first)["status"], "verified")
        self.assertEqual(self._row(second)["status"], "proposed")
        self.assertFalse(json.loads(self._row(first)["meta"])["actionable"])

    def test_in_progress_row_resumes_after_crash(self) -> None:
        row_id = self._insert(status="in_progress")
        result = activation.activate(self.db, self.recovery, self.acceptance)
        self.assertTrue(result["resumed"])
        self.assertEqual(self._row(row_id)["status"], "verified")

    def test_crash_after_claim_leaves_resumable_state(self) -> None:
        row_id = self._insert()
        with self.assertRaises(activation.ActivationInterrupted):
            activation.activate(
                self.db,
                self.recovery,
                self.acceptance,
                crash_after_claim=True,
            )
        self.assertEqual(self._row(row_id)["status"], "in_progress")
        self.assertEqual(activation.activate(self.db, self.recovery, self.acceptance)["status"], "verified")

    def test_policy_weakening_is_archived_not_accepted(self) -> None:
        row_id = self._insert()
        self._write_reports(max_quality_delta=0.1)
        result = activation.activate(self.db, self.recovery, self.acceptance)
        self.assertEqual(result["status"], "archived")
        self.assertIn("max_quality_delta_not_zero", result["reasons"])
        self.assertEqual(self._row(row_id)["status"], "archived")

    def test_persistent_quality_regression_is_archived_and_not_reproposed(self) -> None:
        row_id = self._insert()
        self._write_reports(admitted=False, quality_delta=-1 / 3)
        result = activation.activate(self.db, self.recovery, self.acceptance)
        self.assertEqual(result["status"], "archived")
        self.assertEqual(self._row(row_id)["status"], "archived")
        self.assertEqual(activation.select_actionable(self.db), [])

    def test_candidate_sha_must_link_task_to_recovery(self) -> None:
        row_id = self._insert(old_sha="c" * 64)
        result = activation.activate(self.db, self.recovery, self.acceptance)
        self.assertEqual(result["status"], "archived")
        self.assertIn("quarantined_sha256_mismatch", result["reasons"])
        self.assertEqual(self._row(row_id)["status"], "archived")


if __name__ == "__main__":
    unittest.main()
