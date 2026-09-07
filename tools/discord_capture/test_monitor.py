"""Health/elapsed-time fixtures are not real observation acceptance."""
import tempfile
from pathlib import Path
import unittest

from monitor import assess, record_observation


def sample(**changes):
    values = dict(connected=True, heartbeat_age=20, pending_age=0,
                  db_bytes=20480, requests=0, events=2, free_bytes=1024**3,
                  recent_starts=1, legacy_private=True)
    values.update(segment="one", last_gap=1, policy_sha256="fixture")
    values.update(changes)
    return values


class MonitorTests(unittest.TestCase):
    def test_idle_is_healthy_and_thresholds_are_actionable(self):
        self.assertEqual(assess(sample()), [])
        for change, expected in [(dict(connected=False), "bridge_down"),
                                 (dict(heartbeat_age=181), "heartbeat_stale"),
                                 (dict(pending_age=181), "request_stuck"),
                                 (dict(db_bytes=60*1024**2), "storage_near_limit"),
                                 (dict(events=80001), "storage_near_limit"),
                                 (dict(free_bytes=1), "disk_reserve_low"),
                                 (dict(recent_starts=4), "reconnect_burst"),
                                 (dict(legacy_private=False), "legacy_privacy")]:
            self.assertIn(expected, assess(sample(**change)))

    def test_missing_heartbeat_is_never_healthy(self):
        self.assertIn("heartbeat_stale", assess(sample(heartbeat_age=None)))

    def test_missed_minute_cannot_be_added_to_healthy_time(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            record_observation(root, sample(), "boot", 100)
            row = record_observation(root, sample(), "boot", 220)
            self.assertIn("observation_gap", row["alerts"])
            self.assertEqual(row["healthy_seconds"], 0)

    def test_elapsed_observation_cannot_skip_time_or_claim_product_acceptance(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = record_observation(root, sample(), "boot", 100)
            self.assertEqual(first["healthy_seconds"], 0)
            next_tick = record_observation(root, sample(), "boot", 160)
            self.assertEqual(next_tick["healthy_seconds"], 60)
            skipped = record_observation(root, sample(), "boot", 1000000)
            self.assertEqual(skipped["healthy_seconds"], 0)
            self.assertIn("observation_gap", skipped["alerts"])
            self.assertFalse(skipped["product_acceptance"])

    def test_reboot_fault_and_clock_regression_break_continuity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            record_observation(root, sample(), "boot", 100)
            result = record_observation(root, sample(), "other_boot", 160)
            self.assertIn("observation_gap", result["alerts"])
            result = record_observation(root, sample(), "other_boot", 150)
            self.assertIn("observation_gap", result["alerts"])

    def test_corrupt_state_refuses_instead_of_resetting_observation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "latest.json").write_text("bad")
            (root / "latest.json").chmod(0o600)
            with self.assertRaises(ValueError):
                record_observation(root, sample(), "boot", 100)

    def test_reconnect_or_intervening_gap_breaks_observation(self):
        for changes in (dict(segment="two"), dict(last_gap=5), dict(policy_sha256="changed")):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                record_observation(root, sample(), "boot", 100)
                result = record_observation(root, sample(**changes), "boot", 160)
                self.assertIn("capture_discontinuity", result["alerts"])
                self.assertEqual(result["healthy_seconds"], 0)

    def test_sampling_failure_is_durable_and_cannot_count_healthy(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            record_observation(root, sample(), "boot", 100)
            failed = record_observation(root, {"sample_failed": True}, "boot", 160)
            self.assertIn("monitor_refused", failed["alerts"])
            result = record_observation(root, sample(), "boot", 220)
            self.assertEqual(result["healthy_seconds"], 0)

    def test_abandoned_stage_does_not_disable_future_observations(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "next.json").write_text("abandoned fixture")
            result = record_observation(root, sample(), "boot", 100)
            self.assertEqual(result["healthy_seconds"], 0)

    def test_independent_watchdog_rejects_stale_or_previous_boot_state(self):
        from monitor import check_observer
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            record_observation(root, sample(), "boot", 100)
            check_observer(root, "boot", 160)
            for boot, seconds in (("boot", 221), ("new_boot", 110), ("boot", 90)):
                with self.assertRaises(ValueError):
                    check_observer(root, boot, seconds)


if __name__ == "__main__":
    unittest.main()
