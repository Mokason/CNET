"""Offline corpus boundary tests; run with unittest discovery from the repo."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

try:
    import unicode17_case_tables as corpus
except ModuleNotFoundError as error:
    raise ImportError("UNICODE17_CORPUS_RED pinned offline extractor missing") from error


class Unicode17CaseTablesTests(unittest.TestCase):
    def test_explicit_changes_include_non_latin1_outputs_but_not_identity(self):
        for kind, count, samples, omitted in (
            ("upper", 58, {97: 65, 181: 924, 255: 376}, [0, 65, 223]),
            ("lower", 56, {65: 97, 192: 224, 222: 254}, [0, 97, 223, 255]),
        ):
            result = corpus.make_table(corpus.read_source(corpus.DEFAULT_SOURCE), kind)
            lines = result.decode("ascii").splitlines()
            self.assertEqual(lines[:6], ["CNET_LOCAL_TABLE_V1", f"dataset unicode17_{kind}_latin1",
                "authority verified_tool", "input_bits 8", "output_bits 16", f"rows {count}"])
            rows = dict(tuple(map(int, row.split("\t"))) for row in lines[6:])
            self.assertEqual(len(rows), count)
            self.assertEqual(list(rows), sorted(rows))
            for key, value in samples.items():
                self.assertEqual(rows[key], value)
            for key in omitted:
                self.assertNotIn(key, rows)
            self.assertTrue(result.endswith(b"\n"))
            self.assertLessEqual(len(result), 4096)

    def test_modified_truncated_extended_and_newline_changed_sources_refuse(self):
        source = corpus.DEFAULT_SOURCE.read_bytes()
        for bad in (source[:-1], source + b"\n", source.replace(b"0041", b"0042", 1),
                    source.replace(b"\n", b"\r\n"), b"", b"x" * (corpus.MAX_SOURCE_BYTES + 1)):
            with self.subTest(length=len(bad)), self.assertRaisesRegex(ValueError, "unicode17_source_refused"):
                corpus.make_table(bad, "upper")

    def test_unknown_mapping_refuses(self):
        with self.assertRaisesRegex(ValueError, "unicode17_mapping_refused"):
            corpus.make_table(corpus.DEFAULT_SOURCE.read_bytes(), "fold")

    def test_regular_file_limit_and_symlink_refusal(self):
        with tempfile.TemporaryDirectory() as root:
            large = Path(root, "large")
            with large.open("wb") as handle:
                handle.truncate(corpus.MAX_SOURCE_BYTES + 1)
            alias = Path(root, "alias")
            alias.symlink_to(corpus.DEFAULT_SOURCE)
            fifo = Path(root, "fifo")
            os.mkfifo(fifo)
            for path in (large, alias, fifo, Path(root)):
                with self.subTest(path=path.name), self.assertRaises((ValueError, OSError)):
                    corpus.read_source(path)

    def test_committed_tables_reproduce_byte_for_byte(self):
        for kind in ("upper", "lower"):
            expected = corpus.DEFAULT_SOURCE.with_name(f"unicode17_{kind}_latin1.tsv").read_bytes()
            self.assertEqual(expected, corpus.make_table(corpus.DEFAULT_SOURCE.read_bytes(), kind))

    def test_cli_is_offline_and_refuses_invalid_source_without_partial_output(self):
        entry = Path(corpus.__file__)
        good = subprocess.run([sys.executable, str(entry), "upper"], capture_output=True, timeout=5)
        self.assertEqual(good.returncode, 0)
        self.assertEqual(good.stderr, b"")
        self.assertEqual(good.stdout, corpus.make_table(corpus.DEFAULT_SOURCE.read_bytes(), "upper"))
        bad = subprocess.run([sys.executable, str(entry), "upper", "--source", str(entry)],
                             capture_output=True, timeout=5)
        self.assertEqual(bad.returncode, 2)
        self.assertEqual(bad.stdout, b"")
        self.assertEqual(bad.stderr, b"unicode17_source_refused\n")


if __name__ == "__main__":
    unittest.main()
