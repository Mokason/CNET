#!/usr/bin/env python3
"""The prerequisite guard must refuse missing and stale output, not tolerate it.

WHY THIS EXISTS. At 6f9c859, `make capability_cert` passed in the writer's
worktree and exited 2 in a fresh detached worktree of the same commit. The
difference was `dotnet/Cce.Llm.Tests/obj` -- ignored build state a fresh
checkout never has. `dotnet test --no-restore` against an unrestored project
emits ZERO BYTES and exits 0, so the evaluator "ran" without running, and the
capability could not be certified anywhere except on a machine that already
happened to hold the right build output.

The guard added for it (`scripts/capability_evaluator_prereq.py`) is only worth
having if its negatives hold, so this file attacks it:

  * an evaluator that does not build itself and declares no prepare step is
    REFUSED -- statically, before anything runs;
  * a prepare step that fails is refused, and the evaluator does not run;
  * a prepare step that SUCCEEDS without producing the declared binary is
    refused -- exit 0 is not evidence that a build happened;
  * a binary older than a declared source is refused as stale;
  * a shell prepare argv is refused;
  * the real committed manifests satisfy the declaration rule.

Every fixture here is a `mkdtemp` tree. Nothing in the repository is written,
built or removed by this file.

Usage: python3 tests/test_capability_evaluator_prereq.py
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

import capability_evaluator_prereq as PREREQ  # noqa: E402

MANIFEST_DIR = ROOT / "config" / "capability_manifests"


def manifest_for(
    binary: str = "bin/evaluator",
    sources: list[str] | None = None,
    evaluator: list[str] | None = None,
    prepare: list[str] | None = None,
) -> dict:
    body = {
        "capability_id": "fixture_capability",
        "evaluator": evaluator if evaluator is not None else ["dotnet", "test", "p"],
        "evaluator_sources": sources if sources is not None else ["src/evaluator.c"],
        "evaluator_binary": binary,
    }
    if prepare is not None:
        body["evaluator_prepare"] = prepare
    return body


class DeclarationRuleTests(unittest.TestCase):
    """The static half: what a manifest is allowed to leave unsaid."""

    def test_non_building_evaluator_without_prepare_is_refused(self) -> None:
        problems = PREREQ.declaration_problems(manifest_for())
        self.assertTrue(
            any("evaluator_prepare" in problem for problem in problems),
            f"a dotnet evaluator with no prepare step must be refused: {problems}",
        )

    def test_make_evaluator_needs_no_prepare(self) -> None:
        # `make TARGET` rebuilds its target from its own prerequisites and
        # fails loudly when it cannot, which is exactly why every make-based
        # capability survived the fresh worktree that broke the dotnet one.
        self.assertEqual(
            PREREQ.declaration_problems(
                manifest_for(evaluator=["make", "some_target"])
            ),
            [],
        )

    def test_shell_prepare_is_refused(self) -> None:
        problems = PREREQ.declaration_problems(
            manifest_for(prepare=["sh", "-c", "touch /tmp/pwned"])
        )
        self.assertTrue(
            any("allowlisted" in problem for problem in problems), problems
        )

    def test_prepare_without_a_declared_binary_is_refused(self) -> None:
        body = manifest_for(prepare=["make", "thing"])
        del body["evaluator_binary"]
        problems = PREREQ.declaration_problems(body)
        self.assertTrue(
            any("evaluator_binary" in problem for problem in problems), problems
        )

    def test_empty_prepare_argv_is_refused(self) -> None:
        problems = PREREQ.declaration_problems(manifest_for(prepare=[]))
        self.assertTrue(problems)

    def test_committed_manifests_satisfy_the_rule(self) -> None:
        manifests = sorted(MANIFEST_DIR.glob("*.json"))
        self.assertTrue(manifests, "there are capability manifests to check")
        for path in manifests:
            manifest = json.loads(path.read_text(encoding="utf-8"))
            with self.subTest(capability=manifest["capability_id"]):
                self.assertEqual(PREREQ.declaration_problems(manifest), [])


class ReadinessTests(unittest.TestCase):
    """The dynamic half: prepare runs, and its OUTPUT is what is believed."""

    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory(prefix="cnet-prereq-")
        self.root = Path(self.directory.name)
        (self.root / "src").mkdir()
        (self.root / "bin").mkdir()
        (self.root / "src" / "evaluator.c").write_text(
            "int main(void){return 0;}\n", encoding="utf-8"
        )

    def tearDown(self) -> None:
        self.directory.cleanup()

    def write_makefile(self, body: str) -> None:
        (self.root / "Makefile").write_text(body, encoding="utf-8")

    def test_prepare_that_fails_is_refused(self) -> None:
        self.write_makefile("build:\n\t@echo cannot build; exit 3\n")
        problems = PREREQ.ensure_ready(
            self.root, manifest_for(prepare=["make", "build"])
        )
        self.assertTrue(
            any("evaluator_prepare failed" in problem for problem in problems),
            problems,
        )

    def test_prepare_that_produces_nothing_is_refused(self) -> None:
        # The decisive case: exit 0 is not evidence that a build happened.
        # `dotnet test --no-restore` exits 0 having produced nothing at all.
        self.write_makefile("build:\n\t@true\n")
        problems = PREREQ.ensure_ready(
            self.root, manifest_for(prepare=["make", "build"])
        )
        self.assertTrue(
            any("absent after prepare" in problem for problem in problems), problems
        )

    def test_stale_binary_is_refused(self) -> None:
        self.write_makefile("build:\n\t@true\n")
        binary = self.root / "bin" / "evaluator"
        binary.write_text("stale\n", encoding="utf-8")
        source = self.root / "src" / "evaluator.c"
        # The source is touched a full second AFTER the binary: no filesystem
        # timestamp granularity can make this comparison ambiguous.
        os.utime(binary, ns=(1_000_000_000_000, 1_000_000_000_000))
        os.utime(source, ns=(2_000_000_000_000, 2_000_000_000_000))
        problems = PREREQ.ensure_ready(
            self.root, manifest_for(prepare=["make", "build"])
        )
        self.assertTrue(
            any("older than its declared source" in problem for problem in problems),
            problems,
        )

    def test_fresh_binary_is_accepted(self) -> None:
        self.write_makefile("build:\n\t@cp src/evaluator.c bin/evaluator\n")
        self.assertEqual(
            PREREQ.ensure_ready(self.root, manifest_for(prepare=["make", "build"])),
            [],
        )

    def test_missing_declared_source_is_refused(self) -> None:
        self.write_makefile("build:\n\t@cp src/evaluator.c bin/evaluator\n")
        problems = PREREQ.ensure_ready(
            self.root,
            manifest_for(
                prepare=["make", "build"],
                sources=["src/evaluator.c", "src/absent.c"],
            ),
        )
        self.assertTrue(
            any("source is missing" in problem for problem in problems), problems
        )

    def test_prepare_path_cannot_escape_the_repository(self) -> None:
        self.write_makefile("build:\n\t@true\n")
        with self.assertRaises(ValueError):
            PREREQ.freshness_problems(
                self.root, manifest_for(binary="../outside/evaluator")
            )

    def test_a_self_building_evaluator_is_not_prepared(self) -> None:
        # No prepare declared, so nothing runs and nothing is asserted about a
        # binary the evaluator itself will produce.
        self.write_makefile("build:\n\t@exit 9\n")
        self.assertEqual(
            PREREQ.ensure_ready(
                self.root, manifest_for(evaluator=["make", "target"])
            ),
            [],
        )


class CommandLineTests(unittest.TestCase):
    """The CLI is what `make` runs, so its verdict line is part of the gate."""

    def test_real_manifests_report_ready(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(ROOT / "scripts" / "capability_evaluator_prereq.py")],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=900,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout)
        self.assertIn("CAPABILITY_EVALUATOR_PREREQ_PASS", completed.stdout)
        self.assertNotIn("CAPABILITY_EVALUATOR_PREREQ_FAIL", completed.stdout)

    def test_an_unknown_capability_is_refused(self) -> None:
        completed = subprocess.run(
            [
                sys.executable,
                str(ROOT / "scripts" / "capability_evaluator_prereq.py"),
                "no_such_capability",
            ],
            cwd=ROOT,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=900,
            check=False,
        )
        self.assertEqual(completed.returncode, 1, completed.stdout)


if __name__ == "__main__":
    result = unittest.main(exit=False, verbosity=1).result
    if result.wasSuccessful():
        print(
            "CAPABILITY_EVALUATOR_PREREQ_UNIT_PASS "
            f"checks={result.testsRun} refusals=missing,stale,failed,undeclared"
        )
        raise SystemExit(0)
    print(
        "CAPABILITY_EVALUATOR_PREREQ_UNIT_FAIL "
        f"checks={result.testsRun} failures={len(result.failures)} "
        f"errors={len(result.errors)}"
    )
    raise SystemExit(1)
