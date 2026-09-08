"""Hermetic bridge protocol and process bounds; no Discord or live ledger."""
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import Mock, patch

import learning_bridge as lb
from unicode_queries import Reference

SOURCE = Path(__file__).resolve().parents[2] / "data/unicode17/UnicodeData-Latin1.txt"


class BridgeTests(unittest.TestCase):
    def bridge(self, response):
        bridge = lb.Bridge.__new__(lb.Bridge)
        bridge.reference = Reference(SOURCE)
        bridge.failed = False
        bridge.check_installation = Mock()
        bridge.require_owner = Mock()
        bridge.command = Mock(return_value=response)
        bridge.source_pins = {name: "a" * 64 for name in lb.DATASETS}
        return bridge

    def numeric(self, **changes):
        return dict(event="learning_answer", correlation_id="a" * 32, dataset="unicode17_upper_latin1",
                    key=181, verified=True, value=924, **changes)

    def test_verified_numeric_and_symbolic_answers_use_native_value_not_reference_fallback(self):
        bridge = self.bridge(self.numeric())
        text, status = bridge.answer("unicode upper 181")
        self.assertEqual(status, "peer_ok")
        self.assertIn("924", text)
        self.assertIn("U+039C", text)
        bridge.command.assert_called_once_with("ask", "unicode17_upper_latin1", "181")
        bridge.command.reset_mock()
        bridge.command.return_value = dict(event="learning_symbol_answer", correlation_id="b" * 32,
            dataset="ascii_category", token="DIGIT_ZERO", verified=True, label="Nd",
            source_sha256="a" * 64, symbol_vocabulary_sha256=bridge.reference.vocabulary_sha256)
        text, status = bridge.answer("unicode category DIGIT_ZERO")
        self.assertEqual(status, "peer_ok")
        self.assertIn("Nd", text)

    def test_miss_records_one_demand_and_never_returns_external_label_as_answer(self):
        response = self.numeric()
        response.update(verified=False, value=None)
        bridge = self.bridge(response)
        text, status = bridge.answer("unicode upper 181")
        self.assertEqual(status, "peer_ok")
        self.assertIn("request recorded", text)
        self.assertNotIn("924", text)
        bridge.command.assert_called_once()

    def test_real_symbolic_miss_protocol_has_null_label(self):
        bridge = self.bridge(None)
        bridge.command.return_value = dict(event="learning_symbol_answer", correlation_id="b" * 32,
            dataset="ascii_category", token="DIGIT_ZERO", verified=False, label=None,
            source_sha256="a" * 64, symbol_vocabulary_sha256=bridge.reference.vocabulary_sha256)
        text, status = bridge.answer("unicode category DIGIT_ZERO")
        self.assertEqual(status, "peer_ok")
        self.assertIn("request recorded", text)
        bridge.command.assert_called_once()

    def test_wrong_typed_identity_and_verified_outside_domain_latch_and_pause(self):
        for changes in ({"value": 923}, {"value": True}, {"key": 180}, {"key": True},
                        {"dataset": "other"}, {"verified": 1}, {"event": "learning_tick"},
                        {"extra": "injected"}, {"correlation_id": "bad"}):
            response = self.numeric()
            response.update(changes)
            bridge = self.bridge(response)
            text, status = bridge.answer("unicode upper 181")
            self.assertEqual(status, "peer_error", changes)
            self.assertTrue(bridge.failed)
            self.assertEqual(bridge.command.call_args_list[-1].args, ("pause",))
            self.assertNotIn("923", text)
            bridge.command.reset_mock()
            self.assertEqual(bridge.answer("unicode upper 181")[1], "peer_error")
            bridge.command.assert_not_called()

    def test_external_abstention_cannot_be_reported_as_native_verified(self):
        response = self.numeric()
        response.update(key=65, value=65)
        bridge = self.bridge(response)
        self.assertEqual(bridge.answer("unicode upper 65")[1], "peer_error")

    def test_unrelated_and_malformed_do_not_touch_ledger(self):
        bridge = self.bridge(self.numeric())
        self.assertIsNone(bridge.answer("what can you do"))
        for text in ("unicode upper 181\n", "unicode pause", "unicode upper 256"):
            self.assertEqual(bridge.answer(text)[1], "peer_error")
        bridge.check_installation.assert_not_called()
        bridge.command.assert_not_called()

    def test_timeout_has_unknown_outcome_no_retry_or_reference_answer(self):
        bridge = self.bridge(self.numeric())
        bridge.command.side_effect = lb.BridgeError("command_timeout")
        text, status = bridge.answer("unicode upper 181")
        self.assertEqual(status, "peer_unknown")
        bridge.command.assert_called_once()
        self.assertNotIn("924", text)

    def test_source_or_health_failure_prevents_demand(self):
        bridge = self.bridge(self.numeric())
        bridge.require_owner.side_effect = lb.BridgeError("owner_unavailable")
        self.assertEqual(bridge.answer("unicode upper 181")[1], "peer_error")
        bridge.command.assert_not_called()

    def test_json_duplicate_nonfinite_trailing_or_nonobject_refuses(self):
        for raw in (b'{"verified":true,"verified":false}', b'{"value":NaN}', b'{} {}', b'[]'):
            with self.assertRaises(lb.BridgeError):
                lb.decode(raw)

    def test_malformed_successful_child_output_latches_and_pauses_before_another_ask(self):
        bridge = self.bridge(self.numeric())
        bridge.root = Path("/fixture")
        bridge.dotnet = Path("/fixture/dotnet")
        bridge.command = lb.Bridge.command.__get__(bridge)
        with patch.object(lb, "bounded_call", return_value=(0, b'{"verified":true,"verified":false}', b"")) as run:
            self.assertEqual(bridge.answer("unicode upper 181")[1], "peer_error")
            self.assertTrue(bridge.failed)
            self.assertEqual([call.args[0][3] for call in run.call_args_list], ["ask", "pause"])
            run.reset_mock()
            bridge.answer("unicode upper 181")
            run.assert_not_called()

    def test_native_parser_rejection_and_other_failed_answers_latch_conservatively(self):
        bridge = self.bridge(self.numeric())
        bridge.root = Path("/fixture")
        bridge.dotnet = Path("/fixture/dotnet")
        bridge.command = lb.Bridge.command.__get__(bridge)
        error = b'{"event":"learning_refused","code":"learning_ask_observation_unknown"}\n'
        with patch.object(lb, "bounded_call", return_value=(2, b"", error)) as run:
            self.assertEqual(bridge.answer("unicode upper 181")[1], "peer_error")
            self.assertTrue(bridge.failed)
            self.assertEqual([call.args[0][3] for call in run.call_args_list], ["ask", "pause"])
            run.reset_mock()
            bridge.answer("unicode upper 181")
            run.assert_not_called()


class ProcessTests(unittest.TestCase):
    def test_clean_environment_and_bounded_regular_completion(self):
        with tempfile.TemporaryDirectory() as tmp:
            code = "import os; assert 'CNET_FIXTURE_SECRET' not in os.environ; print('{}')"
            with patch.dict("os.environ", {"CNET_FIXTURE_SECRET": "never inherit"}):
                self.assertEqual(lb.bounded_call([sys.executable, "-c", code], tmp, 2), (0, b"{}\n", b""))

    def test_timeout_and_each_pipe_overflow_are_bounded(self):
        for code in ("import time; time.sleep(5)", "import os; os.write(1,b'x'*40000)",
                     "import os; os.write(2,b'x'*40000)"):
            with tempfile.TemporaryDirectory() as tmp:
                started = time.monotonic()
                with self.assertRaises(lb.BridgeError):
                    lb.bounded_call([sys.executable, "-c", code], tmp, .3)
                self.assertLess(time.monotonic() - started, 2)


if __name__ == "__main__":
    unittest.main()
