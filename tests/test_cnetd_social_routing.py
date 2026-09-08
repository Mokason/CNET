"""Private process fixture, no live configuration, teachers or Discord writes."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch


class SocialRoutingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cnet-social-")
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        (root / "ROUTES.jsonl").write_text('{"pattern":"fixture","pack":"fixture"}\n')
        pack = root / "pack_soul_marble"
        pack.mkdir()
        (pack / "catalog.jsonl").write_text("\n".join(json.dumps(row, separators=(",", ":")) for row in [
            dict(id="soul_who", intent="identity", pattern="Who are you", answer="I am Marble.", privilege=0),
            dict(id="SOUL_ACK", intent="presence", pattern="hello", answer="Still here.", privilege=0),
            dict(id="soul_operator", intent="identity", pattern="Who made you", answer="Fixture operator.", privilege=0),
            dict(id="fixture_fact", intent="fact", pattern="fixture fact", answer="Fixture answer.", privilege=0),
        ]) + "\n")
        self.sock = root / "front.sock"
        env = {"PATH": "/usr/bin:/bin", "LC_ALL": "C", "CNET_PACKS_ROOT": str(root),
               "CNET_SOCK": str(self.sock), "CNET_CORE_BUS_BRICKS_DIR": str(root / "empty"),
               "CNET_MCP_CLIENT": "0", "CNET_MARBLE_STAGE": "0"}
        self.process = subprocess.Popen([str(Path(os.environ.get("CNETD_BIN", "bin/cnetd")).absolute())],
                                        cwd=root, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(self.stop)
        deadline = time.monotonic() + 10
        while not self.sock.exists():
            self.assertIsNone(self.process.poll(), "daemon exited")
            self.assertLess(time.monotonic(), deadline)
            time.sleep(.02)

    def stop(self):
        self.process.terminate()
        try:
            self.process.wait(5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(5)

    def ask(self, q):
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(5)
            client.connect(str(self.sock))
            client.sendall((json.dumps({"q": q, "peer": "synthetic_social"}) + "\n").encode())
            with client.makefile("rb") as stream:
                return json.loads(stream.readline(65536))

    def test_identity_does_not_steal_other_requests(self):
        for q in ["Can you explain what a language model is?", "What are you doing tomorrow?",
                  "Who made you laugh?", "Tell me your name and explain diffraction"]:
            with self.subTest(q=q):
                r = self.ask(q)
                self.assertNotIn(r["skill"], ["soul_who", "soul_operator"], "SOCIAL_ROUTING_RED")
                self.assertFalse(r["verified"])

    def test_catalog_cannot_reintroduce_social_substring(self):
        for q in ["Who are you and explain diffraction", "Why is that star still here",
                  "hello explain diffraction",
                  "Explain the phrase who made you in French",
                  "Who are you" + " " * 510 + "and explain diffraction"]:
            with self.subTest(q=q[:60]):
                r = self.ask(q)
                self.assertTrue(r["miss"], "SOCIAL_CATALOG_RED")
                self.assertFalse(r["verified"])
        r = self.ask("fixture fact and who are you doing work with")
        self.assertIn("Fixture answer.", r["answer"])
        self.assertNotIn("I am Marble", r["answer"])

    def test_normalization_never_drops_question_tail(self):
        for base in ["who's your operator", "you're a language model"]:
            r = self.ask(base + " " * (510 - len(base)) + "x")
            self.assertFalse(r["verified"], "SOCIAL_EXPANSION_RED")
            self.assertNotIn(r["skill"], ["soul_who", "soul_operator"])
        r = self.ask("fixture fact" + " " * 510 + "and explain diffraction")
        self.assertFalse(r["verified"], "SOCIAL_PREPARATION_RED")
        self.assertIn("shorter", r["answer"])

    def test_identity_and_presence_remain(self):
        for q in ["Who are you?", "Please introduce yourself", "Are you a language model?"]:
            self.assertEqual(self.ask(q)["skill"], "soul_who")
        self.assertIn("here", self.ask("Are you still here?")["answer"])

    def test_json_exposes_escaped_peer_and_draft_metadata(self):
        r = self.ask("Who are you?")
        self.assertEqual(r.get("peer"), "synthetic_social", "SOCIAL_JSON_RED")
        self.assertIs(r.get("stage_draft"), False)
        self.assertEqual(r.get("stage"), "")

    def test_real_peer_to_discord_renderer(self):
        sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools/discord_capture"))
        try:
            import gateway
            binary = Path(os.environ.get("CNET_PEER_BIN", "bin/cnet_peer")).absolute()
            with patch.dict(os.environ, {"CNET_SOCK": str(self.sock), "CNET_PEER_BIN": str(binary)}):
                for q, expected in [("Who are you?", "I am Marble"),
                                    ("Can you learn?", "independent verification"),
                                    ("Can you use those skills?", "convert N INPUT_TAG to OUTPUT_TAG"),
                                    ("Explain quasicrystal diffraction", "verified answer")]:
                    answer, status = gateway.peer_ask("synthetic_social", q)
                    self.assertEqual(status, "peer_ok")
                    self.assertIn(expected, answer, "SOCIAL_DISCORD_WIRE_RED")
        finally:
            sys.path.pop(0)

    def test_capability_followups_give_noncertified_help(self):
        for q in ["Can you use those skills?", "What are you able to calculate?"]:
            r = self.ask(q)
            self.assertEqual(r["source"], "ACTION", "SOCIAL_ROUTING_RED")
            self.assertIn("convert N INPUT_TAG to OUTPUT_TAG", r["answer"])
            self.assertFalse(r["verified"])

    def test_learning_question_describes_limits_without_launching_job(self):
        r = self.ask("Can you learn?")
        self.assertEqual(r["skill"], "learning_help_v1", "SOCIAL_ROUTING_RED")
        self.assertIn("independent", r["answer"])
        self.assertFalse(r["verified"])
        self.assertFalse(r["teacher"])

    def test_improvement_question_does_not_invent_progress(self):
        r = self.ask("Any improvements?")
        self.assertEqual(r["skill"], "improvement_status_v1", "SOCIAL_ROUTING_RED")
        self.assertIn("cannot confirm", r["answer"])
        self.assertFalse(r["verified"])

    def test_unknown_gives_honest_next_step(self):
        r = self.ask("Explain quasicrystal diffraction")
        self.assertTrue(r["miss"])
        self.assertFalse(r["verified"])
        self.assertIn("verified answer", r["utterance"], "SOCIAL_ROUTING_RED")
        self.assertNotIn("Still here", r["utterance"])


if __name__ == "__main__":
    unittest.main()
