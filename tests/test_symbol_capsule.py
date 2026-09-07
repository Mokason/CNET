#!/usr/bin/env python3
"""Literal symbolic labels through the existing certified table capsule path."""
import ctypes
from contextlib import contextmanager
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import test_capsule_daemon_lifecycle as lifecycle

REPO = Path(__file__).resolve().parents[1]
TOOL = Path(os.environ.get("CNET_TABLE_TEST_BIN", REPO / "bin/cnet_table_capsule"))
VERIFY = Path(os.environ.get("CNET_TABLE_VERIFY_TEST_BIN", REPO / "bin/cnet_table_verify"))
LIBRARY = Path(os.environ.get("CNET_TEST_CORE_LIBRARY", REPO / "bin/libcnet_capsule_core.so"))
SNAPSHOT = Path(os.environ.get("CNET_LEARNING_SNAPSHOT_TEST_BIN", REPO / "bin/cnet_learning_snapshot"))
DATASET = "unicode_categories"


def source(rows, magic="CNET_LOCAL_SYMBOLS_V1"):
    return (f"{magic}\ndataset {DATASET}\nauthority verified_tool\n"
            f"input_bits 8\noutput_bits 16\nrows {len(rows)}\n" +
            "".join(f"{key}\t{label}\n" for key, label in rows)).encode("utf-8")


class Reply(ctypes.Structure):
    _fields_ = [("verified", ctypes.c_int), ("value", ctypes.c_uint),
                ("hops", ctypes.c_size_t), ("units", ctypes.c_char * 512),
                ("reason", ctypes.c_char * 160)]


class SymbolCapsule(unittest.TestCase):
    def build(self, data, out):
        return subprocess.run([str(TOOL), "build", str(data), DATASET, str(out)],
                              cwd=REPO, capture_output=True, text=True, timeout=30)

    @contextmanager
    def fixture(self, rows, magic="CNET_LOCAL_SYMBOLS_V1"):
        with tempfile.TemporaryDirectory(prefix="cnet-symbol-") as private:
            root = Path(private)
            data, inventory = root / "data", root / "inventory"
            data.mkdir(mode=0o700)
            inventory.mkdir(mode=0o700)
            raw = source(rows, magic)
            path = data / f"{DATASET}.tsv"
            path.write_bytes(raw)
            path.chmod(0o600)
            result = self.build(data, inventory / "version1")
            self.assertEqual(result.returncode, 0, "SYMBOL_CAPSULE_RED " + result.stderr)
            previous = os.environ.get("CNET_CAPSULE_DATA_ROOT")
            os.environ["CNET_CAPSULE_DATA_ROOT"] = str(data)
            try:
                yield root, data, inventory, path, raw
            finally:
                # The existing snapshot API intentionally makes cache copies read-only.
                for current, _, _ in os.walk(root):
                    os.chmod(current, 0o700)
                if previous is None:
                    os.environ.pop("CNET_CAPSULE_DATA_ROOT", None)
                else:
                    os.environ["CNET_CAPSULE_DATA_ROOT"] = previous

    @contextmanager
    def resident(self, inventory):
        lib = ctypes.CDLL(str(LIBRARY))
        lib.cnet_core_host_open.argtypes = [ctypes.c_char_p]
        lib.cnet_core_host_open.restype = ctypes.c_void_p
        lib.cnet_core_host_close.argtypes = [ctypes.c_void_p]
        lib.cnet_core_host_close.restype = ctypes.c_int
        lib.cnet_core_host_pin.argtypes = [ctypes.c_void_p]
        lib.cnet_core_host_pin.restype = ctypes.c_void_p
        lib.cnet_core_host_unpin.argtypes = [ctypes.c_void_p]
        lib.cnet_core_host_unpin.restype = None
        lib.cnet_core_host_ask_text.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
            ctypes.POINTER(Reply), ctypes.c_char_p, ctypes.c_size_t]
        lib.cnet_core_host_ask_text.restype = ctypes.c_int
        lib.cnet_core_host_stage_registry.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
            ctypes.c_int, ctypes.POINTER(ctypes.c_uint64)]
        lib.cnet_core_host_activate.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        host = lib.cnet_core_host_open(os.fsencode(inventory))
        self.assertTrue(host, "SYMBOL_IMPORT_RED canonical asset refused")

        def ask(request, label, ordinal=None, capacity=129):
            lease = lib.cnet_core_host_pin(host)
            self.assertTrue(lease)
            reply, text = Reply(), ctypes.create_string_buffer(capacity)
            try:
                rc = lib.cnet_core_host_ask_text(lease, request.encode("utf-8"),
                                                ctypes.byref(reply), text, capacity)
            finally:
                lib.cnet_core_host_unpin(lease)
            if label is None:
                self.assertNotEqual(rc, 0, "SYMBOL_REFUSAL_RED " + request)
                self.assertEqual((reply.verified, reply.value, text.value), (0, 0, b""))
            else:
                self.assertEqual(rc, 0, "SYMBOL_ALIAS_RED " + reply.reason.decode())
                self.assertEqual((reply.verified, reply.value, text.value),
                                 (1, ordinal, label.encode("ascii")))
                self.assertEqual(reply.hops, 1, "SYMBOL_EXECUTION_RED missing certified hop")
            return reply

        try:
            yield lib, host, ask
        finally:
            self.assertEqual(lib.cnet_core_host_close(host), 0)

    def test_literal_labels_numeric_compatibility_and_terminal_refusal(self):
        rows = [("A", ' ABSTAIN: "quoted"\\literal '), ("Z", "Lu"),
                ("a.b:c-d_0", "lower")]
        with self.fixture(rows) as (_, _, inventory, _, raw):
            self.assertEqual((inventory / "version1/frontend.cvfa").read_bytes(), raw)
            digest = hashlib.sha256(raw).hexdigest()
            with self.resident(inventory) as (_, _, ask):
                for ordinal, (token, label) in enumerate(rows):
                    reply = ask(f"symbol {DATASET} {token}", label, ordinal)
                    self.assertEqual(reply.units.decode(), "table_" + digest[:56])
                    ask(f"data {DATASET} {ordinal}", str(ordinal), ordinal)
                    ask(f"capsule data_{digest[:22]}_key data_{digest[:22]}_val {ordinal}",
                        str(ordinal), ordinal)
                for key in range(len(rows), 256):
                    ask(f"data {DATASET} {key}", None)
                for request in ("symbol", f"symbol {DATASET}", f"symbol {DATASET} UNKNOWN",
                    f"symbol {DATASET} a", f"symbol {DATASET} 0", f"symbol {DATASET} A extra",
                    f"symbol {DATASET} A\n", f"symbol  {DATASET} A", f"symbol\t{DATASET} A",
                    f"symbol {DATASET} A/", f"symbol {DATASET} Ä", f"symbol ../{DATASET} A"):
                    ask(request, None)
                ask(f"symbol {DATASET} A", None, capacity=len(rows[0][1]))

    def test_label_only_replacement_requires_fresh_activation(self):
        with self.fixture([("A", "Lu"), ("a", "Ll")]) as (_, data, inventory, path, _):
            with self.resident(inventory) as (lib, host, ask):
                ask(f"symbol {DATASET} A", "Lu", 0)
                path.write_bytes(source([("A", "UPPER"), ("a", "LOWER")]))
                ask(f"symbol {DATASET} A", None)
                result = self.build(data, inventory / "version2")
                self.assertEqual(result.returncode, 0, result.stderr)
                ask(f"symbol {DATASET} A", None)
                generation = ctypes.c_uint64()
                self.assertEqual(lib.cnet_core_host_stage_registry(host, os.fsencode(inventory), 1,
                    ctypes.byref(generation)), 0, "SYMBOL_REPLACEMENT_RED stage refused")
                self.assertEqual(lib.cnet_core_host_activate(host, generation.value), 0)
                ask(f"symbol {DATASET} A", "UPPER", 0)
                ask(f"symbol {DATASET} a", "LOWER", 1)
                path.unlink()
                ask(f"symbol {DATASET} A", None)

    def test_numeric_source_does_not_expose_symbol_decoder(self):
        with self.fixture([(0, 42)], "CNET_LOCAL_TABLE_V1") as (_, _, inventory, _, _):
            with self.resident(inventory) as (_, _, ask):
                ask(f"data {DATASET} 0", "42", 42)
                ask(f"symbol {DATASET} 0", None)

    def test_source_boundaries_refuse(self):
        valid = source([("A", "Lu"), ("a", "Ll")])
        malformed = [source([]), source([("a", "Ll"), ("A", "Lu")]),
            source([("A", "Lu"), ("A", "Ll")]), source([("", "Lu")]),
            source([("AA", "Lu"), ("A", "Ll")]),
            source([("x" * 49, "Lu")]), source([("A", "")]), source([("A", "x" * 129)]),
            source([("A/B", "Lu")]), source([("A B", "Lu")]), source([("Ä", "Lu")]),
            source([("A", "Lü")]), source([("A", "\x1f")]), source([("A", "\x7f")]),
            source([("A", "Lu\tLl")]), source([("A", "Lu\nLl")]),
            source([(f"k{i:03}", "L") for i in range(257)]),
            valid.replace(b"rows 2", b"rows 02"), valid.replace(b"rows 2", b"rows 3"),
            valid.replace(b"input_bits 8", b"input_bits 9"), valid.replace(b"output_bits 16", b"output_bits 8"),
            valid.replace(b"verified_tool", b"LOCAL"), valid + b"\n", valid[:-1],
            valid + b"\x00", b"x" * 4097]
        with tempfile.TemporaryDirectory(prefix="cnet-symbol-refusal-") as private:
            root = Path(private)
            path = root / f"{DATASET}.tsv"
            for i, raw in enumerate(malformed):
                with self.subTest(case=i):
                    path.write_bytes(raw)
                    path.chmod(0o600)
                    output = root / f"invalid_{i}"
                    self.assertNotEqual(self.build(root, output).returncode, 0,
                                        "SYMBOL_SOURCE_RED malformed source admitted")
                    self.assertFalse(output.exists())

    def test_maximum_key_label_and_256_row_domain(self):
        rows = [(f"k{i:03}", str(i)) for i in range(256)]
        with self.fixture(rows) as (_, _, inventory, _, _):
            with self.resident(inventory) as (_, _, ask):
                for ordinal, (token, label) in enumerate(rows):
                    ask(f"symbol {DATASET} {token}", label, ordinal)
                ask(f"symbol {DATASET} cnet_unknown_0", None)
        with self.fixture([("x" * 48, " " * 128)]) as (_, _, inventory, _, _):
            with self.resident(inventory) as (_, _, ask):
                ask(f"symbol {DATASET} " + "x" * 48, " " * 128, 0)

    def test_source_byte_limit_is_exact(self):
        rows = [(f"k{i:03}", "x") for i in range(32)]
        remaining = 4096 - len(source(rows))
        for i, (token, label) in enumerate(rows):
            extra = min(127, remaining)
            rows[i] = (token, label + "x" * extra)
            remaining -= extra
        self.assertEqual(remaining, 0)
        with self.fixture(rows) as (root, data, _, path, raw):
            self.assertEqual(len(raw), 4096)
            # Extend a non-full label; the resulting format remains canonical.
            index = next(i for i, (_, label) in enumerate(rows) if len(label) < 128)
            rows[index] = (rows[index][0], rows[index][1] + "x")
            path.write_bytes(source(rows))
            self.assertEqual(path.stat().st_size, 4097)
            self.assertNotEqual(self.build(data, root / "over_limit").returncode, 0,
                                "SYMBOL_SIZE_RED 4097-byte source admitted")
            self.assertFalse((root / "over_limit").exists())

    def test_worker_receipt_observes_actual_literal_text_and_absent_token(self):
        rows = [("A", '"quote"\\literal '), ("cnet_unknown_0", "-")]
        with self.fixture(rows) as (root, data, inventory, _, raw):
            worker = root / "worker"
            worker.mkdir(mode=0o700)
            result = subprocess.run([str(VERIFY), "verify-worker", str(data), DATASET,
                str(inventory), str(worker), str(os.getpid()), "10", "128"],
                cwd=REPO, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=35)
            self.assertEqual(result.returncode, 0, "SYMBOL_RECEIPT_RED " + result.stderr)
            expected = ("CNET_SYMBOL_NATIVE_EVAL_V1\n" + f"dataset {DATASET}\n" +
                "source_sha256 " + hashlib.sha256(raw).hexdigest() + "\nresults 256\n" +
                "".join(f"{i}\t1\t{i}\n" if i < len(rows) else f"{i}\t0\t-\n" for i in range(256)) +
                f"symbols {len(rows)}\n" + "".join(f"{key}\t1\t{label}\n" for key, label in rows) +
                "unknown cnet_unknown_1\t0\t-\nend\n")
            self.assertEqual(result.stdout, expected, "SYMBOL_RECEIPT_RED text boundary differs")
            self.assertLess(len(result.stdout.encode()), 16384)
            cache = root / "cache"
            cache.mkdir(mode=0o700)
            frozen = subprocess.run([str(SNAPSHOT), "freeze", str(inventory), str(cache)],
                stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=35)
            self.assertEqual(frozen.returncode, 0, frozen.stderr)
            digest = frozen.stdout.splitlines()[1].removeprefix("snapshot_sha256 ")
            self.assertRegex(digest, r"^[a-f0-9]{64}$")
            staged = subprocess.run([str(VERIFY), "snapshot-worker", str(data), DATASET,
                str(cache), digest, str(worker), str(os.getpid()), "10", "128"],
                cwd=REPO, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=35)
            self.assertEqual(staged.returncode, 0, "SYMBOL_SNAPSHOT_RED " + staged.stderr)
            self.assertEqual(staged.stdout, expected.replace("CNET_SYMBOL_NATIVE_EVAL_V1\n",
                f"CNET_SYMBOL_SNAPSHOT_EVAL_V1\nsnapshot_sha256 {digest}\n", 1))

    def test_daemon_symbol_is_terminal_and_json_preserves_literal_label(self):
        # Reuse its private process/socket fixture without inheriting unrelated tests.
        daemon = lifecycle.ResidentDaemon(methodName="test_chat_is_not_control")
        try:
            daemon.setUp()
            daemon.stop()
            data, demand = daemon.root / "data", daemon.root / "demand"
            data.mkdir(mode=0o700)
            demand.mkdir(mode=0o700)
            label = ' ABSTAIN: "quoted"\\literal '
            path = data / f"{DATASET}.tsv"
            path.write_bytes(source([("A", label)]))
            path.chmod(0o600)
            result = self.build(data, daemon.root / "sets/original/symbol_v1")
            self.assertEqual(result.returncode, 0, result.stderr)
            daemon.env["CNET_CAPSULE_DATA_ROOT"] = str(data)
            daemon.env["CNET_CAPSULE_DEMAND_DIR"] = str(demand)
            daemon.start()
            for query in (f"symbol {DATASET} A", f"symbol {DATASET} UNKNOWN", "symbol"):
                reply = daemon.ask(query)
                self.assertFalse(reply["verified"])
                self.assertFalse(reply["teacher"])
                self.assertEqual(reply["skill"], "capsule_refusal", "SYMBOL_TERMINAL_RED residual route")
            staged = daemon.stage(1, 1, "original")
            self.assertTrue(daemon.command(f"ACTIVATE 1 symbols {staged}").startswith("OK "))
            reply = daemon.ask(f"symbol {DATASET} A")
            self.assertTrue(reply["verified"])
            self.assertFalse(reply["teacher"])
            self.assertEqual(reply["answer"], label, "SYMBOL_JSON_RED literal label changed")
            self.assertEqual(daemon.ask(f"data {DATASET} 0")["answer"], "0")
            reply = daemon.ask(f"symbol {DATASET} UNKNOWN")
            self.assertFalse(reply["verified"])
            self.assertFalse(reply["teacher"])
            self.assertEqual(reply["skill"], "capsule_refusal")
            self.assertEqual(list(demand.iterdir()), [], "SYMBOL_DEMAND_RED native alias wrote demand")
        finally:
            daemon.doCleanups()


if __name__ == "__main__":
    unittest.main(verbosity=2)
