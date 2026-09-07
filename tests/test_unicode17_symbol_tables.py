"""Independent bounded-source extraction and refusal tests (no network)."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
ENTRY = ROOT / "scripts/unicode17_symbol_tables.py"
RAW = ROOT / "data/unicode17/UnicodeData-Latin1.txt"
sys.dont_write_bytecode = True


class Unicode17SymbolTablesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not ENTRY.is_file():
            raise AssertionError("UNICODE17_SYMBOLS_RED offline symbol extractor missing")
        specification = importlib.util.spec_from_file_location("symbol_corpus", ENTRY)
        cls.corpus = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(cls.corpus)

    def test_literal_labels_exact_sorted_vocabulary_and_header(self):
        raw = RAW.read_bytes()
        fields = [line.split(";") for line in raw.decode("ascii").splitlines()]
        for dataset, field in (("ascii_category", 2), ("ascii_bidi", 4)):
            source = self.corpus.make_table(raw, dataset)
            lines = source.decode("ascii").splitlines()
            self.assertEqual(lines[:6], ["CNET_LOCAL_SYMBOLS_V1", "dataset " + dataset,
                "authority verified_tool", "input_bits 8", "output_bits 16", "rows 95"])
            expected = sorted((row[1].replace(" ", "_"), row[field]) for row in fields[32:127])
            self.assertEqual(lines[6:], [token + "\t" + label for token, label in expected])
            self.assertEqual(len(dict(expected)), 95)
            self.assertLessEqual(len(source), 4096)
            self.assertTrue(source.endswith(b"\n"))
            values = dict(expected)
            self.assertEqual(values["LATIN_CAPITAL_LETTER_A"], "Lu" if field == 2 else "L")
            self.assertEqual(values["DIGIT_ZERO"], "Nd" if field == 2 else "EN")
            self.assertEqual(values["SPACE"], "Zs" if field == 2 else "WS")
            self.assertEqual(values["HYPHEN-MINUS"], "Pd" if field == 2 else "ES")
            self.assertNotIn("HYPHEN_MINUS", values)
            self.assertNotIn("DELETE", values)
            self.assertNotIn("LATIN_SMALL_LETTER_Y_WITH_DIAERESIS", values)

    def test_vocabulary_hash_excludes_labels_and_provenance_binds_sources(self):
        raw = RAW.read_bytes()
        provenance = json.loads(self.corpus.make_provenance(raw))
        self.assertEqual(provenance["raw_sha256"], "75dfecc13fe9b1202e3f7c787e4e7f2c848c97c8b092dd75b4f6a2b99990cdc4")
        vocabularies = []
        for dataset in ("ascii_category", "ascii_bidi"):
            source = self.corpus.make_table(raw, dataset)
            vocabulary = b"".join(line.split(b"\t")[0] + b"\n" for line in source.splitlines()[6:])
            entry = provenance["datasets"][dataset]
            self.assertEqual(entry["source_sha256"], hashlib.sha256(source).hexdigest())
            self.assertEqual(entry["vocabulary_sha256"], hashlib.sha256(vocabulary).hexdigest())
            self.assertEqual(entry["rows"], 95)
            self.assertEqual(entry["bytes"], len(source))
            vocabularies.append(vocabulary)
        self.assertEqual(vocabularies[0], vocabularies[1])

    def test_modified_oversized_full_and_unknown_sources_refuse(self):
        raw = RAW.read_bytes()
        for bad in (b"", raw[:-1], raw + b"\n", raw.replace(b"\n", b"\r\n"),
                    raw.replace(b"0041", b"0042", 1), b"x" * (self.corpus.MAX_SOURCE_BYTES + 1)):
            with self.subTest(length=len(bad)), self.assertRaisesRegex(ValueError, "unicode17_symbol_source_refused"):
                self.corpus.make_table(bad, "ascii_category")
        with self.assertRaisesRegex(ValueError, "unicode17_symbol_dataset_refused"):
            self.corpus.make_table(raw, "unknown")

    def test_file_boundary_and_cli_never_emit_partial_refused_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "alias").symlink_to(RAW)
            os.mkfifo(root / "fifo")
            with (root / "large").open("wb") as output:
                output.truncate(self.corpus.MAX_SOURCE_BYTES + 1)
            for path in (root / "alias", root / "fifo", root / "large", root):
                with self.subTest(path=path), self.assertRaises((ValueError, OSError)):
                    self.corpus.read_source(path)
            bad = subprocess.run([sys.executable, str(ENTRY), "ascii_category", "--source", str(ENTRY)],
                                 capture_output=True, timeout=5)
            self.assertEqual(bad.returncode, 2)
            self.assertEqual(bad.stdout, b"")
            self.assertEqual(bad.stderr, b"unicode17_symbol_source_refused\n")

    def test_committed_artifacts_and_cli_reproduce_exactly(self):
        for dataset in ("ascii_category", "ascii_bidi", "provenance"):
            result = subprocess.run([sys.executable, str(ENTRY), dataset], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stderr, b"")
            name = "ascii_symbol_tables.provenance.json" if dataset == "provenance" else dataset + ".symbols.tsv"
            self.assertEqual(result.stdout, RAW.with_name(name).read_bytes())


if __name__ == "__main__":
    unittest.main(verbosity=2)
