"""Notification fixtures never contact a real desktop or external service."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import Mock


def notify(root, boot, seconds, send):
    from alert import notify as implementation
    return implementation(root, boot, lambda: seconds, send)


class AlertTests(unittest.TestCase):
    def test_unit_does_not_turn_throttling_into_failed_service(self):
        unit = (Path(__file__).parent / "systemd/cnet-capture-alert.service").read_text()
        self.assertIn("StartLimitIntervalSec=0", unit, "ALERT_START_LIMIT_RED")
        self.assertIn("/alert.py ", unit)

    def test_repeat_is_successful_suppression_then_resumes(self):
        with tempfile.TemporaryDirectory() as directory:
            send = Mock()
            self.assertEqual(notify(Path(directory), "boot", 100, send), "sent")
            for seconds in [101, 160, 399]:
                self.assertEqual(notify(Path(directory), "boot", seconds, send), "suppressed")
            self.assertEqual(notify(Path(directory), "boot", 400, send), "sent")
            self.assertEqual(send.call_count, 2)

    def test_delivery_failure_never_claims_success_or_suppresses_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            send = Mock(side_effect=subprocess.SubprocessError("fixture"))
            with self.assertRaises(subprocess.SubprocessError):
                notify(root, "boot", 100, send)
            self.assertEqual(notify(root, "boot", 101, Mock()), "sent")

    def test_reboot_and_clock_regression_cannot_suppress_forever(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(notify(root, "boot", 1000, Mock()), "sent")
            with self.assertRaises(ValueError):
                notify(root, "boot", 999, Mock())
            self.assertEqual(notify(root, "next_boot", 1, Mock()), "sent")

    def test_malformed_or_public_state_refuses(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "last_sent.json"
            for raw in ['garbage', '{"boot":"boot","seconds":NaN}',
                        '{"boot":"boot","seconds":true}',
                        '{"boot":"boot","boot":"other","seconds":1}']:
                path.write_text(raw)
                path.chmod(0o600)
                send = Mock()
                with self.assertRaises((ValueError, RuntimeError)):
                    notify(root, "boot", 200, send)
                send.assert_not_called()
            path.write_text(json.dumps(dict(boot="boot", seconds=100)))
            path.chmod(0o644)
            with self.assertRaises(RuntimeError):
                notify(root, "boot", 200, Mock())

    def test_concurrent_trigger_does_not_send_twice(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            nested = Mock()
            def send():
                self.assertEqual(notify(root, "boot", 100, nested), "busy")
            self.assertEqual(notify(root, "boot", 100, send), "sent")
            nested.assert_not_called()

    def test_slow_delivery_does_not_shorten_throttle_interval(self):
        from alert import notify as implementation
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.assertEqual(implementation(root, "boot", Mock(side_effect=[100, 107]), Mock()), "sent")
            for seconds in [400, 406]:
                self.assertEqual(notify(root, "boot", seconds, Mock()), "suppressed")
            self.assertEqual(notify(root, "boot", 407, Mock()), "sent")


if __name__ == "__main__":
    unittest.main()
