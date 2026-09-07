"""Opt-in real-runtime contract tests for the synthetic resident scale harness."""
import json
import importlib.util
import io
import errno
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stdout, redirect_stderr
from unittest.mock import patch
from unittest.mock import Mock


REPOSITORY = Path(__file__).resolve().parents[1]
RUNNER = REPOSITORY / "scripts/capsule_scale_bench.py"
sys.dont_write_bytecode = True


class CapsuleScaleContract(unittest.TestCase):
    def run_scale(self, *args, timeout_seconds=180):
        command = [sys.executable, str(RUNNER), *args]
        process = subprocess.Popen(command, cwd=REPOSITORY, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            stdout, stderr = process.communicate(timeout=timeout_seconds)
        except subprocess.TimeoutExpired:
            # The runner owns a separate child session: parent-only SIGKILL
            # would bypass its finally cleanup. Request graceful teardown.
            process.terminate()
            stdout, stderr = process.communicate(timeout=10)
            raise AssertionError("CAPSULE_SCALE_TEST_TIMEOUT after owned cleanup: " + stderr)
        return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)

    def test_actual_resident_lifetime_and_measurement_receipts(self):
        result = self.run_scale("--counts", "2", "4", "--replicates", "1")
        self.assertEqual(result.returncode, 0,
                         "CAPSULE_SCALE_CONTRACT_RED " + result.stderr + result.stdout)
        rows = [json.loads(line) for line in result.stdout.splitlines()]
        measurements = [row for row in rows if row.get("event") == "measurement"]
        self.assertEqual([row["capsules"] for row in measurements], [2, 4])
        for row in measurements:
            count = row["capsules"]
            self.assertEqual(row["status"], "pass")
            self.assertEqual(row["distinct_identities"], count)
            self.assertEqual(row["correct"], 5 * count)
            self.assertEqual(row["ood_refused"], 251 * count)
            self.assertEqual(row["cross_port_refused"], count)
            self.assertEqual(row["old_pinned_correct"], 5 * (count - 1))
            self.assertTrue(row["old_pinned_new_unit_refused"])
            self.assertTrue(row["rollback_checked"])
            self.assertEqual(row["wrong_verified"], 0)
            self.assertGreater(row["serialized_bytes"], 0)
            self.assertGreater(row["rss_loaded_bytes"], 0)
            self.assertGreater(row["rss_overlap_bytes"], 0)
            self.assertGreaterEqual(row["rss_peak_observed_bytes"], row["rss_overlap_bytes"])
            self.assertGreater(row["load_ms"], 0)
            self.assertGreater(row["stage_ms"], 0)
            for kind in ("covered_us", "ood_us", "cross_port_us"):
                metric = row[kind]
                self.assertGreater(metric["samples"], 0)
                self.assertLessEqual(metric["p50"], metric["p95"])
                self.assertLessEqual(metric["p95"], metric["p99"])
                self.assertLessEqual(metric["p99"], metric["max"])
        artifact = Path(rows[0]["artifact_root"])
        executable = artifact / "capsule_scale_bench"
        quantiles = subprocess.run([str(executable), "selftest"], text=True, capture_output=True, timeout=10)
        self.assertEqual(quantiles.returncode, 0)
        self.assertIn("CAPSULE_SCALE_QUANTILE_PASS", quantiles.stdout)
        mismatch = subprocess.run([str(executable), "measure", str(artifact / "fixture-2"), "4"],
                                  text=True, capture_output=True, timeout=10)
        self.assertEqual(mismatch.returncode, 1)
        self.assertIn("identity_count", mismatch.stderr)
        corrupt = artifact / "corrupt-fixture"
        shutil.copytree(artifact / "fixture-2", corrupt)
        payload = corrupt / "before/scale_unit_0000/unit.cnb"
        with payload.open("r+b") as stream:
            first = stream.read(1)
            stream.seek(0); stream.write(bytes([first[0] ^ 1]))
        refused = subprocess.run([str(executable), "measure", str(corrupt), "2"],
                                 text=True, capture_output=True, timeout=10)
        self.assertEqual(refused.returncode, 1)
        self.assertIn("incumbent_load_or_replay", refused.stderr)
        print(result.stdout, end="")

    def test_invalid_counts_and_replicates_refuse(self):
        for args in (("--counts", "0"), ("--counts", "4097"),
                     ("--counts", "4", "2"), ("--counts", "2", "2"),
                     ("--replicates", "0"), ("--replicates", "4")):
            with self.subTest(args=args):
                self.assertNotEqual(self.run_scale(*args).returncode, 0)

    def test_resource_refusals_and_failure_receipts(self):
        specification = importlib.util.spec_from_file_location("capsule_scale_runner", RUNNER)
        runner = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(runner)
        with tempfile.TemporaryDirectory(prefix="cnet-scale-resource-test-") as directory:
            root = Path(directory)
            with patch.object(runner, "available_memory", return_value=0):
                self.assertEqual(runner.run_child(["/usr/bin/true"], root, "low-memory", 2),
                                 (None, "memory_preflight"))
                self.assertFalse(list(root.iterdir()))
            with patch.object(runner, "disk_bytes", return_value=runner.DISK_LIMIT):
                self.assertEqual(runner.run_child(["/usr/bin/true"], root, "full-disk", 2),
                                 (None, "disk_preflight"))
            with patch.object(runner, "WALL_SECONDS", 1):
                self.assertEqual(runner.run_child(["/usr/bin/sleep", "5"], root, "timeout", 2),
                                 (None, "wall_timeout"))
            self.assertTrue((root / "timeout.stderr").exists())
            self.assertEqual(runner.run_child(["/usr/bin/false"], root, "refusal", 2),
                             (None, "child_exit_1"))
        stdout, stderr = io.StringIO(), io.StringIO()
        with patch.object(sys, "argv", [str(RUNNER), "--counts", "2"]), \
                patch.object(runner, "available_memory", return_value=0), \
                redirect_stdout(stdout), redirect_stderr(stderr):
            self.assertEqual(runner.main(), 1)
        failure = json.loads(stdout.getvalue())
        self.assertEqual(failure["event"], "failure")
        self.assertEqual(failure["reason"], "memory_preflight")
        retained = Path(failure["artifact_root"]) / "results.jsonl"
        self.assertEqual(json.loads(retained.read_text()), failure)

    def test_accounting_failure_requires_confirmed_child_exit(self):
        specification = importlib.util.spec_from_file_location("capsule_scale_runner", RUNNER)
        runner = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(runner)
        completed = Mock(pid=12345)
        completed.poll.return_value = 1
        live = Mock(pid=12346)
        live.poll.return_value = None
        for fault in (PermissionError(errno.EACCES, "deterministic accounting race"),
                      FileNotFoundError(errno.ENOENT, "deterministic accounting race")):
            with self.subTest(fault=type(fault).__name__), patch.object(Path, "read_text", side_effect=fault):
                try:
                    observed = runner.child_written_bytes(completed)
                except OSError:
                    observed = "unhandled_completed_child_accounting_failure"
                self.assertEqual(observed, 0, "CAPSULE_SCALE_ACCOUNTING_RED completed child must reach post-exit checks")
                completed.poll.assert_called()
                with self.assertRaisesRegex(RuntimeError, "child write accounting unavailable"):
                    runner.child_written_bytes(live)

    def test_live_accounting_failure_reaps_own_real_child(self):
        specification = importlib.util.spec_from_file_location("capsule_scale_runner", RUNNER)
        runner = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(runner)
        real_read = Path.read_text
        children = []
        real_popen = subprocess.Popen

        def launch(*args, **kwargs):
            child = real_popen(*args, **kwargs)
            children.append(child)
            return child

        def denied(path, *args, **kwargs):
            if str(path).startswith("/proc/") and str(path).endswith("/io"):
                raise PermissionError(errno.EACCES, "deterministic live accounting failure")
            return real_read(path, *args, **kwargs)

        with tempfile.TemporaryDirectory(prefix="cnet-scale-accounting-test-") as directory, \
                patch.object(subprocess, "Popen", side_effect=launch), patch.object(Path, "read_text", denied):
            with self.assertRaisesRegex(RuntimeError, "child write accounting unavailable"):
                runner.run_child(["/usr/bin/sleep", "5"], Path(directory), "denied", 2)
        self.assertEqual(len(children), 1)
        self.assertIsNotNone(children[0].poll(), "CAPSULE_SCALE_ACCOUNTING_RED live denied child was not reaped")

    def test_sigterm_reaps_owned_new_session_child(self):
        self.check_terminated_runner(False)

    def test_outer_test_timeout_reaps_owned_new_session_child(self):
        self.check_terminated_runner(True)

    def test_first_sigterm_during_cleanup_reaps_owned_child(self):
        self.check_terminated_runner(False, during_cleanup=True)

    def check_terminated_runner(self, outer_timeout, during_cleanup=False):
        code = """
import importlib.util, os, pathlib, signal, subprocess, sys
spec = importlib.util.spec_from_file_location('runner', sys.argv[1])
runner = importlib.util.module_from_spec(spec); spec.loader.exec_module(runner)
pidfile = pathlib.Path(sys.argv[2]); real_popen = subprocess.Popen
state = {'accounting_failed': False, 'sent': False}
if sys.argv[3] == 'cleanup':
    def denied(process):
        state['accounting_failed'] = True
        raise RuntimeError('injected_accounting_failure')
    runner.child_written_bytes = denied
def launch(command, **kwargs):
    child = real_popen(['/usr/bin/sleep', '30'], **kwargs)
    real_poll = child.poll
    def cleanup_poll():
        if state['accounting_failed'] and not state['sent']:
            state['sent'] = True
            os.kill(os.getpid(), signal.SIGTERM)
        return real_poll()
    child.poll = cleanup_poll
    pidfile.write_text(str(child.pid)); return child
subprocess.Popen = launch
sys.argv = [str(runner.__file__), '--counts', '2', '--replicates', '1']
sys.exit(runner.main())
"""
        with tempfile.TemporaryDirectory(prefix="cnet-scale-sigterm-test-") as directory:
            pidfile = Path(directory) / "child.pid"
            process = subprocess.Popen([sys.executable, "-c", code, str(RUNNER), str(pidfile),
                                        "cleanup" if during_cleanup else "normal"],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            child = None
            try:
                deadline = time.monotonic() + 5
                while not pidfile.exists() and time.monotonic() < deadline:
                    time.sleep(.01)
                self.assertTrue(pidfile.exists(), "controlled child did not enter")
                child = int(pidfile.read_text())
                if outer_timeout:
                    with patch.object(subprocess, "Popen", return_value=process), \
                            self.assertRaisesRegex(AssertionError, "CAPSULE_SCALE_TEST_TIMEOUT"):
                        self.run_scale(timeout_seconds=.01)
                else:
                    if not during_cleanup:
                        process.send_signal(signal.SIGTERM)
                    stdout, stderr = process.communicate(timeout=5)
                self.assertNotEqual(process.returncode, 0)
                try:
                    os.kill(child, 0)
                    alive = True
                except ProcessLookupError:
                    alive = False
                self.assertFalse(alive, "CAPSULE_SCALE_SIGTERM_RED owned child survived runner TERM")
                if not outer_timeout:
                    self.assertIn("injected_accounting_failure" if during_cleanup else "runner_terminated_by_SIGTERM", stdout + stderr)
            finally:
                if child is not None:
                    try:
                        os.killpg(child, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                if process.poll() is None:
                    process.kill()
                process.communicate(timeout=5)

    def test_completed_accounting_race_retains_postexit_disk_check(self):
        specification = importlib.util.spec_from_file_location("capsule_scale_runner", RUNNER)
        runner = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(runner)
        read_accounting = runner.child_written_bytes

        def completed_accounting(process):
            self.assertEqual(process.wait(timeout=2), 0)
            with patch.object(Path, "read_text", side_effect=PermissionError(errno.EACCES, "completed race")):
                return read_accounting(process)

        with tempfile.TemporaryDirectory(prefix="cnet-scale-completed-test-") as directory, \
                patch.object(runner, "child_written_bytes", side_effect=completed_accounting) as accounting, \
                patch.object(runner, "disk_bytes", side_effect=[0, runner.DISK_LIMIT + 1]) as disk:
            self.assertEqual(runner.run_child(["/usr/bin/sleep", ".05"], Path(directory), "completed", 2),
                             (None, "disk_budget"))
            accounting.assert_called_once()
            self.assertEqual(disk.call_count, 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
