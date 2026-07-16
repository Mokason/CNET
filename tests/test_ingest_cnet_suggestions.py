#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
import json
import sqlite3
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "ingest_cnet_suggestions.py"
spec = importlib.util.spec_from_file_location("ingest_cnet_suggestions", MODULE_PATH)
assert spec and spec.loader
ingest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ingest)

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


class IngestCnetSuggestionsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.db = self.root / "registry.db"
        with sqlite3.connect(self.db) as connection:
            connection.executescript(SCHEMA)
        self.jsonl = self.root / "suggestions.jsonl"
        rows = [
            {
                "id": "cnet-phase-4",
                "title": "Hermes integration",
                "source": "cnet_model_compression_phase_log",
                "plan": "plan.md",
                "status": "implemented",
                "priority": 0.84,
                "description": "Implemented and verified.",
                "evidence": {"date": "2026-07-06", "tests": ["make phase4"]},
                "next_steps": ["old step"],
                "tags": ["cnet"],
            }
        ]
        self.jsonl.write_text("\n".join(json.dumps(row) for row in rows) + "\n")
        self.report = self.root / "report.json"
        self.report.write_text(
            json.dumps(
                {
                    "overall_pass": True,
                    "verdict": "PASS_CANDIDATE_QUARANTINED",
                    "artifacts": {
                        "candidate": {"sha256": "a" * 64, "path": "/tmp/bad.gguf"}
                    },
                    "admission": {
                        "admitted": False,
                        "reasons": ["quality_regression", "candidate_probe_failure"],
                    },
                    "quality": {"quality_delta": -0.66},
                }
            )
        )

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_import_preserves_implemented_status_and_creates_evidence_followup(self) -> None:
        result = ingest.ingest(self.jsonl, self.db, self.report)
        self.assertEqual(result["inserted"], 2)
        self.assertEqual(result["proposed"], 1)
        with sqlite3.connect(self.db) as connection:
            rows = connection.execute(
                "SELECT title,status,priority,meta FROM suggestions ORDER BY id"
            ).fetchall()
        self.assertEqual(rows[0][1], "implemented")
        self.assertEqual(rows[1][1], "proposed")
        self.assertEqual(rows[1][2], 5)
        meta = json.loads(rows[1][3])
        self.assertEqual(meta["candidate_sha256"], "a" * 64)
        self.assertIn("quality_regression", meta["admission_reasons"])

    def test_reingest_is_deduplicated_by_stable_hash(self) -> None:
        first = ingest.ingest(self.jsonl, self.db, self.report)
        second = ingest.ingest(self.jsonl, self.db, self.report)
        self.assertEqual(first["inserted"], 2)
        self.assertEqual(second["inserted"], 0)
        self.assertEqual(second["skipped"], 2)
        with sqlite3.connect(self.db) as connection:
            self.assertEqual(connection.execute("SELECT COUNT(*) FROM suggestions").fetchone()[0], 2)

    def test_admitted_candidate_does_not_schedule_repair(self) -> None:
        report = json.loads(self.report.read_text())
        report["admission"]["admitted"] = True
        self.report.write_text(json.dumps(report))
        result = ingest.ingest(self.jsonl, self.db, self.report)
        self.assertEqual(result["inserted"], 1)
        self.assertEqual(result["proposed"], 0)


if __name__ == "__main__":
    unittest.main()
