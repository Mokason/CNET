#!/usr/bin/env python3
"""Guarded native observations; independent expected numbers are test labels."""
import ctypes
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_table_capsule import source

REPO = Path(__file__).resolve().parents[1]
BUILDER = Path(os.environ.get("CNET_TABLE_TEST_BIN", REPO / "bin/cnet_table_capsule"))
VERIFIER = Path(os.environ.get("CNET_TABLE_VERIFY_TEST_BIN", REPO / "bin/cnet_table_verify"))
LIBRARY = Path(os.environ.get("CNET_TEST_CORE_LIBRARY", REPO / "bin/libcnet_capsule_core.so"))
SNAPSHOT = Path(os.environ.get("CNET_LEARNING_SNAPSHOT_TEST_BIN", REPO / "bin/cnet_learning_snapshot"))


class TableVerify(unittest.TestCase):
    def setUp(self):
        self.assertTrue(VERIFIER.is_file(), "TABLE_VERIFY_RED native evaluator absent")
        self.private = tempfile.TemporaryDirectory(prefix="cnet-table-verify-")
        self.addCleanup(self.private.cleanup)
        self.root = Path(self.private.name)
        self.data, self.registry, self.output = [self.root / name for name in ("data", "registry", "output")]
        for path in (self.data, self.registry, self.output):
            path.mkdir(mode=0o700)
        self.cache = self.root / "cache"
        self.cache.mkdir(mode=0o700)
        self.raw = source([(0, 120), (7, 42), (19, 0)], authority="verified_tool")
        self.write_source(self.raw)

    def write_source(self, raw):
        path = self.data / "stock_levels.tsv"
        path.write_bytes(raw)
        path.chmod(0o600)

    def build(self):
        run = subprocess.run([str(BUILDER), "build", str(self.data), "stock_levels", str(self.registry / "table")],
                             stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stderr)

    def verify(self, dataset="stock_levels", parent=None, cpu="2", memory="512", registry=None):
        return subprocess.run([str(VERIFIER), "verify-worker", str(self.data), dataset,
                               str(registry or self.registry), str(self.output),
                               str(os.getpid()) if parent is None else parent, cpu, memory],
                              stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10)

    def freeze(self, source_root=None, cache=None):
        self.assertTrue(SNAPSHOT.is_file(), "LEARNING_SNAPSHOT_RED trusted snapshot helper absent")
        return subprocess.run([str(SNAPSHOT), "freeze", str(source_root or self.registry), str(cache or self.cache)],
                              stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10)

    def frozen_identity(self, run):
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertTrue(run.stdout.endswith("\n"))
        lines = run.stdout.splitlines()
        self.assertEqual(len(lines), 4)
        self.assertEqual((lines[0], lines[-1]), ("CNET_LEARNING_SNAPSHOT_V1", "end"))
        self.assertRegex(lines[1], r"^snapshot_sha256 [0-9a-f]{64}$")
        self.assertRegex(lines[2], r"^bytes (0|[1-9][0-9]*)$")
        return lines[1].split()[1], int(lines[2].split()[1])

    def verify_snapshot(self, digest, parent=None, cpu="2", memory="512", cache=None):
        return subprocess.run([str(VERIFIER), "snapshot-worker", str(self.data), "stock_levels", str(cache or self.cache),
                               digest, str(self.output), str(os.getpid()) if parent is None else parent, cpu, memory],
                              stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10)

    def observations(self, run, raw=None, snapshot=None):
        self.assertEqual(run.returncode, 0, "TABLE_VERIFY_RED " + run.stderr)
        self.assertLess(len(run.stdout.encode("ascii")), 8192)
        self.assertNotIn("\r", run.stdout)
        self.assertTrue(run.stdout.endswith("\n"))
        lines = run.stdout.splitlines()
        header = ["CNET_TABLE_NATIVE_EVAL_V1"] if snapshot is None else [
            "CNET_TABLE_SNAPSHOT_EVAL_V1", "snapshot_sha256 " + snapshot]
        header += ["dataset stock_levels",
                   "source_sha256 " + hashlib.sha256(self.raw if raw is None else raw).hexdigest(), "results 256"]
        self.assertEqual(len(lines), len(header) + 257)
        self.assertEqual(lines[:len(header)], header)
        self.assertEqual(lines[-1], "end")
        values = {}
        for key, line in enumerate(lines[len(header):-1]):
            fields = line.split("\t")
            self.assertEqual(len(fields), 3)
            self.assertEqual(fields[0], str(key))
            self.assertIn(fields[1], ("0", "1"))
            if fields[1] == "0":
                self.assertEqual(fields[2], "-")
                values[key] = None
            else:
                value = int(fields[2])
                self.assertEqual(fields[2], str(value))
                self.assertTrue(0 <= value <= 65535)
                values[key] = value
        self.assertEqual(list(self.output.iterdir()), [])
        return values

    def test_reference_capsule_emits_exact_all_domain_observations(self):
        self.build()
        observed = self.observations(self.verify())
        expected = {0: 120, 7: 42, 19: 0}
        self.assertEqual(observed, {key: expected.get(key) for key in range(256)})

    def test_core_loader_retains_fd_path_after_registry_rename(self):
        self.build()
        library = ctypes.CDLL(str(LIBRARY))
        library.cnet_capsule_owner_directory.argtypes = [ctypes.c_char_p, ctypes.c_int]
        library.cnet_capsule_owner_directory.restype = ctypes.c_int
        library.cnet_capsule_core_open.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
        library.cnet_capsule_core_open.restype = ctypes.c_void_p
        library.cnet_capsule_core_close.argtypes = [ctypes.c_void_p]
        class Reply(ctypes.Structure):
            _fields_ = [("verified", ctypes.c_int), ("value", ctypes.c_uint),
                        ("hops", ctypes.c_size_t), ("units", ctypes.c_char * 512),
                        ("reason", ctypes.c_char * 160)]
        library.cnet_capsule_core_ask.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(Reply)]
        fd = library.cnet_capsule_owner_directory(os.fsencode(self.registry), 1)
        self.assertGreaterEqual(fd, 0)
        core = None
        previous = os.environ.get("CNET_CAPSULE_DATA_ROOT")
        os.environ["CNET_CAPSULE_DATA_ROOT"] = str(self.data)
        try:
            self.registry.rename(self.root / "retained_registry")
            self.registry.mkdir(mode=0o700)
            error = ctypes.create_string_buffer(160)
            core = library.cnet_capsule_core_open(f"/proc/self/fd/{fd}".encode(), error, len(error))
            self.assertTrue(core, "TABLE_VERIFY_PIN_RED " + error.value.decode())
            reply = Reply()
            self.assertEqual(library.cnet_capsule_core_ask(core, b"data stock_levels 7", ctypes.byref(reply)), 0)
            self.assertEqual((reply.verified, reply.value), (1, 42))
        finally:
            if core:
                library.cnet_capsule_core_close(core)
            os.close(fd)
            if previous is None:
                os.environ.pop("CNET_CAPSULE_DATA_ROOT", None)
            else:
                os.environ["CNET_CAPSULE_DATA_ROOT"] = previous

    def test_wrong_value_capsule_cannot_satisfy_corrected_reference(self):
        wrong = source([(0, 120), (7, 43), (19, 0)], authority="verified_tool")
        self.write_source(wrong)
        self.build()
        observed = self.observations(self.verify(), wrong)
        # A completed evaluator report is not an approval or a source of labels.
        self.assertEqual(observed[7], 43)
        self.assertNotEqual(observed[7], 42)
        self.write_source(self.raw)
        corrected = self.observations(self.verify())
        self.assertTrue(all(value is None for value in corrected.values()))

    def test_stale_source_abstains_and_corrupt_payload_refuses(self):
        self.build()
        fresh = source([(0, 120), (7, 42), (19, 0)], authority="user_correction")
        self.write_source(fresh)
        observed = self.observations(self.verify(), fresh)
        self.assertTrue(all(value is None for value in observed.values()))
        self.write_source(self.raw)
        payload = self.registry / "table/unit.cnb"
        damaged = bytearray(payload.read_bytes())
        damaged[len(damaged) // 2] ^= 1
        payload.write_bytes(damaged)
        run = self.verify()
        self.assertNotEqual(run.returncode, 0, "TABLE_VERIFY_RED corrupt candidate accepted")
        self.assertEqual(run.stdout, "")
        self.assertIn("TABLE_VERIFY_REFUSED candidate", run.stderr)

    def test_unsafe_source_registry_and_canonical_arguments_refuse(self):
        self.build()
        for dataset, parent, cpu, memory in (
            ("../stock_levels", None, "2", "512"), ("Stock_levels", None, "2", "512"),
            ("stock_levels", "0", "2", "512"), ("stock_levels", "01", "2", "512"),
            ("stock_levels", None, "02", "512"), ("stock_levels", None, "121", "512"),
            ("stock_levels", None, "2", "63"), ("stock_levels", None, "2", "2049"),
        ):
            with self.subTest(dataset=dataset, parent=parent, cpu=cpu, memory=memory):
                run = self.verify(dataset, parent, cpu, memory)
                self.assertNotEqual(run.returncode, 0)
                self.assertEqual(run.stdout, "")
        path = self.data / "stock_levels.tsv"
        path.chmod(0o644)
        self.assertNotEqual(self.verify().returncode, 0)
        path.chmod(0o600)
        self.registry.chmod(0o755)
        self.assertNotEqual(self.verify().returncode, 0)
        self.registry.chmod(0o700)
        alias = self.root / "registry_alias"
        alias.symlink_to(self.registry, target_is_directory=True)
        self.assertNotEqual(self.verify(registry=alias).returncode, 0)
        self.assertEqual(list(self.output.iterdir()), [])

    def test_guard_refuses_before_reading_inputs(self):
        path = self.data / "stock_levels.tsv"
        path.unlink()
        os.mkfifo(path, 0o600)
        run = self.verify(parent=str(os.getpid() + 1))
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("TABLE_VERIFY_REFUSED worker_sandbox", run.stderr)
        self.assertEqual(run.stdout, "")
        (self.output / "not_empty").write_bytes(b"owner")
        run = self.verify()
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("TABLE_VERIFY_REFUSED worker_sandbox", run.stderr)
        self.assertEqual((self.output / "not_empty").read_bytes(), b"owner")

    def test_snapshot_worker_uses_existing_canonical_identity(self):
        self.build()
        digest, size = self.frozen_identity(self.freeze())
        self.assertEqual(self.frozen_identity(self.freeze()), (digest, size))
        self.assertEqual({p.name for p in self.cache.iterdir()}, {digest})
        # This is the same existing native snapshot API called by STAGE, not
        # a test reimplementation of its inventory hash algorithm.
        library = ctypes.CDLL(str(LIBRARY))
        library.cnet_capsule_snapshot_verify.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(ctypes.c_size_t)]
        fd = os.open(self.cache, os.O_RDONLY | os.O_DIRECTORY)
        try:
            verified_size = ctypes.c_size_t()
            self.assertEqual(library.cnet_capsule_snapshot_verify(fd, digest.encode(), ctypes.byref(verified_size)), 0)
            self.assertEqual(verified_size.value, size)
        finally:
            os.close(fd)
        observed = self.observations(self.verify_snapshot(digest), snapshot=digest)
        self.assertEqual(observed, {key: {0: 120, 7: 42, 19: 0}.get(key) for key in range(256)})

    def test_frozen_identity_matches_actual_native_stage_and_evaluation(self):
        self.build()
        digest, _ = self.frozen_identity(self.freeze())
        state = self.root / "native_state"
        state.mkdir(mode=0o700)
        library = ctypes.CDLL(str(LIBRARY))
        library.cnet_capsule_store_open.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        library.cnet_capsule_store_open.restype = ctypes.c_void_p
        library.cnet_capsule_store_close.argtypes = [ctypes.c_void_p]
        library.cnet_capsule_store_stage.argtypes = [ctypes.c_void_p, ctypes.c_uint64, ctypes.c_char_p,
                                                   ctypes.c_int, ctypes.c_char_p]
        class Status(ctypes.Structure):
            _fields_ = [("revision", ctypes.c_uint64), ("active", ctypes.c_char * 65),
                        ("rollback", ctypes.c_char * 65), ("staged", ctypes.c_char * 65),
                        ("uncertain", ctypes.c_int)]
        library.cnet_capsule_store_status.argtypes = [ctypes.c_void_p, ctypes.POINTER(Status)]
        store = library.cnet_capsule_store_open(os.fsencode(self.root), os.fsencode(state))
        self.assertTrue(store)
        try:
            status = Status()
            self.assertEqual(library.cnet_capsule_store_status(store, ctypes.byref(status)), 0)
            before_revision, before_active = status.revision, status.active
            staged = ctypes.create_string_buffer(65)
            # STAGE set identifiers are <=63 characters; the 64-character
            # snapshot digest is an identity, never a new set-name convention.
            self.assertEqual(library.cnet_capsule_store_stage(store, before_revision, b"registry", 1, staged), 0)
            self.assertEqual(staged.value.decode(), digest)
            self.assertEqual(library.cnet_capsule_store_status(store, ctypes.byref(status)), 0)
            self.assertEqual((status.revision, status.active), (before_revision, before_active))
            self.assertEqual(status.staged.decode(), digest)
            observed = self.observations(self.verify_snapshot(digest, cache=state / "snapshots"), snapshot=digest)
            self.assertEqual(observed[7], 42)
        finally:
            self.assertEqual(library.cnet_capsule_store_close(store), 0)

    def test_changed_snapshot_or_wrong_identity_refuses(self):
        self.build()
        digest, _ = self.frozen_identity(self.freeze())
        wrong = ("0" if digest[0] != "0" else "1") + digest[1:]
        self.assertNotEqual(self.verify_snapshot(wrong).returncode, 0)
        payload = self.cache / digest / "table/unit.cnb"
        payload.chmod(0o600)
        payload.write_bytes(payload.read_bytes() + b"corrupt")
        run = self.verify_snapshot(digest)
        self.assertNotEqual(run.returncode, 0, "TABLE_SNAPSHOT_RED changed snapshot accepted")
        self.assertEqual(run.stdout, "")
        self.assertIn("TABLE_VERIFY_REFUSED snapshot", run.stderr)

    def test_snapshot_freeze_copies_bytes_without_semantic_approval(self):
        self.build()
        payload = self.registry / "table/unit.cnb"
        payload.write_bytes(b"not a capsule")
        digest, _ = self.frozen_identity(self.freeze())
        self.assertEqual((self.cache / digest / "table/unit.cnb").read_bytes(), b"not a capsule")
        run = self.verify_snapshot(digest)
        self.assertNotEqual(run.returncode, 0)
        self.assertEqual(run.stdout, "")
        self.assertIn("TABLE_VERIFY_REFUSED candidate", run.stderr)

    def test_snapshot_freeze_unsafe_roots_refuse(self):
        self.build()
        self.registry.chmod(0o755)
        self.assertNotEqual(self.freeze().returncode, 0)
        self.registry.chmod(0o700)
        self.cache.chmod(0o755)
        self.assertNotEqual(self.freeze().returncode, 0)
        self.cache.chmod(0o700)
        alias = self.root / "registry_alias"
        alias.symlink_to(self.registry, target_is_directory=True)
        self.assertNotEqual(self.freeze(source_root=alias).returncode, 0)
        self.assertEqual(list(self.cache.iterdir()), [])

    def test_snapshot_worker_guard_and_arguments_refuse(self):
        self.build()
        digest, _ = self.frozen_identity(self.freeze())
        for invalid in (digest.upper(), digest[:-1], "../" + digest, "x" * 64):
            run = self.verify_snapshot(invalid)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn("worker_arguments", run.stderr)
        run = self.verify_snapshot(digest, parent=str(os.getpid() + 1))
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("worker_sandbox", run.stderr)
        self.assertNotEqual(self.verify_snapshot(digest, cpu="02").returncode, 0)
        self.assertNotEqual(self.verify_snapshot(digest, memory="2049").returncode, 0)
        self.assertEqual(list(self.output.iterdir()), [])

    def test_snapshot_worker_entry_seals_before_snapshot_read(self):
        run = self.verify_snapshot("0" * 64, parent=str(os.getpid() + 1))
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("TABLE_VERIFY_REFUSED worker_sandbox", run.stderr,
                      "TABLE_SNAPSHOT_RED guarded snapshot entry absent")


if __name__ == "__main__":
    unittest.main(verbosity=2)
