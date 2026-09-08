"""Synthetic capture/history joins. Never attest origin or mutate live stores."""
import importlib.util
import json
import sqlite3
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock

from capture_task_identity import task_identity
from journal import CaptureError, Journal
from test_journal import OWNER, CHANNEL, message
from test_task_bridge import experience


class TaskIdentityTests(unittest.TestCase):
    def test_only_immutable_scope_message_and_exact_delivered_text_determine_id(self):
        args = (OWNER, CHANNEL, message()["id"], "uppercase µ")
        identity = task_identity(*args)
        self.assertEqual(identity, task_identity(*args))
        self.assertRegex(identity, r"^[a-f0-9]{32}$")
        for changed in (("555555555555555555", *args[1:]), (OWNER, OWNER, *args[2:]),
                        (*args[:2], "555555555555555555", args[3]), (*args[:3], "uppercase μ")):
            self.assertNotEqual(identity, task_identity(*changed))
        for changed in (("bad", *args[1:]), (*args[:3], ""), (*args[:3], "x" * 16385)):
            with self.assertRaises(CaptureError):
                task_identity(*changed)


class TaskInboxTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.find_spec("task_inbox")
        self.assertIsNotNone(spec, "CAPTURE_INBOX_RED: missing read-only capture-side history")
        self.module = importlib.import_module("task_inbox")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.journal = Journal(self.root, OWNER, CHANNEL)
        self.addCleanup(self.journal.close)
        self.bridge = Mock(task_mode=True)
        self.ids = []
        for i in range(11):
            data = dict(message(), id=str(int(message()["id"]) + i), content=f"private fixture {i}")
            self.assertTrue(self.journal.begin(data, data["content"]))
            self.ids.append(task_identity(OWNER, CHANNEL, data["id"], data["content"]))
            if i % 2:
                self.journal.finish(data["id"], "peer_unknown", "reply_unknown")
        def trace(verb, identities):
            self.assertEqual(verb, "trace")
            self.assertLessEqual(len(identities.split(',')), 10)
            return dict(event="learning_trace", correlation_id="a" * 32,
                        matches=[dict(RequestId=identity, Experience=(experience(RequestId=identity,
                            ApprovedSourceSha256="b" * 64, ApprovedExpected=925,
                            ApprovedBoot=experience()["Boot"], ApprovedNanoseconds=3) if identity == self.ids[1] else None))
                            for identity in identities.split(',')])
        self.bridge.command.side_effect = trace

    def test_capture_first_page_keeps_missing_history_and_never_exposes_raw_text_or_eligibility(self):
        before = (self.root / "capture.sqlite").read_bytes()
        report = self.module.inspect_page(self.root, self.bridge, 0, 10)
        self.assertEqual(report["next_after"], 10)
        self.assertTrue(report["has_more"])
        self.assertEqual(len(report["captures"]), 10)
        self.assertFalse(report["training_eligible"])
        self.assertFalse(report["origin_attested"])
        self.assertEqual(report["view"], "capture_side_separate_snapshots")
        first, linked = report["captures"][:2]
        self.assertEqual(first["link_state"], "no_observation")
        self.assertIsNone(first["experience"])
        self.assertIsNone(first["peer_status"])
        self.assertEqual(linked["link_state"], "matched")
        self.assertEqual(linked["peer_status"], "peer_unknown")
        self.assertEqual(linked["experience"]["Origin"], "unreviewed")
        self.assertEqual(linked["experience"]["SourceSha256"], "a" * 64)
        self.assertEqual(linked["experience"]["ApprovedSourceSha256"], "b" * 64)
        encoded = json.dumps(report)
        self.assertNotIn("private fixture", encoded)
        self.assertNotIn(OWNER, encoded)
        self.assertNotIn(CHANNEL, encoded)
        self.assertLess(len(encoded.encode()), 32768)
        self.assertEqual(before, (self.root / "capture.sqlite").read_bytes())
        self.bridge.require_owner.assert_not_called()
        self.assertEqual(self.bridge.check_installation.call_count, 2)
        last = self.module.inspect_page(self.root, self.bridge, 10, 10)
        self.assertEqual(last["next_after"], 11)
        self.assertFalse(last["has_more"])
        self.bridge.command.reset_mock()
        empty = self.module.inspect_page(self.root, self.bridge, 11, 10)
        self.assertEqual(empty["captures"], [])
        self.assertEqual(empty["next_after"], 11)
        self.bridge.command.assert_not_called()

    def test_completion_is_not_part_of_link_and_changed_text_or_scope_does_not_reuse_identity(self):
        before = self.module.inspect_page(self.root, self.bridge, 0, 1)["captures"][0]["task_request_id"]
        self.journal.finish(message()["id"], "peer_ok", "reply_sent")
        self.assertEqual(before, self.module.inspect_page(self.root, self.bridge, 0, 1)["captures"][0]["task_request_id"])
        with self.journal.db:
            self.journal.db.execute("UPDATE requests SET delivered_text='different private text' WHERE ord=1")
        changed = self.module.inspect_page(self.root, self.bridge, 0, 1)["captures"][0]
        self.assertNotEqual(before, changed["task_request_id"])
        self.assertEqual(changed["link_state"], "no_observation")

    def test_bounds_invalid_scope_and_malformed_trace_fail_closed_without_pause_or_replay(self):
        for after, limit in ((-1, 1), (True, 1), (2**63, 1), (0, 0), (0, 11), (0, True)):
            with self.assertRaises((CaptureError, ValueError)):
                self.module.inspect_page(self.root, self.bridge, after, limit)
        self.bridge.command.assert_not_called()
        self.bridge.command.side_effect = None
        for reply in (dict(), dict(event="learning_trace", correlation_id="a" * 32, matches=[]),
                      dict(event="learning_trace", correlation_id="a" * 32,
                           matches=[dict(RequestId="0" * 32, Experience=None)])):
            self.bridge.command.return_value = reply
            with self.assertRaises((CaptureError, ValueError)):
                self.module.inspect_page(self.root, self.bridge, 0, 1)
        self.assertTrue(all(call.args[0] == "trace" for call in self.bridge.command.call_args_list))
        with self.journal.db:
            self.journal.db.execute("UPDATE policy SET body='{}'")
        with self.assertRaises(CaptureError):
            self.module.inspect_page(self.root, self.bridge, 0, 1)

    def test_wal_capture_is_refused_before_sqlite_can_create_sidecars(self):
        self.assertEqual(self.journal.db.execute("PRAGMA journal_mode=WAL").fetchone(), ("wal",))
        self.journal.close()
        before = {path.name: path.read_bytes() for path in self.root.iterdir()}
        try:
            self.module.inspect_page(self.root, self.bridge, 0, 1)
        except (CaptureError, ValueError):
            pass
        else:
            self.fail("CAPTURE_INBOX_WAL_RED: read-only inspector accepted WAL and may create sidecars")
        self.assertEqual(before, {path.name: path.read_bytes() for path in self.root.iterdir()})
        self.bridge.command.assert_not_called()


if __name__ == "__main__":
    unittest.main()
