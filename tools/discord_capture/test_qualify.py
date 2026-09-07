"""Development qualification fixtures, not real demand or improvement evidence."""
import unittest
import hashlib
import json
from unittest.mock import patch

from qualify import compare_direct, qualify
import test_dataset


class QualificationTests(unittest.TestCase):
    setUp = test_dataset.DatasetTests.setUp
    def test_empty_data_never_authorizes_fitting(self):
        from dataset import export_dataset
        target = self.root / "export"
        export_dataset(self.capture, target, self.mapper)
        report = qualify(target, self.mapper)
        self.assertEqual(report["eligible_episodes"], 0)
        self.assertIn("no_eligible_episodes", report["withheld_reasons"])
        self.assertFalse(report["amd_fitting_eligible"])

    def test_reviewed_windows_remain_whole_development_and_tampering_refuses(self):
        from dataset import export_dataset
        rows = [dict(ord=i, segment="fixture", received_ns=w*1800*10**9,
                     completed_ns=(w*1800+1)*10**9, delivered_text="convert 3 bytes to bits")
                for i, w in enumerate((2, 4, 6), 1)]
        events = [dict(segment="fixture", at_ns=t*10**9, kind="gateway_heartbeat")
                  for t in range(3540, 12661, 60)]
        with patch("dataset.snapshot", return_value=("{}", rows, events)), patch("dataset.time.time_ns", return_value=13000*10**9):
            initial = self.root / "initial"
            export_dataset(self.capture, initial, self.mapper)
            episodes = json.loads((initial / "episodes.json").read_bytes())
            target = self.root / "reviewed"
            export_dataset(self.capture, target, self.mapper,
                           {ep["episode_sha256"]: "human" for ep in episodes})
        report = qualify(target, self.mapper)
        self.assertEqual(report["eligible_episodes"], 3)
        self.assertEqual(report["splits"], dict(development_fit=[2, 4], development_validation=[6], confirmation=[]))
        self.assertFalse(report["amd_fitting_eligible"])
        for filename, mutate in (("episodes.json", lambda value: value[0].update(split="confirmation")),
                                 ("evidence.jsonl", lambda value: value[0].update(expected=25))):
            path = target / filename
            original = path.read_bytes()
            values = [json.loads(line) for line in original.splitlines()] if filename.endswith("jsonl") else json.loads(original)
            mutate(values)
            raw = ("\n".join(json.dumps(v) for v in values) + "\n").encode() if filename.endswith("jsonl") else json.dumps(values).encode()
            path.write_bytes(raw)
            manifest_path = target / "manifest.json"
            manifest = json.loads(manifest_path.read_bytes())
            manifest["files"][filename] = hashlib.sha256(raw).hexdigest()
            manifest_path.write_text(json.dumps(manifest))
            with self.assertRaises(ValueError):
                qualify(target, self.mapper)
            path.write_bytes(original)
            manifest["files"][filename] = hashlib.sha256(original).hexdigest()
            manifest_path.write_text(json.dumps(manifest))


class ControlsTests(unittest.TestCase):
    def test_equal_budgets_and_proven_direct_bound(self):
        labels = [dict(status="verified_tool", input_tag="u8", output_tag="masked8", key=250)] * 20
        report = compare_direct(labels)
        self.assertEqual(report["scope"], "prospective_direct_catalog_only")
        self.assertFalse(report["native_outcomes_measured"])
        plans = {row["policy"]: row for row in report["controls"]}
        self.assertEqual(plans["cursor"]["value"], 0)
        self.assertEqual(plans["exact"]["value"], 20)
        self.assertEqual(plans["exact"]["upper_bound"], 20)
        self.assertEqual(report["headroom_upper_vs_strongest"], 0)
        for row in plans.values():
            self.assertLessEqual(len(row["actions"]), 8)
            self.assertLessEqual(row["work"], 256)

    def test_unmapped_or_oversized_episode_refuses(self):
        with self.assertRaises(ValueError):
            compare_direct([dict(status="unmapped")])
        with self.assertRaises(ValueError):
            compare_direct([{}] * 2049)


if __name__ == "__main__":
    unittest.main()
