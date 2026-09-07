"""Contract fixtures only: never evidence of genuine request demand."""
import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import allocator_chronology_audit as audit


class ChronologyAuditTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "events.jsonl"

    def scan(self, data):
        self.path.write_bytes(data)
        return audit.inspect(self.path)

    def test_valid_legacy_rows_never_become_training_authority(self):
        raw = b'{"ts":"2026-09-07T00:00:00Z","query":"private","answer":"secret","source":"ASK_USER"}\n'
        r = self.scan(raw)
        self.assertEqual(r["sha256"], hashlib.sha256(raw).hexdigest())
        self.assertEqual(r["objects"], 1)
        self.assertEqual(r["caller_origin_rows"], 0)
        self.assertFalse(r["training_eligible"])
        self.assertNotIn("private", json.dumps(r))
        self.assertNotIn("secret", json.dumps(r))

    def test_invalid_duplicate_nonfinite_and_nonobjects_counted(self):
        r = self.scan(b'{bad}\n{"ts":1,"ts":2}\n{"x":NaN}\n[]\n')
        self.assertEqual(r["invalid_json"], 3)
        self.assertEqual(r["nonobjects"], 1)
        self.assertEqual(r["lines"], 4)

    def test_partial_final_line_is_not_an_event(self):
        r = self.scan(b'{"ts":"2026-09-07T00:00:00Z"}')
        self.assertEqual(r["unterminated_lines"], 1)
        self.assertEqual(r["objects"], 0)

    def test_time_order_and_invalid_calendar(self):
        r = self.scan(b'{"ts":"2026-09-07T00:00:02Z"}\n'
                      b'{"ts":"2026-09-07T00:00:01Z"}\n'
                      b'{"ts":"2026-02-30T00:00:00Z"}\n')
        self.assertEqual(r["timestamp_regressions"], 1)
        self.assertEqual(r["invalid_timestamps"], 1)
        self.assertEqual(r["min_timestamp"], "2026-09-07T00:00:01Z")

    def test_line_limit_does_not_split_one_record_into_many(self):
        with patch.object(audit, "MAX_LINE", 64):
            r = self.scan(b'x' * 150 + b'\n{}\n')
        self.assertEqual(r["oversized_lines"], 1)
        self.assertEqual(r["lines"], 2)
        self.assertEqual(r["objects"], 1)

    def test_total_size_bound(self):
        self.path.write_bytes(b'{}\n' * 10)
        with patch.object(audit, "MAX_BYTES", 8):
            with self.assertRaisesRegex(ValueError, "source_size"):
                audit.inspect(self.path)

    def test_symlink_and_special_file_refuse_without_blocking(self):
        self.path.symlink_to(Path(self.tmp.name) / "missing")
        with self.assertRaises(ValueError):
            audit.inspect(self.path)
        fifo = Path(self.tmp.name) / "fifo"
        os.mkfifo(fifo)
        with self.assertRaisesRegex(ValueError, "source_type"):
            audit.inspect(fifo)

    def test_empty_and_invalid_utf8(self):
        self.assertEqual(self.scan(b'')["lines"], 0)
        self.assertEqual(self.scan(b'{"x":"\xff"}\n')["invalid_json"], 1)

    def test_deadline_is_enforced(self):
        self.path.write_bytes(b'{}\n')
        with patch.object(audit, "now", side_effect=[0, 11]):
            with self.assertRaisesRegex(ValueError, "deadline"):
                audit.inspect(self.path)

    def test_response_metadata_does_not_identify_caller(self):
        r = self.scan(b'{"source":"USER","verified":true,"answer":"x"}\n')
        self.assertEqual(r["caller_origin_rows"], 0)
        self.assertEqual(r["answer_rows"], 1)
        self.assertFalse(r["training_eligible"])

    def test_content_never_changes(self):
        raw = b'{"ts":"2026-09-07T00:00:00Z"}\n'
        self.scan(raw)
        self.assertEqual(self.path.read_bytes(), raw)

    def test_changed_source_refuses(self):
        self.path.write_bytes(b'{}\n')
        calls = 0

        def replace_on_read():
            nonlocal calls
            calls += 1
            if calls == 2:
                self.path.write_bytes(b'[]\n')
            return 0

        with patch.object(audit, "now", side_effect=replace_on_read):
            with self.assertRaisesRegex(ValueError, "source_changed"):
                audit.inspect(self.path)

    def test_json_float_overflow_and_deep_nesting_refuse(self):
        raw = b'{"x":1e9999}\n' + b'[' * 2000 + b']' * 2000 + b'\n'
        self.assertEqual(self.scan(raw)["invalid_json"], 2)

    def test_nesting_bound_is_explicit_and_ignores_quoted_brackets(self):
        raw = b'[' * 65 + b']' * 65 + b'\n'
        self.assertEqual(self.scan(raw)["invalid_json"], 1)
        self.assertEqual(self.scan(b'[' * 64 + b']' * 64 + b'\n')["nonobjects"], 1)
        self.assertEqual(self.scan(b'{"query":"' + b'[' * 100 + b'\\\"}"}\n')["objects"], 1)

    def test_boottime_failure_has_no_fallback(self):
        with patch.object(audit.time, "clock_gettime", side_effect=OSError("clock failed")):
            with self.assertRaisesRegex(ValueError, "boottime_unavailable"):
                audit.now()

    def test_readable_virtual_file_is_not_an_empty_snapshot(self):
        with self.assertRaisesRegex(ValueError, "source_changed"):
            audit.inspect("/proc/self/status")


if __name__ == "__main__":
    unittest.main()
