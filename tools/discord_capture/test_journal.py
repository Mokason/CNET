"""Synthetic contract fixtures; never live demand or training evidence."""
import json
import os
from pathlib import Path
import tempfile
import unittest
import hashlib
import sqlite3
from unittest.mock import patch

from journal import Journal, CaptureError, readiness

OWNER = "111111111111111111"
CHANNEL = "222222222222222222"


def message(**changes):
    return dict(id="333333333333333333", channel_id=CHANNEL,
                author={"id": OWNER, "bot": False}, type=0,
                timestamp="2026-09-07T12:00:00.000000+00:00",
                content="  private question  ", **changes)


class JournalTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.journal = Journal(self.root, OWNER, CHANNEL)
        self.addCleanup(self.journal.close)

    def begin(self, data=None):
        return self.journal.begin(data or message(), "private question")

    def test_selected_request_commits_before_completion_without_answer_label(self):
        self.assertTrue(self.begin())
        report = readiness(self.root)
        self.assertEqual(report["requests"], 1)
        self.assertEqual(report["unfinished"], 1)
        self.journal.finish(message()["id"], "peer_ok", "reply_sent")
        report = readiness(self.root)
        self.assertEqual(report["unfinished"], 0)
        self.assertEqual(report["unmapped"], 1)
        self.assertFalse(report["training_eligible"])
        self.assertNotIn("private question", json.dumps(report))
        row = self.journal.db.execute("SELECT raw_text,delivered_text FROM requests").fetchone()
        self.assertEqual(row, ("  private question  ", "private question"))

    def test_scope_bots_webhooks_forwards_systems_and_guilds_are_excluded(self):
        variants = [dict(channel_id="444444444444444444"),
                    dict(author={"id": "444444444444444444"}),
                    dict(author={"id": OWNER, "bot": True}),
                    dict(webhook_id="555555555555555555"), dict(type=7),
                    dict(guild_id="555555555555555555"),
                    dict(message_snapshots=[{}]), dict(message_reference={"type": 1})]
        for changes in variants:
            data = message()
            data.update(changes)
            self.assertFalse(self.journal.selected(data), changes)
            self.assertFalse(self.journal.begin(data, "text"), changes)
        self.assertEqual(readiness(self.root)["requests"], 0)

    def test_duplicate_and_restart_do_not_repeat_attempt(self):
        self.assertTrue(self.begin())
        self.assertFalse(self.begin())
        self.journal.close()
        self.journal = Journal(self.root, OWNER, CHANNEL)
        self.addCleanup(self.journal.close)
        self.assertFalse(self.begin())
        report = readiness(self.root)
        self.assertEqual(report["requests"], 1)
        self.assertEqual(report["unfinished"], 1)
        self.assertEqual(report["segments"], 2)

    def test_policy_cannot_change_on_existing_dataset(self):
        self.journal.close()
        with self.assertRaises(CaptureError):
            Journal(self.root, OWNER, "555555555555555555")

    def test_missing_or_bad_policy_refuses(self):
        for value in ["", "*", "123,456", "../file", True]:
            with self.assertRaises(CaptureError):
                Journal(self.root, value, CHANNEL)

    def test_private_files_and_single_writer(self):
        self.assertEqual((self.root / "capture.sqlite").stat().st_mode & 0o777, 0o600)
        with self.assertRaises(CaptureError):
            Journal(self.root, OWNER, CHANNEL)

    def test_unsafe_directory_file_symlink_and_hardlink_refuse(self):
        self.journal.close()
        self.root.chmod(0o750)
        with self.assertRaises(CaptureError):
            Journal(self.root, OWNER, CHANNEL)
        self.root.chmod(0o700)
        database = self.root / "capture.sqlite"
        database.chmod(0o644)
        with self.assertRaises(CaptureError):
            readiness(self.root)
        database.chmod(0o600)
        os.link(database, self.root / "linked")
        with self.assertRaises(CaptureError):
            readiness(self.root)
        (self.root / "linked").unlink()
        database.rename(self.root / "saved")
        database.symlink_to(self.root / "saved")
        with self.assertRaises(CaptureError):
            Journal(self.root, OWNER, CHANNEL)

    def test_malformed_selected_input_refuses_without_truncation(self):
        for changes in [dict(id="invalid"), dict(timestamp="2026-02-30T00:00:00Z"),
                        dict(content="x" * 16385), dict(content=7)]:
            data = message()
            data.update(changes)
            with self.assertRaises(CaptureError):
                self.begin(data)
        self.assertEqual(readiness(self.root)["requests"], 0)

    def test_reply_failure_is_not_success_and_answer_cannot_be_label(self):
        self.begin()
        with self.assertRaises(CaptureError):
            self.journal.finish(message()["id"], "private answer", "reply_sent")
        self.journal.finish(message()["id"], "peer_error", "reply_failed")
        report = readiness(self.root)
        self.assertEqual(report["peer_failures"], 1)
        self.assertEqual(report["reply_failures"], 1)
        self.assertEqual(report["verified_labels"], 0)

    def test_quota_failure_propagates_and_existing_rows_survive(self):
        self.begin()
        with patch("journal.MAX_REQUESTS", 1):
            data = message()
            data["id"] = "333333333333333334"
            with self.assertRaises(CaptureError):
                self.begin(data)
        self.assertEqual(readiness(self.root)["requests"], 1)

    def test_events_mark_sequence_gaps_and_ready(self):
        self.journal.observe_sequence(1)
        self.journal.event("gateway_ready")
        self.journal.observe_sequence(3)
        report = readiness(self.root)
        self.assertEqual(report["sequence_gaps"], 1)
        self.assertEqual(report["ready_segments"], 1)

    def test_completion_is_one_time_and_same_segment(self):
        with self.assertRaises(CaptureError):
            self.journal.finish(message()["id"], "peer_ok", "reply_sent")
        self.begin()
        self.journal.finish(message()["id"], "peer_ok", "reply_sent")
        with self.assertRaises(CaptureError):
            self.journal.finish(message()["id"], "peer_ok", "reply_sent")

    def test_missing_policy_never_relabels_old_requests(self):
        self.begin()
        self.journal.db.execute("DELETE FROM policy")
        self.journal.db.commit()
        self.journal.close()
        with self.assertRaises(CaptureError):
            Journal(self.root, "555555555555555555", CHANNEL)
        with self.assertRaises(CaptureError):
            readiness(self.root)

    def test_unsupported_schema_is_not_repaired(self):
        self.journal.db.execute("DROP TABLE events")
        self.journal.db.commit()
        self.journal.close()
        with self.assertRaises(CaptureError):
            Journal(self.root, OWNER, CHANNEL)

    def test_page_size_must_keep_exact_byte_bound(self):
        self.journal.db.execute("PRAGMA page_size=65536")
        self.journal.db.execute("VACUUM")
        self.journal.close()
        with self.assertRaises(CaptureError):
            Journal(self.root, OWNER, CHANNEL)

    def test_regression_event_quota_rolls_back_request(self):
        with patch("journal.time.time_ns", return_value=100):
            self.begin()
        data = message()
        data["id"] = "333333333333333334"
        with patch("journal.MAX_EVENTS", 1), patch("journal.time.time_ns", return_value=90):
            with self.assertRaises(CaptureError):
                self.begin(data)
        self.assertEqual(readiness(self.root)["requests"], 1)

    def test_sqlite_full_leaves_no_committed_request(self):
        pages = self.journal.db.execute("PRAGMA page_count").fetchone()[0]
        self.journal.db.execute("PRAGMA max_page_count=" + str(pages))
        data = message()
        data["content"] = "x" * 16000
        with self.assertRaises(sqlite3.DatabaseError):
            self.journal.begin(data, "y" * 16000)
        self.assertEqual(readiness(self.root)["requests"], 0)

    def test_readiness_never_mutates_store(self):
        self.begin()
        database = self.root / "capture.sqlite"
        before = hashlib.sha256(database.read_bytes()).hexdigest()
        readiness(self.root)
        self.assertEqual(hashlib.sha256(database.read_bytes()).hexdigest(), before)


if __name__ == "__main__":
    unittest.main()
