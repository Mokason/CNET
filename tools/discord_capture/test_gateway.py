"""Mock Discord/peer only: these fixtures must never reach the live journal."""
import contextlib
import io
import json
import subprocess
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch, Mock

import gateway
from journal import CaptureError, Journal, readiness
from test_journal import CHANNEL, OWNER, message


class GatewayTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.journal = Journal(self.root, OWNER, CHANNEL)
        self.addCleanup(self.journal.close)
        self.gw = gateway.Gateway("fixture-not-a-token", self.journal)
        self.env = patch.dict(os.environ, {"CNET_DISCORD_PREFIX": "", "CNET_DISCORD_CHANNELS": ""})
        self.env.start()
        self.addCleanup(self.env.stop)
        network = patch("socket.socket.connect", side_effect=AssertionError("fixture_network_forbidden"))
        network.start()
        self.addCleanup(network.stop)

    def test_pre_forward_commit_and_duplicate_suppression(self):
        def peer(author, text):
            self.assertEqual(readiness(self.root)["unfinished"], 1)
            return "private answer", "peer_ok"
        log = io.StringIO()
        with patch.object(gateway, "peer_ask", side_effect=peer) as ask, \
                patch.object(gateway, "api", return_value={"id": "777777777777777777"}), \
                patch.object(gateway, "stamp_last_origin"), contextlib.redirect_stdout(log):
            self.gw.handle_message(message())
            self.gw.handle_message(message())
        self.assertEqual(ask.call_count, 1)
        self.assertEqual(readiness(self.root)["unfinished"], 0)
        self.assertNotIn("private", log.getvalue())

    def test_owner_learning_is_captured_before_single_dispatch_and_no_peer_fallback(self):
        def learn(text):
            self.assertEqual(readiness(self.root)["unfinished"], 1)
            self.assertEqual(text, "unicode upper 181")
            return "ABSTAIN: request recorded", "peer_ok"
        learner = Mock()
        learner.answer.side_effect = learn
        self.gw.learner = learner
        data = dict(message(), content="unicode upper 181")
        with patch.object(gateway, "peer_ask") as peer, patch.object(gateway, "api", return_value={}), \
                patch.object(gateway, "stamp_last_origin"):
            self.gw.handle_message(data)
            self.gw.handle_message(data)
        learner.answer.assert_called_once()
        peer.assert_not_called()

    def test_learning_authority_never_leaks_outside_selected_owner_dm(self):
        for changes in (dict(channel_id="555555555555555555"), dict(author={"id": "555555555555555555"}),
                        dict(guild_id="555555555555555555"), dict(webhook_id="555555555555555555"),
                        dict(message_snapshots=[{}])):
            self.gw.learner = Mock()
            with patch.object(gateway, "peer_ask", return_value=("answer", "peer_ok")), \
                    patch.object(gateway, "api", return_value={}), patch.object(gateway, "stamp_last_origin"):
                self.gw.handle_message(dict(message(), content="unicode upper 181", **changes))
            self.gw.learner.answer.assert_not_called()

    def test_task_mode_dispatch_uses_fresh_capture_and_stable_identity_only_once(self):
        learner = Mock(task_mode=True)
        learner.answer.return_value = ("legacy", "peer_ok")
        learner.task_answer.return_value = ("ABSTAIN: captured observation", "peer_ok")
        self.gw.learner = learner
        data = dict(message(), content="What's the uppercase of µ?")
        def task(text, identity):
            self.assertEqual(readiness(self.root)["unfinished"], 1)
            self.assertEqual(text, data["content"])
            self.assertRegex(identity, r"^[a-f0-9]{32}$")
            return "ABSTAIN: captured observation", "peer_ok"
        learner.task_answer.side_effect = task
        with patch.object(gateway, "peer_ask") as peer, patch.object(gateway, "api", return_value={}), \
                patch.object(gateway, "stamp_last_origin"):
            self.gw.handle_message(data)
            self.gw.handle_message(data)
        self.assertEqual(learner.task_answer.call_count, 1, "CAPTURE_TASK_DISPATCH_RED: task route not used")
        learner.answer.assert_not_called()
        peer.assert_not_called()

    def test_task_mode_never_replays_crash_before_or_after_native_admission(self):
        self.gw.learner = Mock(task_mode=True)
        data = dict(message(), content="uppercase µ")
        self.assertTrue(self.journal.begin(data, data["content"]))
        with patch.object(gateway, "peer_ask") as peer, patch.object(gateway, "api") as api:
            self.gw.handle_message(data)  # Simulates crash after capture, before native.
        self.gw.learner.task_answer.assert_not_called()
        peer.assert_not_called()
        api.assert_not_called()
        self.gw.learner.task_answer.side_effect = RuntimeError("fixture_native_may_have_accepted")
        next_data = dict(data, id=str(int(data["id"]) + 1))
        with patch.object(gateway, "peer_ask") as peer, patch.object(gateway, "api") as api, \
                patch.object(gateway, "stamp_last_origin"):
            with self.assertRaises(RuntimeError):
                self.gw.handle_message(next_data)
            self.gw.handle_message(next_data)
        self.gw.learner.task_answer.assert_called_once()
        peer.assert_not_called()
        api.assert_not_called()

    def test_learning_preserves_nonspace_whitespace_for_strict_parser_and_capture(self):
        learner = Mock()
        learner.answer.return_value = ("ABSTAIN: malformed", "peer_error")
        self.gw.learner = learner
        for i, text in enumerate(("unicode upper 181\n", "unicode\tupper 181", "unicode upper 181\u00a0")):
            with patch.object(gateway, "peer_ask") as peer, patch.object(gateway, "api", return_value={}), \
                    patch.object(gateway, "stamp_last_origin"), patch.dict(os.environ, {"CNET_DISCORD_PREFIX": "!c"}):
                self.gw.handle_message(dict(message(), id=str(333333333333333334 + i), content="!c " + text))
            learner.answer.assert_called_with(text)
            peer.assert_not_called()
            delivered = self.journal.db.execute("SELECT delivered_text FROM requests ORDER BY ord DESC LIMIT 1").fetchone()[0]
            self.assertEqual(delivered, text)

    def test_learning_unknown_outcome_and_reply_failure_do_not_retry_or_fall_back(self):
        self.gw.learner = Mock()
        self.gw.learner.answer.return_value = ("ABSTAIN: outcome unknown", "peer_unknown")
        with patch.object(gateway, "peer_ask") as peer, patch.object(gateway, "api", side_effect=TimeoutError), \
                patch.object(gateway, "stamp_last_origin"):
            self.gw.handle_message(dict(message(), content="unicode upper 181"))
        self.gw.learner.answer.assert_called_once()
        peer.assert_not_called()
        self.assertEqual(self.journal.db.execute("SELECT peer_status,reply_status FROM requests").fetchone(),
                         ("peer_unknown", "reply_unknown"))

    def test_learning_config_is_opt_in_complete_and_requires_verified_capture_scope(self):
        with patch.dict(os.environ, {"CNET_DISCORD_LEARNING_ROOT": "", "CNET_DISCORD_LEARNING_PIN": ""}):
            self.assertIsNone(gateway.configured_learner(None))
        for root, pin, journal in (("/tmp/fixture", "", self.journal), ("", "a"*64, self.journal),
                                   ("/tmp/fixture", "a"*64, None)):
            with patch.dict(os.environ, {"CNET_DISCORD_LEARNING_ROOT": root, "CNET_DISCORD_LEARNING_PIN": pin}), \
                    self.assertRaises(CaptureError):
                gateway.configured_learner(journal)

    def test_fatal_capture_refusal_never_invokes_peer(self):
        with patch.object(self.journal, "begin", side_effect=CaptureError("fixture")), \
                patch.object(gateway, "peer_ask") as ask:
            with self.assertRaises(CaptureError):
                self.gw.handle_message(message())
        ask.assert_not_called()

    def test_other_account_serves_without_capture(self):
        data = message()
        data["author"] = {"id": "555555555555555555", "username": "fixture"}
        with patch.object(gateway, "peer_ask", return_value=("answer", "peer_ok")) as ask, \
                patch.object(gateway, "api", return_value={}), \
                patch.object(gateway, "stamp_last_origin"):
            self.gw.handle_message(data)
        ask.assert_called_once()
        self.assertEqual(readiness(self.root)["requests"], 0)

    def test_ambiguous_timeout_is_recorded_without_retry(self):
        with patch.object(gateway, "peer_ask", return_value=("timeout", "peer_unknown")) as ask, \
                patch.object(gateway, "api", side_effect=TimeoutError), \
                patch.object(gateway, "stamp_last_origin"):
            self.gw.handle_message(message())
        ask.assert_called_once()
        row = self.journal.db.execute("SELECT peer_status,reply_status FROM requests").fetchone()
        self.assertEqual(row, ("peer_unknown", "reply_unknown"))

    def test_runtime_scope_verifies_exact_individual_owner_dm(self):
        app = {"owner": {"id": OWNER}, "team": None}
        dm = {"id": CHANNEL, "type": 1, "recipients": [{"id": OWNER}]}
        with patch.object(gateway, "api", side_effect=[app, dm]):
            gateway.validate_capture_scope("fixture", OWNER, CHANNEL)
        dm["type"] = 0
        with patch.object(gateway, "api", side_effect=[app, dm]):
            with self.assertRaises(CaptureError):
                gateway.validate_capture_scope("fixture", OWNER, CHANNEL)

    def test_callback_refusal_closes_socket_and_latches_fatal(self):
        self.gw.ws = Mock()
        event = json.dumps({"op": 0, "t": "MESSAGE_CREATE", "s": 1, "d": message()})
        with patch.object(self.journal, "begin", side_effect=CaptureError("private payload")), \
                patch.object(gateway, "peer_ask") as ask, \
                contextlib.redirect_stdout(io.StringIO()) as log:
            self.gw.on_message(self.gw.ws, event)
            self.gw.on_message(self.gw.ws, event)
        self.assertTrue(self.gw.fatal)
        self.gw.ws.close.assert_called_once()
        ask.assert_not_called()
        self.assertNotIn("private", log.getvalue())

    def test_prefix_preserves_original_input_but_records_delivered_text(self):
        data = message()
        data["content"] = "  !c private question  "
        with patch.dict(os.environ, {"CNET_DISCORD_PREFIX": "!c"}), \
                patch.object(gateway, "peer_ask", return_value=("answer", "peer_ok")), \
                patch.object(gateway, "api", return_value={}), \
                patch.object(gateway, "stamp_last_origin"):
            self.gw.handle_message(data)
        self.assertEqual(self.journal.db.execute("SELECT raw_text,delivered_text FROM requests").fetchone(),
                         (data["content"], "private question"))

    def test_gateway_disables_redirects_and_rejects_nonupgrade(self):
        sock = Mock()
        sock.getstatus.return_value = 302
        with patch.object(gateway, "api", return_value={"url": "wss://gateway.discord.gg"}), \
                patch.object(gateway.websocket, "WebSocketApp"), \
                patch.object(gateway.websocket, "WebSocket", return_value=sock):
            with self.assertRaises(CaptureError):
                self.gw.run()
        self.assertEqual(sock.connect.call_args.kwargs["redirect_limit"], 0)
        sock.send.assert_not_called()
        sock.close.assert_called_once()

    def test_incomplete_policy_refuses_without_running_gateway(self):
        with patch.dict(os.environ, {"CNET_DISCORD_CAPTURE_DIR": str(self.root),
                                     "CNET_DISCORD_CAPTURE_OWNER": "",
                                     "CNET_DISCORD_CAPTURE_CHANNEL": ""}), \
                patch.object(gateway, "load_token", return_value="fixture"), \
                patch.object(gateway, "api", return_value={"bot": True}), \
                patch.object(gateway.Gateway, "run") as run:
            with self.assertRaises(CaptureError):
                gateway.main()
        run.assert_not_called()

    def test_actual_peer_adapter_timeout_invokes_process_only_once(self):
        with patch.object(gateway.subprocess, "run", side_effect=subprocess.TimeoutExpired("fixture", 120)) as run:
            answer, status = gateway.peer_ask("fixture", "private question")
        run.assert_called_once()
        self.assertEqual(status, "peer_unknown")
        self.assertNotIn("private", answer)

    def test_native_control_framing_and_truncation_inputs_never_invoke_peer(self):
        for query in ("QUIT", "PING", "STATUS", "--help", "line\nQUIT", "line\rQUIT", "a\0b", "x" * 8100):
            with patch.object(gateway.subprocess, "run") as run:
                answer, status = gateway.peer_ask("fixture", query)
            run.assert_not_called()
            self.assertEqual(status, "peer_error")
            self.assertEqual(answer, "(request cannot be forwarded safely)")

    def test_failed_completion_stays_unfinished_and_closes_gateway(self):
        self.gw.ws = Mock()
        event = json.dumps({"op": 0, "t": "MESSAGE_CREATE", "s": 1, "d": message()})
        with patch.object(self.journal, "finish", side_effect=CaptureError("fixture")), \
                patch.object(gateway, "peer_ask", return_value=("answer", "peer_ok")) as ask, \
                patch.object(gateway, "api", return_value={}), \
                patch.object(gateway, "stamp_last_origin"):
            self.gw.on_message(self.gw.ws, event)
            self.gw.on_message(self.gw.ws, event)
        ask.assert_called_once()
        self.gw.ws.close.assert_called_once()
        self.assertEqual(readiness(self.root)["unfinished"], 1)

    def test_rest_redirect_refused_and_read_is_bounded(self):
        self.assertIsNone(gateway.NoRedirect().redirect_request(None, None, 302, "", {}, "https://fixture.invalid"))
        response = Mock()
        response.read.return_value = b"x" * 262145
        manager = Mock()
        manager.__enter__ = Mock(return_value=response)
        manager.__exit__ = Mock(return_value=False)
        opener = Mock()
        opener.open.return_value = manager
        with patch.object(gateway.urllib.request, "build_opener", return_value=opener):
            with self.assertRaises(CaptureError):
                gateway.api("GET", "/gateway/bot", "fixture")
        response.read.assert_called_once_with(262145)

    def test_run_ready_and_disconnect_record_segment_without_demand(self):
        sock = Mock()
        sock.getstatus.return_value = 101
        sock.recv.side_effect = [json.dumps({"op": 0, "t": "READY", "s": 1,
                                          "d": {"user": {"id": "999999999999999999"}}}), ""]
        with patch.object(gateway, "api", return_value={"url": "wss://gateway.discord.gg"}), \
                patch.object(gateway.websocket, "WebSocket", return_value=sock):
            self.gw.run()
        self.assertEqual(readiness(self.root)["ready_segments"], 1)
        self.assertEqual(readiness(self.root)["requests"], 0)
        self.assertEqual(self.journal.db.execute("SELECT count(*) FROM events WHERE kind='gateway_close'").fetchone(), (1,))

    def test_full_mocked_connection_identifies_captures_and_closes(self):
        sock = Mock()
        sock.getstatus.return_value = 101
        frames = [{"op": 10, "d": {"heartbeat_interval": 41250}},
                  {"op": 0, "t": "READY", "s": 1, "d": {"user": {"id": "999999999999999999"}}},
                  {"op": 0, "t": "MESSAGE_CREATE", "s": 2, "d": message()}]
        sock.recv.side_effect = [json.dumps(frame) for frame in frames] + [""]
        with patch.object(gateway, "api", side_effect=[{"url": "wss://gateway.discord.gg"},
                                                      {"id": "777777777777777777"}]), \
                patch.object(gateway.websocket, "WebSocket", return_value=sock), \
                patch.object(gateway.threading, "Thread"), \
                patch.object(gateway, "peer_ask", return_value=("answer", "peer_ok")) as ask, \
                patch.object(gateway, "stamp_last_origin"):
            self.gw.run()
        ask.assert_called_once()
        self.assertEqual(json.loads(sock.send.call_args.args[0])["op"], 2)
        self.assertEqual(readiness(self.root)["requests"], 1)
        self.assertEqual(readiness(self.root)["unfinished"], 0)

    def test_ack_records_bounded_heartbeat_without_demand(self):
        self.gw.me_id = "999999999999999999"
        with patch.object(gateway.time, "monotonic", return_value=100):
            self.gw.dispatch(json.dumps({"op": 11, "d": None}))
            self.gw.dispatch(json.dumps({"op": 11, "d": None}))
        self.assertEqual(self.journal.db.execute("SELECT count(*) FROM events WHERE kind='gateway_heartbeat'").fetchone(), (1,))
        self.assertEqual(readiness(self.root)["requests"], 0)


if __name__ == "__main__":
    unittest.main()
