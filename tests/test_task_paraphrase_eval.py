"""Scoring/integrity tests use developer fixtures, never sealed confirmation."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

MODULE = Path(__file__).resolve().parents[1] / "tools/task_paraphrase_eval/evaluate.py"
if not MODULE.exists():
    raise SystemExit("PARAPHRASE_EVAL_RED missing_bounded_frozen_scorer")
spec = importlib.util.spec_from_file_location("paraphrase_eval", MODULE)
evaluator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(evaluator)
REPOSITORY = MODULE.parents[2]
PROBE = REPOSITORY / ".artifacts/task-verified/bin/TaskParaphraseProbe/debug/cnet-task-paraphrase-probe.dll"
ASSEMBLY = REPOSITORY / ".artifacts/task-verified/bin/CnetControlPlane/debug/cnet-control.dll"


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
    def test_nonscalar_fields_fail_with_validation_errors(self):
        for key in ("status", "operation"):
            value = fixture()
            value["cases"][0][key] = []
            with self.assertRaises(ValueError, msg="PARAPHRASE_TYPES_RED invalid scalar shape"):
                load(value)

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


class ActualManagedProbe(unittest.TestCase):
    def invoke(self, raw, identity=None):
        self.assertTrue(PROBE.is_file(), "PARAPHRASE_PROBE_RED missing_hash_bound_managed_probe")
        identity = identity or hashlib.sha256(ASSEMBLY.read_bytes()).hexdigest()
        return subprocess.run([os.environ.get("DOTNET_HOST_PATH", "dotnet"), str(PROBE), str(ASSEMBLY), identity],
                              input=raw, capture_output=True, timeout=30)

    def test_unchanged_real_parser_returns_only_proposals(self):
        result = self.invoke(evaluator.encode_requests(["uppercase µ", "uppercase 65", "unrelated request"]))
        self.assertEqual(result.returncode, 0, result.stderr)
        response = json.loads(result.stdout)
        self.assertEqual(response["schema"], 1)
        self.assertEqual(response["assembly_sha256"], hashlib.sha256(ASSEMBLY.read_bytes()).hexdigest())
        proposals = response["proposals"]
        self.assertEqual([p["Status"] for p in proposals], ["ready", "clarify", "abstain"])
        self.assertEqual(proposals[0]["Key"], 181)
        self.assertEqual(proposals[0]["Dataset"], "unicode17_upper_latin1")
        self.assertNotIn("Value", proposals[0])

    def test_hash_mismatch_refuses_before_parser_execution(self):
        result = self.invoke(evaluator.encode_requests(["uppercase A"]), "0" * 64)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, b"")

    def test_lone_surrogates_reach_the_actual_parser_as_abstentions(self):
        requests = ['uppercase "\ud800"', 'lowercase "\udfff"', 'uppercase 😀']
        result = self.invoke(evaluator.encode_requests(requests))
        self.assertEqual(result.returncode, 0, "PARAPHRASE_SURROGATE_RED probe rejected parser-domain input")
        proposals = json.loads(result.stdout)["proposals"]
        self.assertEqual([p["Status"] for p in proposals], ["abstain"] * 3)
        self.assertTrue(all(p["Dataset"] is None and p["Key"] is None for p in proposals))

    def test_probe_input_bounds_and_types_refuse(self):
        for raw in (b'{}', b'[]', b'[null]', b'[1]', b'not json', b' ' * (4 * 1024 * 1024 + 1),
                    b'[[65536]]', b'[[-1]]', b'[[1.5]]', b'[[true]]', b'[["65"]]', b'["uppercase A"]',
                    json.dumps([[65] * 4097]).encode(), evaluator.encode_requests(["uppercase A"] * 129)):
            with self.subTest(length=len(raw)):
                result = self.invoke(raw)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, b"")


class EvaluationCommand(unittest.TestCase):
    def test_missing_pins_do_not_return_success(self):
        result = subprocess.run(["python3", str(MODULE), "qualification"], capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0, "PARAPHRASE_CLI_RED missing pins silently succeed")

    def test_bad_parser_pin_and_existing_output_refuse(self):
        with tempfile.TemporaryDirectory(prefix="cnet-paraphrase-cli-") as temporary:
            output = Path(temporary) / "report.json"
            args = ["python3", str(MODULE), "qualification", "--assembly", str(ASSEMBLY),
                    "--assembly-sha256", hashlib.sha256(ASSEMBLY.read_bytes()).hexdigest(),
                    "--parser-sha256", "0" * 64, "--output", str(output)]
            result = subprocess.run(args, capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0, "PARAPHRASE_CLI_RED wrong parser pin accepted")
            output.write_bytes(b"retained")
            result = subprocess.run(args, capture_output=True, timeout=10)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_bytes(), b"retained")


class RunnerIntegrity(unittest.TestCase):
    """Exercise wiring with developer-only bytes, not either frozen collection."""
    def setUp(self):
        self.corpus = json.dumps(fixture()).encode()
        self.frozen = json.dumps({"qualification_sha256": evaluator.digest(self.corpus)}).encode()
        self.response = {"schema": 1, "assembly_sha256": evaluator.digest(b"assembly"),
                         "proposals": perfect(load(fixture()))}
        self.args = SimpleNamespace(suite="original", collection="qualification", assembly="/fixture/assembly.dll",
                                    assembly_sha256=evaluator.digest(b"assembly"), parser_sha256=evaluator.digest(b"source"))

    def read(self, path, limit=None):
        return {"freeze.json": self.frozen, "LearningTaskProposal.cs": b"source", "assembly.dll": b"assembly",
                "cnet-task-paraphrase-probe.dll": b"probe", "qualification.json": self.corpus,
                "confirmation.json": self.corpus}[Path(path).name]

    def invoke(self, *, read=None, stdout=None, returncode=0, stderr=b"", dotnet="/fixture/dotnet"):
        with patch.object(evaluator, "FREEZE_SHA256", evaluator.digest(self.frozen)), \
             patch.object(evaluator, "FOLLOWUP_FREEZE_SHA256", evaluator.digest(self.frozen)), \
             patch.object(evaluator, "read_bounded", side_effect=read or self.read), \
             patch.object(evaluator.shutil, "which", return_value=dotnet), \
             patch.object(evaluator.subprocess, "run", return_value=SimpleNamespace(
                 returncode=returncode, stderr=stderr, stdout=stdout or json.dumps(self.response).encode())) as probe:
            result = evaluator.run(self.args)
            self.assertEqual(probe.call_args.kwargs["env"], {})
            self.assertEqual(probe.call_args.kwargs["timeout"], 30)
            self.assertEqual(len(json.loads(probe.call_args.kwargs["input"])), 128)
            return result

    def test_identity_bound_report_keeps_all_cases_and_synthetic_origin(self):
        result = self.invoke()
        self.assertTrue(result["passed"])
        self.assertEqual(result["origin"], "synthetic")
        self.assertFalse(result["training_eligible"])
        self.assertEqual(result["probe_sha256"], evaluator.digest(b"probe"))
        self.assertEqual(len(result["cases"]), 128)

    def test_reports_identify_the_selected_suite(self):
        result = self.invoke()
        self.assertIn("suite", result, "PARAPHRASE_SUITE_RED missing population identity")
        self.assertEqual(result["suite"], "original")

    def test_unknown_suites_and_unavailable_collections_refuse(self):
        for suite in ("../escape", "followup", "round3"):
            self.args.suite = suite
            with self.subTest(suite=suite), self.assertRaises(ValueError, msg="PARAPHRASE_SUITE_RED silently selected original data"):
                self.invoke()  # Follow-up has confirmation only, no qualification.

    def test_followup_reads_only_its_separately_pinned_confirmation(self):
        self.args.suite = "followup"
        self.args.collection = "confirmation"
        self.corpus = json.dumps({**fixture(), "name": "confirmation"}).encode()
        self.frozen = json.dumps({"confirmation_sha256": evaluator.digest(self.corpus)}).encode()
        visited = []
        def read(path, limit=None):
            visited.append(Path(path))
            return self.read(path, limit)
        result = self.invoke(read=read)
        self.assertEqual(result["suite"], "followup")
        self.assertEqual(result["collection"], "confirmation")
        corpus_files = [p for p in visited if p.name in {"freeze.json", "confirmation.json", "qualification.json"}]
        self.assertTrue(all(p.parent.name == "task_paraphrases_followup_20260909" for p in corpus_files))
        self.assertNotIn("qualification.json", [p.name for p in corpus_files])

    def test_round3_uses_only_its_own_frozen_confirmation(self):
        self.args.suite = "round3"
        self.args.collection = "confirmation"
        self.corpus = json.dumps({**fixture(), "name": "confirmation"}).encode()
        self.frozen = json.dumps({"confirmation_sha256": evaluator.digest(self.corpus)}).encode()
        visited = []
        def read(path, limit=None):
            visited.append(Path(path))
            return self.read(path, limit)
        with patch.object(evaluator, "ROUND3_FREEZE_SHA256", evaluator.digest(self.frozen), create=True):
            result = self.invoke(read=read)
        self.assertEqual(result["suite"], "round3", "PARAPHRASE_ROUND3_RED wrong population")
        corpus_files = [p for p in visited if p.name in {"freeze.json", "confirmation.json", "qualification.json"}]
        self.assertTrue(all(p.parent.name == "task_paraphrases_round3_20260909" for p in corpus_files))
        self.assertNotIn("qualification.json", [p.name for p in corpus_files])

    def test_changed_artifacts_cannot_issue_a_successful_score(self):
        for name in ("freeze.json", "LearningTaskProposal.cs", "assembly.dll", "qualification.json"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.invoke(read=lambda path, limit=None: b"changed" if Path(path).name == name else self.read(path, limit))
        seen = 0
        def mutated_source(path, limit=None):
            nonlocal seen
            if Path(path).name == "LearningTaskProposal.cs":
                seen += 1
                if seen > 1: return b"changed after execution"
            return self.read(path, limit)
        with self.assertRaises(ValueError): self.invoke(read=mutated_source)

    def test_missing_runtime_failed_probe_and_malformed_receipts_refuse(self):
        for options in ({"dotnet": None}, {"returncode": 2}, {"stderr": b"failure"},
                        {"stdout": b"[]"}, {"stdout": b"{}"}, {"stdout": b"invalid json"}):
            with self.subTest(options=options), self.assertRaises(ValueError): self.invoke(**options)
        self.response["assembly_sha256"] = "0" * 64
        with self.assertRaises(ValueError): self.invoke()

    def test_main_reports_quality_failure_and_reserves_output(self):
        self.response["proposals"][0]["Key"] = 255  # Genuine score failure in the developer fixture.
        report = self.invoke()
        with tempfile.TemporaryDirectory(prefix="cnet-paraphrase-wiring-") as temporary:
            output = Path(temporary) / "report.json"
            args = ["qualification", "--assembly", self.args.assembly, "--assembly-sha256", self.args.assembly_sha256,
                    "--parser-sha256", self.args.parser_sha256, "--output", str(output)]
            with patch.object(evaluator, "run", return_value=report):
                self.assertEqual(evaluator.main(args), 1)
                self.assertEqual(json.loads(output.read_bytes())["total"], 128)
                self.assertEqual(output.stat().st_mode & 0o777, 0o600)
                self.assertEqual(evaluator.main(args), 2)
                args[-1] = "relative.json"
                self.assertEqual(evaluator.main(args), 2)

    def test_bounded_read_and_strict_utf8(self):
        with tempfile.TemporaryFile() as stream:
            stream.write(b"12345"); stream.flush()
            with self.assertRaises(ValueError): evaluator.read_bounded(f"/proc/self/fd/{stream.fileno()}", 4)
        with self.assertRaises(ValueError): evaluator.decode(b"\xff")


if __name__ == "__main__":
    unittest.main(verbosity=2)
