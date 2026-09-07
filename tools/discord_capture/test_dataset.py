"""Snapshot/split fixtures only; not independent demand evidence."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from journal import Journal
from evidence import Mapper, digest
from dataset import export_dataset, validate_export, build_episodes
from test_journal import OWNER, CHANNEL, message


class DatasetTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.capture = self.root / "capture"
        self.capture.mkdir(mode=0o700)
        self.j = Journal(self.capture, OWNER, CHANNEL)
        self.addCleanup(self.j.close)
        self.mapper = Mapper(lambda text: "capsule bytes bits 3", lambda *args: b"24\n", {})

    def test_immutable_export_preserves_unknown_origin_and_no_confirmation(self):
        self.j.begin(message(), "convert 3 bytes to bits")
        self.j.finish(message()["id"], "peer_ok", "reply_sent")
        target = self.root / "export"
        report = export_dataset(self.capture, target, self.mapper)
        self.assertEqual(report["requests"], 1)
        self.assertEqual(report["verified_labels"], 1)
        self.assertEqual(report["eligible_episodes"], 0)
        self.assertFalse(report["training_eligible"])
        self.assertEqual(validate_export(target)["purpose"], "development_only")
        with self.assertRaises(FileExistsError):
            export_dataset(self.capture, target, self.mapper)

    def test_tampered_export_refuses(self):
        target = self.root / "export"
        export_dataset(self.capture, target, self.mapper)
        (target / "requests.jsonl").write_bytes(b"{}\n")
        with self.assertRaises(ValueError):
            validate_export(target)

    def test_manifest_cannot_self_authorize_training_or_confirmation(self):
        target = self.root / "export"
        export_dataset(self.capture, target, self.mapper)
        path = target / "manifest.json"
        original = json.loads(path.read_bytes())
        for field in ("training_eligible", "confirmation_eligible"):
            changed = dict(original, **{field: True})
            path.write_text(json.dumps(changed))
            with self.assertRaises(ValueError):
                validate_export(target)

    def test_expired_final_label_cannot_publish(self):
        self.j.begin(message(), "input")
        target = self.root / "export"
        clock = [0]
        original = self.mapper.label
        def late(*args):
            result = original(*args)
            clock[0] = 31
            return result
        with patch("dataset.now_boot", side_effect=lambda: clock[0]), patch.object(self.mapper, "label", side_effect=late):
            with self.assertRaises(ValueError):
                export_dataset(self.capture, target, self.mapper)
        self.assertFalse((target / "manifest.json").exists())

    def test_manifest_is_included_in_total_byte_budget(self):
        target = self.root / "export"
        with patch("dataset.MAX_BYTES", 400):
            with self.assertRaises(ValueError):
                export_dataset(self.capture, target, self.mapper)
        self.assertFalse((target / "manifest.json").exists())

    def test_incomplete_publication_is_invalid(self):
        target = self.root / "export"
        with patch("dataset.write_private", side_effect=OSError("fixture")):
            with self.assertRaises(OSError):
                export_dataset(self.capture, target, self.mapper)
        with self.assertRaises((OSError, ValueError)):
            validate_export(target)

    def test_exports_cannot_publish_inside_a_repository(self):
        (self.root / ".git").write_text("gitdir: fixture")
        with self.assertRaises(ValueError):
            export_dataset(self.capture, self.root / "export", self.mapper)
        self.assertFalse((self.root / "export").exists())

    def episode(self, **changes):
        row = dict(ord=1, segment="segment", received_ns=3600 * 10**9,
                   boot_ns=100, completed_ns=3601 * 10**9, id="fixture")
        row.update(changes)
        return row

    def test_whole_windows_never_random_request_splits(self):
        rows = [self.episode(), self.episode(ord=2, received_ns=3610 * 10**9)]
        labels = [dict(status="verified_tool")] * 2
        episodes = build_episodes(rows, labels, [], 6000 * 10**9)
        self.assertEqual(len(episodes), 1)
        self.assertEqual(episodes[0]["requests"], 2)
        self.assertIn("origin_unreviewed", episodes[0]["excluded"])

    def test_gap_reconnect_pending_and_unmapped_exclude_whole_window(self):
        row = self.episode(completed_ns=None)
        events = [dict(segment="segment", at_ns=3600 * 10**9, kind="sequence_gap")]
        ep = build_episodes([row], [dict(status="unmapped")], events, 6000 * 10**9)[0]
        for reason in ("continuity_gap", "unfinished", "unmapped", "origin_unreviewed"):
            self.assertIn(reason, ep["excluded"])

    def test_open_window_never_scored(self):
        ep = build_episodes([self.episode()], [dict(status="verified_tool")], [], 3700 * 10**9)[0]
        self.assertIn("window_open", ep["excluded"])

    def test_review_binds_exact_episode_contents_and_test_stays_excluded(self):
        rows = [self.episode()]
        labels = [dict(status="verified_tool")]
        events = [dict(segment="segment", at_ns=t * 10**9, kind="gateway_heartbeat") for t in range(3540, 5461, 60)]
        first = build_episodes(rows, labels, events, 6000 * 10**9)[0]
        reviews = {first["episode_sha256"]: "human"}
        self.assertEqual(build_episodes(rows, labels, events, 6000 * 10**9, reviews)[0]["excluded"], [])
        rows[0]["id"] = "different"
        self.assertIn("origin_unreviewed", build_episodes(rows, labels, [], 6000 * 10**9, reviews)[0]["excluded"])
        reviews = {first["episode_sha256"]: "test"}
        rows[0]["id"] = "fixture"
        self.assertIn("origin_test", build_episodes(rows, labels, [], 6000 * 10**9, reviews)[0]["excluded"])


if __name__ == "__main__":
    unittest.main()
