"""Synthetic protocol fixtures only; no network, live ledger or origin review."""
import json
import contextlib
import io
from pathlib import Path
import unittest
from unittest.mock import Mock, patch

import learning_bridge as lb
from unicode_queries import DATASETS, Reference

SOURCE = Path(__file__).resolve().parents[2] / "data/unicode17/UnicodeData-Latin1.txt"
IDENTITY = "b" * 32
BOOT = "00000000-0000-0000-0000-000000000001"


def experience(**changes):
    row = dict(Sequence=1, RequestId=IDENTITY, Dataset=DATASETS[0], Key=181, Origin="unreviewed",
               Boot=BOOT, StartedNanoseconds=1, FinishedBoot=BOOT, FinishedNanoseconds=2,
               State="verified", SourceSha256="a" * 64, Expected=924, Value=924,
               ApprovedSourceSha256=None, ApprovedExpected=None, ApprovedBoot=None,
               ApprovedNanoseconds=None, ReviewConflict=False)
    row.update(changes)
    return row


def response(**changes):
    reply = dict(event="learning_task", correlation_id="c" * 32,
                 proposal=dict(Status="ready", Code="typed_case_change", Dataset=DATASETS[0], Key=181, Prompt=None),
                 replayed=False, experience=experience())
    reply.update(changes)
    return reply


class TaskBridgeTests(unittest.TestCase):
    def bridge(self, reply):
        self.assertTrue(callable(getattr(lb.Bridge, "task_answer", None)), "TASK_BRIDGE_RED: missing validator")
        bridge = lb.Bridge.__new__(lb.Bridge)
        bridge.task_mode, bridge.failed = True, False
        bridge.source_pins = {name: "a" * 64 for name in DATASETS}
        bridge.reference = Reference(SOURCE)
        bridge.check_installation = Mock()
        bridge.require_owner = Mock()
        bridge.command = Mock(return_value=reply)
        return bridge

    def test_verified_task_checks_external_reference_and_uses_unreviewed_origin(self):
        bridge = self.bridge(response())
        text, status = bridge.task_answer("What's the uppercase of µ?", IDENTITY)
        self.assertEqual(status, "peer_ok")
        self.assertIn("924 (U+039C)", text)
        bridge.command.assert_called_once_with("task", "unreviewed", IDENTITY, "What's the uppercase of µ?")
        self.assertEqual(bridge.check_installation.call_count, 2)

    def test_task_logs_are_correlated_and_do_not_contain_private_text(self):
        bridge = self.bridge(response())
        log = io.StringIO()
        with contextlib.redirect_stdout(log):
            bridge.task_answer("private fixture question", IDENTITY)
        self.assertTrue(log.getvalue().startswith('{'), "TASK_LOG_RED: missing structured task event")
        record = json.loads(log.getvalue())
        self.assertEqual(record["request_id"], IDENTITY)
        self.assertEqual(record["event"], "captured_task_result")
        self.assertEqual(record["experience_state"], "verified")
        self.assertNotIn("private fixture", log.getvalue())
        bridge.command.side_effect = lb.BridgeError("private child error")
        log = io.StringIO()
        with contextlib.redirect_stdout(log):
            bridge.task_answer("private fixture question", IDENTITY)
        record = json.loads(log.getvalue())
        self.assertEqual(record["request_id"], IDENTITY)
        self.assertEqual(record["code"], "outcome_unknown")
        self.assertNotIn("private", log.getvalue())

    def test_miss_retains_observation_without_claiming_demand_or_returning_reference(self):
        bridge = self.bridge(response(experience=experience(State="miss", Value=None)))
        text, status = bridge.task_answer("uppercase µ", IDENTITY)
        self.assertEqual(status, "peer_ok")
        self.assertIn("owner approval", text)
        self.assertNotIn("924", text)
        bridge.command.assert_called_once()

    def test_every_parser_refusal_is_terminal_without_peer_or_legacy_lookup(self):
        for state, code in (("clarify", "specify_case_input"), ("abstain", "unsupported_intent"),
                            ("abstain", "input_bounds"), ("abstain", "input_domain"),
                            ("abstain", "dataset_not_authorized")):
            proposal = dict(Status=state, Code=code, Dataset=None, Key=None,
                            Prompt="Untrusted child prompt" if state == "clarify" else None)
            bridge = self.bridge(response(proposal=proposal, experience=None))
            text, status = bridge.task_answer("unsupported fixture", IDENTITY)
            self.assertEqual(status, "peer_ok")
            self.assertNotIn("Untrusted", text)
            bridge.command.assert_called_once()

    def test_ood_unknown_and_conflict_never_render_a_label(self):
        for state, key, expected, value, status in (("abstain", 223, None, None, "peer_ok"),
                ("unknown", 181, 924, None, "peer_unknown"), ("conflict", 181, 924, 923, "peer_error")):
            reply = response(experience=experience(State=state, Key=key, Expected=expected, Value=value))
            reply["proposal"]["Key"] = key
            bridge = self.bridge(reply)
            text, actual = bridge.task_answer("fixture", IDENTITY)
            self.assertEqual(actual, status)
            self.assertNotIn("923", text)
            self.assertNotIn("924", text)
            self.assertEqual(bridge.failed, state == "conflict")

    def test_malformed_task_or_experience_latches_and_pauses(self):
        for changes in (dict(RequestId="d" * 32), dict(Origin="synthetic"), dict(Key=True),
                        dict(Value=True), dict(Value=923), dict(Expected=923), dict(SourceSha256="0" * 64),
                        dict(Sequence=True), dict(ReviewConflict=True), dict(extra="injected"),
                        dict(ApprovedSourceSha256="a" * 64), dict(FinishedNanoseconds=-1),
                        dict(State="miss", Value=924), dict(State="pending", Value=None)):
            bridge = self.bridge(response(experience=experience(**changes)))
            text, status = bridge.task_answer("uppercase µ", IDENTITY)
            self.assertEqual(status, "peer_error", changes)
            self.assertTrue(bridge.failed, changes)
            self.assertEqual(bridge.command.call_args_list[-1].args, ("pause",))
            self.assertNotIn("924", text)
        for changes in (dict(event="wrong"), dict(correlation_id=True), dict(replayed=1), dict(proposal=None),
                        dict(experience=None), dict(extra=True)):
            bridge = self.bridge(response(**changes))
            self.assertEqual(bridge.task_answer("uppercase µ", IDENTITY)[1], "peer_error")
            self.assertTrue(bridge.failed)

    def test_replayed_answer_is_history_not_fresh_verification(self):
        bridge = self.bridge(response(replayed=True))
        text, status = bridge.task_answer("uppercase µ", IDENTITY)
        self.assertEqual(status, "peer_unknown")
        self.assertNotIn("924", text)
        self.assertIn("retained", text)

    def test_replayed_conflict_still_latches_and_pauses(self):
        for changes in (dict(State="conflict", Value=923), dict(ReviewConflict=True)):
            bridge = self.bridge(response(replayed=True, experience=experience(**changes)))
            self.assertEqual(bridge.task_answer("uppercase µ", IDENTITY)[1], "peer_error",
                             "TASK_REPLAY_CONFLICT_RED: historical conflict did not close the bridge")
            self.assertTrue(bridge.failed)
            self.assertEqual(bridge.command.call_args_list[-1].args, ("pause",))

    def test_pause_failure_remains_latched_and_pending_replay_does_not_execute_again(self):
        bridge = self.bridge(response(experience=experience(Value=923)))
        bridge.command.side_effect = [response(experience=experience(Value=923)), lb.BridgeError("private pause error")]
        log = io.StringIO()
        with contextlib.redirect_stdout(log):
            self.assertEqual(bridge.task_answer("private input", IDENTITY)[1], "peer_error")
        self.assertTrue(bridge.failed)
        self.assertEqual([json.loads(line)["event"] for line in log.getvalue().splitlines()],
                         ["captured_task_pause_failed", "captured_task_refused"])
        self.assertNotIn("private", log.getvalue())
        bridge = self.bridge(response(replayed=True, experience=experience(State="pending", Value=None,
                              FinishedBoot=None, FinishedNanoseconds=None)))
        self.assertEqual(bridge.task_answer("uppercase µ", IDENTITY)[1], "peer_unknown")
        bridge.command.assert_called_once()

    def test_timeout_source_failure_latch_and_bad_local_arguments_never_retry(self):
        bridge = self.bridge(response())
        bridge.command.side_effect = lb.BridgeError("command_timeout")
        self.assertEqual(bridge.task_answer("uppercase µ", IDENTITY)[1], "peer_unknown")
        bridge.command.assert_called_once()
        bridge.command.reset_mock()
        bridge.failed = True
        self.assertEqual(bridge.task_answer("uppercase µ", IDENTITY)[1], "peer_error")
        bridge.command.assert_not_called()
        bridge.failed = False
        bridge.check_installation.side_effect = lb.BridgeError("source_changed")
        self.assertEqual(bridge.task_answer("uppercase µ", IDENTITY)[1], "peer_error")
        bridge.command.assert_not_called()
        bridge.check_installation.side_effect = None
        for text, identity in (("bad\0input", IDENTITY), ("uppercase µ", "not-an-id")):
            self.assertEqual(bridge.task_answer(text, identity)[1], "peer_error")
        bridge.command.assert_not_called()

    def test_task_command_accepts_only_structured_unknown_exit_two_and_has_no_approval_authority(self):
        bridge = self.bridge(response())
        bridge.root, bridge.dotnet = Path("/fixture"), Path("/fixture/dotnet")
        bridge.command = lb.Bridge.command.__get__(bridge)
        reply = response(experience=experience(State="unknown", Value=None))
        with patch.object(lb, "bounded_call", return_value=(2, json.dumps(reply).encode(), b"")):
            self.assertEqual(bridge.task_answer("uppercase µ", IDENTITY)[1], "peer_unknown")
        for verb in ("ask", "lookup", "approve", "import", "resume", "tick", "run"):
            with patch.object(lb, "bounded_call") as run, self.assertRaises(lb.BridgeError):
                bridge.command(verb)
            run.assert_not_called()
        for code, raw, err in ((2, json.dumps(response()).encode(), b""), (0, json.dumps(reply).encode(), b""),
                               (2, b"{}", b"refused"), (1, b"{}", b"")):
            with patch.object(lb, "bounded_call", return_value=(code, raw, err)), self.assertRaises(lb.ProtocolError):
                bridge.command("task", "unreviewed", IDENTITY, "uppercase µ")


if __name__ == "__main__":
    unittest.main()
