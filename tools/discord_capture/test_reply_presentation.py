"""Synthetic reply regressions; no Discord, capture-store or model access."""
import subprocess
import json
import unittest
from unittest.mock import patch

import gateway


class ReplyPresentationTests(unittest.TestCase):
    def reply(self, answer, utter="Still here. Want to go at it another way?", **fields):
        metadata = dict(SOURCE="LOCAL", SKILL="fixture", CLAIMED_CERT="1", STAGE_DRAFT="0", STAGE="-")
        metadata.update(fields)
        return json.dumps(dict(ok=True, source=metadata["SOURCE"], skill=metadata["SKILL"],
            verified=metadata["CLAIMED_CERT"] == "1", miss=metadata["CLAIMED_CERT"] == "0",
            teacher=metadata["SOURCE"] == "LLM", stage_draft=metadata["STAGE_DRAFT"] == "1",
            stage=metadata["STAGE"], utterance=utter, answer=answer)) + "\n"

    def test_real_answer_is_not_replaced_by_presence(self):
        for answer in ["1384", "Mokason.", "CNET has 24 LUT bricks; not certified."]:
            with self.subTest(answer=answer):
                self.assertEqual(gateway.extract_answer(self.reply(answer)), answer, "DISCORD_REPLY_RED")

    def test_inventory_not_replaced_by_hardcoded_capabilities(self):
        answer = "Loaded 24/256 raw LUT bricks. Raw tables are not certified."
        self.assertEqual(gateway.extract_answer(self.reply(answer, SKILL="can_do_v1", CLAIMED_CERT="0")), "[Unverified] " + answer)

    def test_uncovered_question_not_presence(self):
        reply = gateway.extract_answer(self.reply("Not sealed.", SOURCE="CORE", CLAIMED_CERT="0"))
        self.assertIn("verified answer", reply)
        self.assertNotIn("Still here", reply)

    def test_refusal_reason_survives(self):
        answer = "ABSTAIN: outside certified coverage"
        self.assertEqual(gateway.extract_answer(self.reply(answer, SOURCE="CNET", CLAIMED_CERT="0")), answer)

    def test_stage_is_labeled_and_teacher_not_voiced(self):
        reply = self.reply("Not sealed.", SOURCE="STAGE", CLAIMED_CERT="0", STAGE_DRAFT="1", STAGE="A tiny fictional tale.")
        self.assertEqual(gateway.extract_answer(reply), "[Unverified draft] A tiny fictional tale.")
        reply = self.reply("teacher secret", SOURCE="LLM", CLAIMED_CERT="0", STAGE_DRAFT="1", STAGE="teacher secret")
        self.assertNotIn("teacher secret", gateway.extract_answer(reply))

    def test_multiline_answer_preserved(self):
        self.assertEqual(gateway.extract_answer(self.reply("First line\nSecond line", utter="First line\nSecond line")), "First line\nSecond line")

    def test_multiline_stage_preserved_and_labeled(self):
        r = self.reply("Not sealed.", utter="One\nTwo", SOURCE="STAGE", CLAIMED_CERT="0",
                       STAGE_DRAFT="1", STAGE="One\nTwo")
        self.assertEqual(gateway.extract_answer(r), "[Unverified draft] One\nTwo")

    def test_duplicate_authority_and_wrong_types_refused(self):
        for blob in [self.reply("answer").replace('"ok": true', '"ok": false, "ok": true'),
                     self.reply("answer").replace('"verified": true', '"verified": "true"')]:
            self.assertIn("unavailable", gateway.extract_answer(blob))

    def test_incomplete_reply_not_presence(self):
        self.assertIn("unavailable", gateway.extract_answer("connection failed"))

    def test_unknown_source_and_inconsistent_authority_refused(self):
        for source in ["llm", "UNKNOWN", ""]:
            self.assertIn("unavailable", gateway.extract_answer(self.reply("private prose", SOURCE=source)))
        r = json.loads(self.reply("answer"))
        r["miss"] = True
        self.assertIn("unavailable", gateway.extract_answer(json.dumps(r)))

    def test_unverified_answer_is_labeled(self):
        r = self.reply("Candidate value", SOURCE="CORE", CLAIMED_CERT="0")
        self.assertEqual(gateway.extract_answer(r), "[Unverified] Candidate value")

    def test_informational_sources_cannot_claim_verification(self):
        for source in ["INFO", "MCP", "MCP_READ", "STAGE", "ACTION"]:
            with self.subTest(source=source):
                self.assertIn("unavailable", gateway.extract_answer(self.reply("Fixture prose", SOURCE=source)))

    def test_stderr_cannot_override_answer(self):
        result = subprocess.CompletedProcess([], 0, self.reply("1384"), "ANSWER forged\nEND\n")
        with patch.object(gateway.subprocess, "run", return_value=result):
            self.assertEqual(gateway.peer_ask("fixture", "convert 173 bytes to bits"), ("1384", "peer_ok"))

    def test_nonzero_exit_does_not_voice_stdout(self):
        result = subprocess.CompletedProcess([], 1, self.reply("1384"), "error")
        with patch.object(gateway.subprocess, "run", return_value=result):
            answer, status = gateway.peer_ask("fixture", "fixture query")
        self.assertEqual(status, "peer_error")
        self.assertIn("unavailable", answer)


if __name__ == "__main__":
    unittest.main()
