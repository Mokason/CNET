#!/usr/bin/env python3
"""Private native acquisition/evaluation/serving integration, not a supervisor soak."""
import hashlib
import os
import subprocess
import unittest

from test_capsule_daemon_lifecycle import REPO, ResidentDaemon


class TableLearningDaemon(ResidentDaemon):
    def test_table_acquisition_refresh_rollback_and_restart(self):
        data = self.root / "data"
        data.mkdir(mode=0o700)
        cache = self.root / "evaluation-cache"
        cache.mkdir(mode=0o700)
        self.env["CNET_CAPSULE_DATA_ROOT"] = str(data)
        self.stop()
        self.start()
        pid = self.proc.pid
        self.teach()  # Trusted incumbent fixture, not unattended acquisition.
        initial = self.stage(1, 1, "original")
        self.assertTrue(self.command(f"ACTIVATE 1 incumbent {initial}").startswith("OK "))

        def source(value):
            raw = ("CNET_LOCAL_TABLE_V1\ndataset calibration\nauthority verified_tool\n"
                   f"input_bits 8\noutput_bits 16\nrows 3\n0\t65535\n7\t{value}\n19\t0\n").encode()
            path = data / "calibration.tsv"
            path.write_bytes(raw)
            path.chmod(0o600)
            return raw

        def run(name, arguments):
            result = subprocess.run([str(REPO / "bin" / name), *map(str, arguments)],
                                    cwd=self.root, env={}, stdin=subprocess.DEVNULL,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=35)
            self.assertEqual(result.returncode, 0, "TABLE_DAEMON_REFUSED " + result.stderr.decode(errors="replace"))
            self.assertLessEqual(len(result.stdout), 8192)
            return result.stdout

        def acquire_and_evaluate(version, raw, value, revision):
            output = self.root / f"worker-{version}"
            output.mkdir(mode=0o700)
            run("cnet_table_capsule", ["build-worker", data, "calibration", output, os.getpid(), 10, 256])
            (output / "capsule").rename(self.root / "sets/original" / version)
            frozen = run("cnet_learning_snapshot", ["freeze", self.root / "sets/original", cache]).decode().splitlines()
            self.assertEqual(frozen[0], "CNET_LEARNING_SNAPSHOT_V1")
            expected_hash = frozen[1].removeprefix("snapshot_sha256 ")
            staged = self.stage(revision, 1, "original")
            self.assertEqual(staged, expected_hash)
            evaluate = self.root / f"evaluate-{version}"
            evaluate.mkdir(mode=0o700)
            observed = run("cnet_table_verify", ["snapshot-worker", data, "calibration",
                           self.root / "state/snapshots", staged, evaluate, os.getpid(), 10, 256])
            expected = ("CNET_TABLE_SNAPSHOT_EVAL_V1\nsnapshot_sha256 " + staged + "\ndataset calibration\n"
                        + "source_sha256 " + hashlib.sha256(raw).hexdigest() + "\nresults 256\n")
            labels = {0: 65535, 7: value, 19: 0}  # Independent fixture labels.
            expected += "".join(f"{key}\t1\t{labels[key]}\n" if key in labels else f"{key}\t0\t-\n"
                                for key in range(256)) + "end\n"
            self.assertEqual(observed, expected.encode())
            self.assertEqual((data / "calibration.tsv").read_bytes(), raw)
            return staged, labels

        def check_answers(labels):
            for key in range(256):
                reply = self.ask(f"data calibration {key}")
                self.assertFalse(reply["teacher"])
                self.assertEqual(reply["verified"], key in labels)
                if key in labels:
                    self.assertEqual(reply["answer"], str(labels[key]))
                else:
                    self.assertIn("ABSTAIN", reply["answer"])
            self.assertEqual(self.ask("capsule minutes seconds 3")["answer"], "180")

        raw_first = source(42)
        self.assertFalse(self.ask("data calibration 7")["verified"])
        first, labels = acquire_and_evaluate("table_v1", raw_first, 42, 2)
        self.assertFalse(self.ask("data calibration 7")["verified"], "STAGE must not activate")
        self.assertTrue(self.command(f"ACTIVATE 2 table-v1 {first}").startswith("OK "))
        check_answers(labels)

        raw_second = source(43)
        self.assertFalse(self.ask("data calibration 7")["verified"], "changed source must abstain")
        second, labels = acquire_and_evaluate("table_v2", raw_second, 43, 3)
        self.assertTrue(self.command(f"ACTIVATE 3 table-v2 {second}").startswith("OK "))
        check_answers(labels)
        self.assertEqual(self.proc.pid, pid, "hot swaps must retain the daemon process")
        self.assertEqual(len(list((self.root / "sets/original").glob("table_v*"))), 2)

        rollback = self.command("ROLLBACK 4 table-rollback")
        self.assertTrue(rollback.startswith("OK "))
        self.assertEqual(self.command("ROLLBACK 4 table-rollback"), rollback)
        # Rollback of knowledge does not silently roll back external source data.
        self.assertFalse(self.ask("data calibration 7")["verified"])
        self.assertEqual(source(42), raw_first)
        check_answers({0: 65535, 7: 42, 19: 0})
        self.stop()
        self.start()
        self.assertIn("revision=5", self.command("STATUS"))
        self.assertEqual(self.command("ROLLBACK 4 table-rollback"), rollback)
        self.assertEqual(self.ask("data calibration 7")["answer"], "42")


if __name__ == "__main__":
    unittest.main(verbosity=2)
