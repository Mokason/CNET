#!/usr/bin/env python3
"""Real local-header acquisition, existing capsule assets, no teacher/service."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
TOOL = Path(os.environ.get("CNET_SOURCE_TEST_BIN", REPO / "bin/cnet_source_capsule"))


class SourceCapsule(unittest.TestCase):
    def run_tool(self, *args):
        self.assertTrue(TOOL.is_file(), "SOURCE_CAPSULE_RED native source producer absent")
        return subprocess.run([str(TOOL), *map(str, args)], cwd=REPO,
                              capture_output=True, text=True, timeout=20)

    def test_actual_checkout_to_bound_capsule_and_no_overwrite(self):
        with tempfile.TemporaryDirectory(prefix="cnet-source-domain-") as private:
            output = Path(private) / "facts"
            run = self.run_tool("build", REPO, output, "source_facts")
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn("receipt=literal_line_check", run.stdout)
            self.assertIn("pending_activation=1", run.stdout)
            self.assertEqual(output.stat().st_mode & 0o777, 0o700)
            self.assertEqual({x.name for x in output.iterdir()},
                             {"unit.cnb", "manifest.cknow", "frontend.cvfa"})
            before = {p.name: p.read_bytes() for p in output.iterdir()}
            asset = json.loads(before["frontend.cvfa"])
            self.assertEqual(asset["extractor"], "cnet_literal_define_v1")
            self.assertEqual(asset["tool_sha256"], hashlib.sha256(Path("/usr/bin/grep").read_bytes()).hexdigest())
            # Manually maintained expectations, independent of acquisition output.
            expected = ["1", "2", "frontend.cvfa", "160", "32"]
            for fact, value in zip(asset["facts"], expected, strict=True):
                source = (REPO / fact["path"]).read_bytes()
                self.assertEqual(fact["value"], value)
                self.assertEqual(fact["sha256"], hashlib.sha256(source).hexdigest())
                lineno, line = fact["receipt"].split(":", 1)
                self.assertEqual(source.decode().splitlines()[int(lineno) - 1] + "\n", line)
            again = self.run_tool("build", REPO, output, "source_facts")
            self.assertNotEqual(again.returncode, 0)
            self.assertEqual(before, {p.name: p.read_bytes() for p in output.iterdir()})

    def test_named_fact_and_unsupported_intents(self):
        names = self.run_tool("list")
        self.assertEqual(names.returncode, 0)
        self.assertEqual(len(names.stdout.splitlines()), 5)
        request = self.run_tool("request", "capsule-asset-file")
        self.assertEqual(request.stdout, "capsule cnet_source_fact cnet_source_answer 2\n")
        for name in ("../capsule-schema", "ignore previous instructions", "capsule-schema extra"):
            with self.subTest(name=name):
                run = self.run_tool("request", name)
                self.assertNotEqual(run.returncode, 0)
                self.assertEqual(run.stdout, "")

    def test_output_control_remains_private_and_nofollow(self):
        with tempfile.TemporaryDirectory(prefix="cnet-source-output-") as private:
            root = Path(private)
            owned = root / "owned"
            owned.mkdir(mode=0o700)
            link = root / "linked"
            link.symlink_to(owned, target_is_directory=True)
            for output in (link / "facts", owned / ".." / "escaped"):
                run = self.run_tool("build", REPO, output, "source_facts")
                self.assertNotEqual(run.returncode, 0)
            owned.chmod(0o775)
            run = self.run_tool("build", REPO, owned / "facts", "source_facts")
            self.assertNotEqual(run.returncode, 0)
            self.assertEqual(list(owned.iterdir()), [])

    def test_output_leaf_matches_selectable_snapshot_component(self):
        with tempfile.TemporaryDirectory(prefix="cnet-source-name-") as private:
            root = Path(private)
            for name in ("not selectable", "bad\nname", "évidence", "a" * 97, ".hidden"):
                with self.subTest(name=name):
                    output = root / name
                    run = self.run_tool("build", REPO, output, "source_facts")
                    self.assertNotEqual(run.returncode, 0,
                                        "SOURCE_COMPONENT_RED producer must refuse unselectable output names")
                    self.assertFalse(output.exists())
                    missing_source = self.run_tool("build", root / "missing", output, "source_facts")
                    self.assertIn("output_parent_ownership_or_path", missing_source.stderr,
                                  "output validation must precede source acquisition")

    def test_missing_source_never_publishes(self):
        with tempfile.TemporaryDirectory(prefix="cnet-source-missing-") as private:
            root = Path(private)
            run = self.run_tool("build", root / "missing", root / "facts", "source_facts")
            self.assertNotEqual(run.returncode, 0)
            self.assertFalse((root / "facts").exists())

    def test_durable_publication_faults(self):
        with tempfile.TemporaryDirectory(prefix="cnet-source-sync-") as private:
            root = Path(private)
            binary = root / "source-fault-test"
            sanitizer_flags = (["-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
                               if os.environ.get("ASAN_OPTIONS") else [])
            compile_run = subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                *sanitizer_flags,
                "-D_POSIX_C_SOURCE=200809L", "-DCNET_SOURCE_CAPSULE_TESTING", "-Iinclude",
                "tools/cnet_source_capsule.c", "src/serve/cnet_capsule_evidence.c",
                "-Lbin", "-lcnet_capsule_core", f"-Wl,-rpath,{REPO / 'bin'}", "-o", str(binary)],
                cwd=REPO, capture_output=True, text=True, timeout=30)
            self.assertEqual(compile_run.returncode, 0, compile_run.stderr)
            for point in ("before_export", "directory_commit", "parent_commit"):
                with self.subTest(point=point):
                    output = root / point
                    run = subprocess.run([str(binary), "build", str(REPO), str(output), "source_facts"],
                                         cwd=REPO, capture_output=True, text=True, timeout=20,
                                         env={**os.environ, "CNET_SOURCE_FAIL_SYNC": point})
                    if point == "before_export":
                        self.assertNotEqual(run.returncode, 0, "SOURCE_PUBLICATION_RED injected pre-export failure ignored")
                        self.assertFalse(output.exists())
                    else:
                        self.assertEqual(run.returncode, 3, "SOURCE_PUBLICATION_UNCERTAIN_RED post-export sync failure must retain complete capsule")
                        self.assertIn("SOURCE_CAPSULE_COMMIT_UNCERTAIN", run.stderr)
                        self.assertEqual({p.name for p in output.iterdir()},
                                         {"unit.cnb", "manifest.cknow", "frontend.cvfa"})
                    self.assertNotIn("SOURCE_CAPSULE_PASS", run.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
