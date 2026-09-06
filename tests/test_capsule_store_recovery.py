#!/usr/bin/env python3
"""Actual-daemon refusal/recovery checks; no inherited lifecycle test copies."""
import hashlib
import os
import socket
import stat
import subprocess
import unittest

import test_capsule_daemon_lifecycle as lifecycle


class StoreRecovery(unittest.TestCase):
    def setUp(self):
        # Composition deliberately reuses only fixture methods, not the other
        # TestCase's test_* methods (which unittest would otherwise run twice).
        self.daemon = lifecycle.ResidentDaemon(methodName="runTest")
        self.addCleanup(self.daemon.doCleanups)
        self.daemon.setUp()
        self.daemon.teach()
        ticket = self.daemon.stage(1, 1, "original")
        result = self.daemon.command(f"ACTIVATE 1 recovery_fixture {ticket}")
        self.assertTrue(result.startswith("OK "), result)
        self.status = result
        self.fields = dict(part.split("=", 1) for part in result.split()[1:])
        self.daemon.stop()
        self.state = self.daemon.root / "state"
        self.selection = self.state / "selection"
        self.original = self.selection.read_bytes()
        self.assertEqual(self.original[:13], b"CNET-ACTIVE-1")
        self.assertEqual(hashlib.sha256(self.original[:-64]).hexdigest().encode(), self.original[-64:])

    def assert_refused(self):
        # subprocess.run kills and reaps on TimeoutExpired; a planted FIFO must
        # produce a refusal, not rely on that timeout for eventual cleanup.
        try:
            result = subprocess.run([str(lifecycle.REPO / "bin/cnetd")],
                                    cwd=self.daemon.root, env=self.daemon.env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=3)
        except subprocess.TimeoutExpired:
            self.fail("CAPSULE_STORE_RECOVERY_RED startup_hung")
        self.assertGreater(result.returncode, 0, "startup must refuse normally, not succeed or crash")
        self.assertIn(b"capsule", result.stderr.lower())
        for path in (self.daemon.sock, self.daemon.control):
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(.25)
                with self.assertRaises(OSError, msg="failed recovery must not leave a serving endpoint"):
                    client.connect(str(path))

    def assert_recovers(self):
        self.daemon.start()
        self.assertEqual(self.daemon.command("STATUS"), self.status)
        answer = self.daemon.ask("convert 3 minutes to seconds")
        self.assertTrue(answer["verified"])
        self.assertEqual(answer["answer"], "180")
        self.assertFalse(self.daemon.ask("convert 6 minutes to seconds")["verified"])
        self.daemon.stop()

    def corrupt_record(self, payload):
        self.selection.write_bytes(payload)
        identity = self.selection.stat()
        self.assert_refused()
        self.assertEqual(self.selection.read_bytes(), payload, "failed recovery must not reset or repair selection")
        after = self.selection.stat()
        self.assertEqual((after.st_dev, after.st_ino), (identity.st_dev, identity.st_ino))
        self.selection.write_bytes(self.original)
        self.assert_recovers()

    def test_truncated_selection_refuses_without_reset(self):
        for length in (0, 255, len(self.original) - 1):
            with self.subTest(length=length):
                self.corrupt_record(self.original[:length])

    def test_selection_checksum_corruption_refuses(self):
        damaged = bytearray(self.original)
        damaged[-1] = ord("0") if damaged[-1] != ord("0") else ord("1")
        self.corrupt_record(damaged)

    def test_reserved_fields_refuse_even_with_matching_checksum(self):
        # Test the format itself, independently of checksum detection. Header
        # padding, digest terminator and both reserved regions must be canonical.
        for offset in (14, 88, 219, 255):
            with self.subTest(offset=offset):
                damaged = bytearray(self.original)
                self.assertEqual(damaged[offset], 0)
                damaged[offset] = 1
                damaged[-64:] = hashlib.sha256(damaged[:-64]).hexdigest().encode()
                self.corrupt_record(damaged)

    def missing_snapshot(self, field):
        selected = self.state / "snapshots" / self.fields[field]
        self.assertTrue(selected.is_dir())
        hidden = self.daemon.root / f"held-{field}"
        # Cross-parent rename updates '..', requiring owner-write on this
        # otherwise read-only snapshot directory. The daemon is stopped; retain
        # all bytes and restore the original mode before testing recovery.
        mode = stat.S_IMODE(selected.stat().st_mode)
        selected.chmod(mode | stat.S_IWUSR)
        selected.rename(hidden)
        hidden.chmod(mode)
        try:
            self.assert_refused()
            self.assertEqual(self.selection.read_bytes(), self.original)
            self.assertTrue(hidden.is_dir())
            self.assertFalse(selected.exists())
        finally:
            hidden.chmod(mode | stat.S_IWUSR)
            hidden.rename(selected)
            selected.chmod(mode)
        self.assert_recovers()

    def test_missing_active_snapshot_refuses(self):
        self.missing_snapshot("active")

    def test_missing_rollback_snapshot_refuses_despite_valid_active(self):
        self.assertNotEqual(self.fields["active"], self.fields["rollback"])
        self.missing_snapshot("rollback")

    def substitute_selection(self, kind):
        saved = self.daemon.root / "saved-selection"
        self.selection.rename(saved)
        try:
            if kind == "symlink":
                self.selection.symlink_to(saved)
            else:
                os.mkfifo(self.selection, 0o600)
            before = self.selection.lstat()
            self.assert_refused()
            after = self.selection.lstat()
            self.assertEqual((after.st_dev, after.st_ino, after.st_mode),
                             (before.st_dev, before.st_ino, before.st_mode))
            self.assertEqual(saved.read_bytes(), self.original)
        finally:
            self.selection.unlink()
            saved.rename(self.selection)
        self.assert_recovers()

    def test_selection_symlink_refuses_without_touching_target(self):
        self.substitute_selection("symlink")

    def test_selection_fifo_refuses_without_hanging(self):
        self.substitute_selection("fifo")

    def test_unknown_pending_names_are_retained(self):
        for suffix in ("owner-data", "G" * 32):
            with self.subTest(suffix=suffix):
                pending = self.state / ("selection-pending-" + suffix)
                pending.write_bytes(b"unrecognized owner artifact\n")
                pending.chmod(0o600)
                self.assert_refused()
                self.assertEqual(pending.read_bytes(), b"unrecognized owner artifact\n")
                self.assertEqual(self.selection.read_bytes(), self.original)
                pending.unlink()
        self.assert_recovers()

    def test_nonregular_pending_entries_are_retained(self):
        pending = self.state / ("selection-pending-" + "a" * 32)
        target = self.daemon.root / "owner-data"
        target.write_bytes(b"not a pending record\n")
        for kind in ("fifo", "symlink", "directory"):
            with self.subTest(kind=kind):
                if kind == "fifo":
                    os.mkfifo(pending, 0o600)
                elif kind == "symlink":
                    pending.symlink_to(target)
                else:
                    pending.mkdir(mode=0o700)
                before = pending.lstat()
                self.assert_refused()
                after = pending.lstat()
                self.assertEqual((after.st_dev, after.st_ino, after.st_mode),
                                 (before.st_dev, before.st_ino, before.st_mode))
                self.assertEqual(target.read_bytes(), b"not a pending record\n")
                self.assertEqual(self.selection.read_bytes(), self.original)
                if stat.S_ISDIR(after.st_mode):
                    pending.rmdir()
                else:
                    pending.unlink()
        self.assert_recovers()

    def test_only_owned_canonical_pending_record_is_collected(self):
        pending = self.state / ("selection-pending-" + "b" * 32)
        pending.write_bytes(b"partial interrupted publication")
        pending.chmod(0o600)
        note = self.state / "operator-note"
        note.write_bytes(b"owner data outside the cleanup namespace\n")
        self.assert_recovers()
        self.assertFalse(pending.exists())
        self.assertEqual(note.read_bytes(), b"owner data outside the cleanup namespace\n")
        self.assertEqual(self.selection.read_bytes(), self.original)


if __name__ == "__main__":
    unittest.main(verbosity=2)
