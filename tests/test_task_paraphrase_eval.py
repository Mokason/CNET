"""Scoring/integrity tests use developer fixtures, never sealed confirmation."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest

MODULE = Path(__file__).resolve().parents[1] / "tools/task_paraphrase_eval/evaluate.py"
if not MODULE.exists():
    raise SystemExit("PARAPHRASE_EVAL_RED missing_bounded_frozen_scorer")
spec = importlib.util.spec_from_file_location("paraphrase_eval", MODULE)
evaluator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evaluator)


def fixture():
    return {"schema": 1, "name": "qualification", "cases": [
        {"id": f"q{i:03}", "family": f"family_{i % 8}", "text": f"developer fixture {i}",
         "status": "ready" if i < 80 else "clarify" if i < 104 else "abstain",
         "operation": "upper" if i < 40 else "lower" if i < 80 else None,
         "key": i if i < 80 else None} for i in range(128)]}


def load(value):
    raw = json.dumps(value, ensure_ascii=False).encode()
    return evaluator.load_corpus(raw, hashlib.sha256(raw).hexdigest(), "qualification")


def perfect(cases):
    return [{"Status": c["status"], "Code": "fixture", "Dataset":
             f'unicode17_{c["operation"]}_latin1' if c["status"] == "ready" else None,
             "Key": c["key"], "Prompt": "clarify" if c["status"] == "clarify" else None}
            for c in cases]


class CorpusIntegrity(unittest.TestCase):
    def test_valid_fixed_population(self):
        self.assertEqual(len(load(fixture())), 128)

    def test_mutations_refuse_without_changing_denominators(self):
        for mutation in ("schema", "name", "extra", "count", "duplicate_id", "duplicate_text", "family",
                         "unknown_status", "key_bool", "key_range", "missing_key", "nonready_key", "operation", "quota"):
            with self.subTest(mutation=mutation):
                value = fixture()
                if mutation == "schema": value["schema"] = True
                elif mutation == "name": value["name"] = "confirmation"
                elif mutation == "extra": value["cases"][0]["extra"] = "ignored"
                elif mutation == "count": value["cases"].pop()
                elif mutation == "duplicate_id": value["cases"][1]["id"] = value["cases"][0]["id"]
                elif mutation == "duplicate_text": value["cases"][1]["text"] = value["cases"][0]["text"]
                elif mutation == "family": value["cases"][0]["family"] = "../escape"
                elif mutation == "unknown_status": value["cases"][0]["status"] = "other"
                elif mutation == "key_bool": value["cases"][0]["key"] = True
                elif mutation == "key_range": value["cases"][0]["key"] = 256
                elif mutation == "missing_key": del value["cases"][0]["key"]
                elif mutation == "nonready_key": value["cases"][80]["key"] = 1
                elif mutation == "operation": value["cases"][0]["operation"] = "title"
                elif mutation == "quota": value["cases"][0]["operation"] = "lower"
                with self.assertRaises(ValueError): load(value)

    def test_hash_duplicate_json_properties_and_byte_bound(self):
        raw = json.dumps(fixture()).encode()
        with self.assertRaises(ValueError): evaluator.load_corpus(raw, "0" * 64, "qualification")
        duplicate = raw.replace(b'"schema": 1', b'"schema": 0, "schema": 1')
        with self.assertRaises(ValueError):
            evaluator.load_corpus(duplicate, hashlib.sha256(duplicate).hexdigest(), "qualification")
        oversized = b" " * 131073
        with self.assertRaises(ValueError):
            evaluator.load_corpus(oversized, hashlib.sha256(oversized).hexdigest(), "qualification")


class ExactScoring(unittest.TestCase):
    def setUp(self):
        self.cases = load(fixture())
        self.proposals = perfect(self.cases)

    def test_exact_success_reports_all_rows_and_categories(self):
        result = evaluator.score(self.cases, self.proposals)
        self.assertTrue(result["passed"])
        self.assertEqual(result["total"], 128)
        self.assertEqual(result["exact"], 128)
        self.assertEqual(len(result["cases"]), 128)
        self.assertEqual(result["by_status"]["ready"], {"total": 80, "exact": 80})
        self.assertEqual(result["by_operation"]["upper"], {"total": 40, "exact": 40})
        self.assertEqual(len(result["by_family"]), 8)

    def test_wrong_ready_proposal_cannot_hide_inside_high_accuracy(self):
        for index in (0, 80, 104):
            with self.subTest(index=index):
                predictions = copy.deepcopy(self.proposals)
                predictions[index] = {"Status": "ready", "Code": "fixture", "Dataset": "unicode17_upper_latin1", "Key": 255, "Prompt": None}
                result = evaluator.score(self.cases, predictions)
                self.assertFalse(result["passed"])
                self.assertEqual(result["wrong_ready"], 1)
                self.assertEqual(result["exact"], 127)

    def test_conservative_refusals_reduce_useful_coverage(self):
        for i in range(9): self.proposals[i] = {"Status": "abstain", "Code": "fixture", "Dataset": None, "Key": None, "Prompt": None}
        result = evaluator.score(self.cases, self.proposals)
        self.assertFalse(result["passed"])
        self.assertEqual(result["wrong_ready"], 0)
        self.assertEqual(result["by_status"]["ready"]["exact"], 71)

    def test_operation_floor_cannot_be_hidden_by_other_operation(self):
        for i in range(7): self.proposals[i] = {"Status": "abstain", "Code": "fixture", "Dataset": None, "Key": None, "Prompt": None}
        self.assertFalse(evaluator.score(self.cases, self.proposals)["passed"])

    def test_clarification_and_ood_floors_are_separate(self):
        for first, count, status in ((80, 3, "abstain"), (104, 2, "clarify")):
            predictions = copy.deepcopy(self.proposals)
            for i in range(first, first + count):
                predictions[i] = {"Status": status, "Code": "fixture", "Dataset": None, "Key": None, "Prompt": "clarify" if status == "clarify" else None}
            self.assertFalse(evaluator.score(self.cases, predictions)["passed"])

    def test_malformed_or_missing_outputs_cannot_be_scored_as_safe(self):
        for malformed in ({}, {"Status": "clarify", "Code": "bad", "Dataset": "unicode17_upper_latin1", "Key": 1, "Prompt": "?"},
                          {"Status": "unknown", "Code": "bad", "Dataset": None, "Key": None, "Prompt": None},
                          {"Status": "ready", "Code": "bad", "Dataset": "unicode17_upper_latin1", "Key": True, "Prompt": None}):
            predictions = copy.deepcopy(self.proposals)
            predictions[0] = malformed
            with self.assertRaises(ValueError): evaluator.score(self.cases, predictions)
        with self.assertRaises(ValueError): evaluator.score(self.cases, self.proposals[:-1])


if __name__ == "__main__":
    unittest.main(verbosity=2)
