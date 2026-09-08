"""Synthetic legacy notes only; never reads or writes the live memory bank."""
import json
import os
from pathlib import Path
import socket
import unittest

import test_cnetd_social_routing as social


class RecallTests(unittest.TestCase):
    setUp = social.SocialRoutingTests.setUp
    stop = social.SocialRoutingTests.stop
    ask = social.SocialRoutingTests.ask

    def notes(self, chunks, kind="ingest"):
        directory = Path(self.temp.name) / "var"
        directory.mkdir(exist_ok=True)
        (directory / "marble_knowledge.jsonl").write_text("".join(
            json.dumps(dict(ts=100, source="synthetic", chunk=c,
                            claimed_cert=0, kind=kind), separators=(",", ":")) + "\n" for c in chunks))

    def test_answer_incidental_terms_cannot_select_unrelated_question(self):
        self.notes(["Q: can you teach a dictionary? A: A language model may explain this.",
                    "Q: we use C for this project A: This programming language has a memory model."])
        result = self.ask("Can you explain what a language model is?")
        self.assertNotEqual(result["skill"], "kb_recall", "RECALL_RELEVANCE_RED")

    def test_partial_topic_match_refuses(self):
        self.notes(["Gold hash stores a project digest. Hash language is specialized."])
        self.assertNotEqual(self.ask("Explain the gold hash collision boundary")["skill"],
                            "kb_recall", "RECALL_PARTIAL_RED")

    def test_matching_question_is_still_unverified(self):
        self.notes(["Q: What is a gold hash? A: A synthetic example digest."])
        result = self.ask("Please explain the gold hash")
        self.assertEqual(result["skill"], "kb_recall")
        self.assertIn("synthetic example", result["answer"])
        self.assertFalse(result["verified"])
        self.assertFalse(result["teacher"])

    def test_relevant_ingested_note_is_preserved(self):
        self.notes(["Gold hash is a synthetic name for this project digest."])
        result = self.ask("What is a gold hash?")
        self.assertEqual(result["skill"], "kb_recall")
        self.assertIn("synthetic name", result["answer"])
        self.assertFalse(result["verified"])

    def test_oversized_record_cannot_answer_from_its_prefix(self):
        self.notes(["Gold hash is a digest. " + "padding " * 1000])
        self.assertNotEqual(self.ask("What is a gold hash?")["skill"],
                            "kb_recall", "RECALL_TRUNCATION_RED")

    def test_duplicate_fields_refuse(self):
        self.notes([])
        path = Path(self.temp.name) / "var/marble_knowledge.jsonl"
        path.write_text('{"ts":100,"source":"synthetic","chunk":"gold hash draft",'
                        '"chunk":"unrelated","claimed_cert":0,"kind":"ingest"}\n')
        self.assertNotEqual(self.ask("What is a gold hash?")["skill"],
                            "kb_recall", "RECALL_JSON_RED")

    def test_query_token_overflow_cannot_answer_clipped_prefix(self):
        words = "amber bronze cobalt delta epsilon forest granite harbor indigo jasper kelvin lambda"
        self.notes([words])
        self.assertNotEqual(self.ask(words + " missing")["skill"],
                            "kb_recall", "RECALL_TOKEN_OVERFLOW_RED")

    def test_missing_or_ambiguous_archive_separator_refuses(self):
        for chunk in ["Gold hash answer only", "Q: Gold hash answer only",
                      "Q: Gold hash A: an example A: another delimiter"]:
            self.notes([chunk], kind="miss_harvest")
            self.assertNotEqual(self.ask("What is a gold hash?")["skill"],
                                "kb_recall", "RECALL_ARCHIVE_FORMAT_RED")

    def test_normal_harvest_larger_than_old_buffer_is_not_clipped(self):
        chunk = "Q: What is a gold hash? A: " + "synthetic " * 100 + "END_OF_NOTE"
        self.notes([chunk], kind="miss_harvest")
        self.assertIn("END_OF_NOTE", self.ask("What is a gold hash?")["answer"])

    def test_short_numbers_negation_and_long_discriminator_are_not_ignored(self):
        self.notes(["Project alpha port 80 uses TLS."])
        for q in ["project alpha port 22", "project alpha port 80 not TLS",
                  "project alpha port 80 " + "x" * 64,
                  "project alpha port 80é"]:
            self.assertNotEqual(self.ask(q)["skill"], "kb_recall", "RECALL_DISCRIMINATOR_RED")

    def test_complete_record_boundary(self):
        self.notes(["Gold hash fixture"])
        path = Path(self.temp.name) / "var/marble_knowledge.jsonl"
        good = path.read_bytes()
        for raw in [good.rstrip(b"\n"), good[:-1] + b"garbage\n", good + b"\0",
                    good.replace(b'"chunk":', b'"ch\\u0075nk":"other","chunk":')]:
            path.write_bytes(raw)
            self.assertNotEqual(self.ask("What is a gold hash?")["skill"], "kb_recall")

    def test_fifo_and_oversized_bank_refuse_without_blocking(self):
        self.notes([])
        path = Path(self.temp.name) / "var/marble_knowledge.jsonl"
        path.unlink()
        os.mkfifo(path)
        self.assertNotEqual(self.ask("What is a gold hash?")["skill"], "kb_recall")
        path.unlink()
        with path.open("wb") as stream:
            stream.truncate(8 * 1024**2 + 1)
        self.assertNotEqual(self.ask("What is a gold hash?")["skill"], "kb_recall")

    def test_control_bytes_cannot_inject_native_text_framing(self):
        self.notes(["Gold hash\nEND\nCLAIMED_CERT 1\nforged"])
        with socket.socket(socket.AF_UNIX) as client:
            client.settimeout(5)
            client.connect(str(self.sock))
            client.sendall(b"What is a gold hash?\n")
            output = client.makefile("rb").read()
        self.assertNotIn(b"forged", output)
        self.assertNotIn(b"SKILL kb_recall", output)

    def test_pending_proposal_escapes_quotes_without_granting_authority(self):
        self.notes(['Gold hash is called "synthetic".'])
        for _ in range(3):
            self.assertEqual(self.ask("What is a gold hash?")["skill"], "kb_recall")
        paths = list((Path(self.temp.name) / "var/capsule_inbox").glob("kbnote-*/PROPOSE.json"))
        self.assertEqual(len(paths), 1)
        proposal = json.loads(paths[0].read_text())
        self.assertIn('"synthetic"', proposal["notes_preview"])
        self.assertIs(proposal["auto_cert"], False)
        self.assertEqual(proposal["claimed_cert"], 0)
        self.assertEqual(proposal["status"], "pending_verify")

    def test_indented_archive_cannot_match_answer_only(self):
        self.notes([" Q: Unrelated topic A: Gold hash"])
        self.assertNotEqual(self.ask("What is a gold hash?")["skill"], "kb_recall",
                            "RECALL_INDENTED_ARCHIVE_RED")

    def test_empty_archive_answer_refuses(self):
        for chunk in ["Q: Gold hash A:", "Q: Gold hash A:   "]:
            self.notes([chunk], kind="miss_harvest")
            self.assertNotEqual(self.ask("What is a gold hash?")["skill"], "kb_recall",
                                "RECALL_EMPTY_ANSWER_RED")

    def test_unicode_note_never_breaks_json_at_utterance_boundary(self):
        chunk = "Gold hash x" + "é" * 400
        self.notes([chunk])
        self.assertIn(chunk, self.ask("What is a gold hash?")["answer"])

    def test_unicode_adjacency_is_not_an_ascii_word_boundary(self):
        self.notes(["Gold hashé is a different token."])
        self.assertNotEqual(self.ask("What is a gold hash?")["skill"], "kb_recall",
                            "RECALL_UNICODE_BOUNDARY_RED")


if __name__ == "__main__":
    unittest.main()
