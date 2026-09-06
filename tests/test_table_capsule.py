#!/usr/bin/env python3
"""Bounded owner-table acquisition and resident refresh; no teacher/service."""
import ctypes
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
TOOL = Path(os.environ.get("CNET_TABLE_TEST_BIN", REPO / "bin/cnet_table_capsule"))
LIBRARY = Path(os.environ.get("CNET_TEST_CORE_LIBRARY", REPO / "bin/libcnet_capsule_core.so"))


def source(rows, dataset="stock_levels", authority="user_correction"):
    return ("CNET_LOCAL_TABLE_V1\n" + f"dataset {dataset}\n"
            f"authority {authority}\ninput_bits 8\noutput_bits 16\n"
            f"rows {len(rows)}\n" + "".join(f"{x}\t{y}\n" for x, y in rows)).encode()


class TableCapsule(unittest.TestCase):
    def build(self, root, dataset, out):
        self.assertTrue(TOOL.is_file(), "TABLE_CAPSULE_RED native table producer absent")
        return subprocess.run([str(TOOL), "build", str(root), dataset, str(out)],
                              cwd=REPO, capture_output=True, text=True, timeout=30)

    def test_verified_tool_source_keeps_its_authority(self):
        with tempfile.TemporaryDirectory(prefix="cnet-table-authority-") as private:
            root = Path(private)
            raw = source([(0, 120)], authority="verified_tool")
            path = root / "stock_levels.tsv"
            path.write_bytes(raw)
            path.chmod(0o600)
            run = self.build(root, "stock_levels", root / "capsule")
            self.assertEqual(run.returncode, 0, "TABLE_AUTHORITY_RED approved verified-tool source refused")
            self.assertIn("authority=verified_tool", run.stdout)
            self.assertEqual((root / "capsule/frontend.cvfa").read_bytes(), raw)

    def test_worker_build_uses_private_output(self):
        with tempfile.TemporaryDirectory(prefix="cnet-table-worker-") as private:
            root = Path(private)
            data, output = root / "data", root / "output"
            data.mkdir(mode=0o700)
            output.mkdir(mode=0o700)
            path = data / "stock_levels.tsv"
            path.write_bytes(source([(0, 120)], authority="verified_tool"))
            path.chmod(0o600)
            run = subprocess.run([str(TOOL), "build-worker", str(data), "stock_levels", str(output),
                                  str(os.getpid()), "2", "64"],
                                 cwd=REPO, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=35)
            self.assertEqual(run.returncode, 0, "TABLE_WORKER_RED " + run.stderr)
            self.assertEqual({p.name for p in output.iterdir()}, {"capsule"})
            self.assertEqual({p.name for p in (output / "capsule").iterdir()},
                             {"unit.cnb", "manifest.cknow", "frontend.cvfa"})
            self.assertIn("pending_activation=1", run.stdout)

    def test_worker_parent_and_policy_arguments_refuse_before_source_processing(self):
        with tempfile.TemporaryDirectory(prefix="cnet-table-worker-arguments-") as private:
            root = Path(private)
            data, output = root / "data", root / "output"
            data.mkdir(mode=0o700)
            output.mkdir(mode=0o700)
            (data / "stock_levels.tsv").write_bytes(source([(0, 120)]))
            (data / "stock_levels.tsv").chmod(0o600)
            parent = str(os.getpid())
            valid = [parent, "2", "64"]
            malformed = [
                [], [parent], [parent, "2"],
                ["0", "2", "64"], ["1", "2", "64"], ["0" + parent, "2", "64"],
                ["2147483648", "2", "64"], ["-1", "2", "64"], ["+" + parent, "2", "64"],
                [parent, "0", "64"], [parent, "121", "64"], [parent, "02", "64"],
                [parent, "+2", "64"], [parent, "2.0", "64"], [parent, "", "64"],
                [parent, "4294967296", "64"], [parent, "2", "63"], [parent, "2", "2049"],
                [parent, "2", "064"], [parent, "2", "64.0"], [parent, "2", "-1"],
                [parent, "2", "64\n"], valid + ["extra"],
            ]
            for arguments in malformed:
                with self.subTest(arguments=arguments):
                    run = subprocess.run([str(TOOL), "build-worker", str(data), "stock_levels", str(output), *arguments],
                                         cwd=REPO, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=5)
                    self.assertNotEqual(run.returncode, 0, "TABLE_WORKER_ARGUMENTS_RED malformed authority accepted")
                    self.assertTrue("worker_arguments" in run.stderr or "usage:" in run.stderr, run.stderr)
                    self.assertEqual(list(output.iterdir()), [])
            run = subprocess.run([str(TOOL), "build-worker", str(data), "stock_levels", str(output),
                                  str(os.getpid() + 1), "2", "64"],
                                 cwd=REPO, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=5)
            self.assertNotEqual(run.returncode, 0, "TABLE_WORKER_PARENT_RED mismatched parent accepted")
            self.assertIn("worker_sandbox", run.stderr)
            self.assertEqual(list(output.iterdir()), [])

    def test_owner_table_build_and_exact_asset(self):
        with tempfile.TemporaryDirectory(prefix="cnet-table-") as private:
            root = Path(private)
            data = root / "data"
            data.mkdir(mode=0o700)
            raw = source([(0, 120), (7, 42), (19, 0)])
            (data / "stock_levels.tsv").write_bytes(raw)
            (data / "stock_levels.tsv").chmod(0o600)
            output = root / "capsule"
            run = self.build(data, "stock_levels", output)
            self.assertEqual(run.returncode, 0, "TABLE_CAPSULE_RED " + run.stderr)
            self.assertIn("TABLE_CAPSULE_PASS", run.stdout)
            self.assertIn("source_sha256=" + hashlib.sha256(raw).hexdigest(), run.stdout)
            self.assertEqual({p.name for p in output.iterdir()},
                             {"unit.cnb", "manifest.cknow", "frontend.cvfa"})
            self.assertEqual((output / "frontend.cvfa").read_bytes(), raw)
            before = {p.name: p.read_bytes() for p in output.iterdir()}
            self.assertNotEqual(self.build(data, "stock_levels", output).returncode, 0)
            self.assertEqual(before, {p.name: p.read_bytes() for p in output.iterdir()})

    def test_resident_alias_and_refresh(self):
        class Reply(ctypes.Structure):
            _fields_ = [("verified", ctypes.c_int), ("value", ctypes.c_uint),
                        ("hops", ctypes.c_size_t), ("units", ctypes.c_char * 512),
                        ("reason", ctypes.c_char * 160)]
        self.assertTrue(LIBRARY.is_file(), "TABLE_ALIAS_RED runtime absent")
        lib = ctypes.CDLL(str(LIBRARY))
        lib.cnet_core_host_open.argtypes = [ctypes.c_char_p]
        lib.cnet_core_host_open.restype = ctypes.c_void_p
        lib.cnet_core_host_pin.argtypes = [ctypes.c_void_p]
        lib.cnet_core_host_pin.restype = ctypes.c_void_p
        lib.cnet_core_host_unpin.argtypes = [ctypes.c_void_p]
        lib.cnet_core_host_close.argtypes = [ctypes.c_void_p]
        lib.cnet_core_host_ask.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(Reply)]
        lib.cnet_core_host_stage_registry.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
                                                     ctypes.POINTER(ctypes.c_uint64)]
        lib.cnet_core_host_activate.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        with tempfile.TemporaryDirectory(prefix="cnet-table-host-") as private:
            root = Path(private)
            data, inventory = root / "data", root / "inventory"
            data.mkdir(mode=0o700)
            inventory.mkdir(mode=0o700)
            original = source([(0, 120), (7, 42), (19, 0)])
            path = data / "stock_levels.tsv"
            path.write_bytes(original)
            path.chmod(0o600)
            self.assertEqual(self.build(data, "stock_levels", inventory / "version1").returncode, 0)
            previous = os.environ.get("CNET_CAPSULE_DATA_ROOT")
            os.environ["CNET_CAPSULE_DATA_ROOT"] = str(data)
            host = lib.cnet_core_host_open(os.fsencode(inventory))
            self.assertTrue(host, "TABLE_ALIAS_RED table capsule cannot load")

            def ask(text, expected):
                lease = lib.cnet_core_host_pin(host)
                self.assertTrue(lease)
                reply = Reply()
                try:
                    rc = lib.cnet_core_host_ask(lease, text.encode(), ctypes.byref(reply))
                finally:
                    lib.cnet_core_host_unpin(lease)
                if expected is None:
                    self.assertNotEqual(rc, 0, "TABLE_ALIAS_RED OOD/stale query answered")
                    self.assertEqual(reply.verified, 0)
                else:
                    self.assertEqual(rc, 0, "TABLE_ALIAS_RED " + reply.reason.decode())
                    self.assertEqual((reply.verified, reply.value), (1, expected))
            try:
                for key in range(256):
                    ask(f"data stock_levels {key}", {0: 120, 7: 42, 19: 0}.get(key))
                for bad in ("data ../stock_levels 7", "data stock_levels 07", "data stock_levels 256",
                            "data stock_levels 7 extra", "data stock_levels -1", "data stock_levels 7\n"):
                    ask(bad, None)
                path.write_bytes(source([(0, 120), (7, 43), (19, 0)]))
                ask("data stock_levels 7", None)
                self.assertEqual(self.build(data, "stock_levels", inventory / "version2").returncode, 0)
                ask("data stock_levels 7", None)  # Unactivated files do not alter the resident view.
                generation = ctypes.c_uint64()
                self.assertEqual(lib.cnet_core_host_stage_registry(host, os.fsencode(inventory), 1,
                                 ctypes.byref(generation)), 0, "TABLE_ALIAS_RED retained-history refresh refused")
                self.assertEqual(lib.cnet_core_host_activate(host, generation.value), 0)
                ask("data stock_levels 7", 43)
                old = hashlib.sha256(original).hexdigest()[:22]
                ask(f"capsule data_{old}_key data_{old}_val 7", None)
            finally:
                self.assertEqual(lib.cnet_core_host_close(host), 0)
                if previous is None:
                    os.environ.pop("CNET_CAPSULE_DATA_ROOT", None)
                else:
                    os.environ["CNET_CAPSULE_DATA_ROOT"] = previous

    def test_source_boundary_refusals(self):
        with tempfile.TemporaryDirectory(prefix="cnet-table-boundary-") as private:
            root = Path(private)
            data = root / "data"
            data.mkdir(mode=0o700)
            path = data / "stock_levels.tsv"
            valid = source([(0, 120), (7, 42)])
            malformed = [
                source([]), source([(7, 42), (0, 120)]), source([(0, 1), (0, 2)]),
                source([(256, 42)]), source([(0, 65536)]), source([(-1, 42)]),
                valid.replace(b"0\t120", b"00\t120"), valid.replace(b"7\t42", b"7 42"),
                valid.replace(b"user_correction", b"LOCAL"),
                valid.replace(b"input_bits 8", b"input_bits 9"),
                valid.replace(b"output_bits 16", b"output_bits 15"),
                valid.replace(b"rows 2", b"rows 3"), valid + b"\n", valid[:-1],
                valid + b"\x00", b"x" * 4097, source([(0, 1)], "other_dataset"),
            ]
            for i, raw in enumerate(malformed):
                with self.subTest(case=i):
                    path.write_bytes(raw)
                    path.chmod(0o600)
                    out = root / f"invalid_{i}"
                    run = self.build(data, "stock_levels", out)
                    self.assertNotEqual(run.returncode, 0, "TABLE_SOURCE_RED malformed source accepted")
                    self.assertFalse(out.exists())
            path.write_bytes(valid)
            for mode in (0o644, 0o660, 0o700):
                path.chmod(mode)
                self.assertNotEqual(self.build(data, "stock_levels", root / "unsafe_mode").returncode, 0)
            path.chmod(0o600)
            for dataset in ("../stock_levels", "Stock_levels", "stock-levels", "a" * 32, ""):
                self.assertNotEqual(self.build(data, dataset, root / "unsafe_name").returncode, 0)
            linked = data / "linked.tsv"
            os.link(path, linked)
            self.assertNotEqual(self.build(data, "stock_levels", root / "hardlink").returncode, 0)
            linked.unlink()
            path.unlink()
            path.symlink_to(root / "missing")
            self.assertNotEqual(self.build(data, "stock_levels", root / "symlink").returncode, 0)
            path.unlink()
            os.mkfifo(path, 0o600)
            self.assertNotEqual(self.build(data, "stock_levels", root / "fifo").returncode, 0)
            path.unlink()
            path.write_bytes(valid)
            path.chmod(0o600)
            data.chmod(0o755)
            self.assertNotEqual(self.build(data, "stock_levels", root / "public_root").returncode, 0)
            data.chmod(0o700)
            alias = root / "alias"
            alias.symlink_to(data, target_is_directory=True)
            self.assertNotEqual(self.build(alias, "stock_levels", root / "root_link").returncode, 0)
            self.assertNotEqual(self.build(data, "stock_levels", alias / "output").returncode, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
