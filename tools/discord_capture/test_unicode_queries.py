"""Finite external-source fixtures only; never live captured demand."""
import hashlib
from pathlib import Path
import tempfile
import unittest

import unicode_queries as uq

SOURCE = Path(__file__).resolve().parents[2] / "data/unicode17/UnicodeData-Latin1.txt"


class UnicodeQueryTests(unittest.TestCase):
    def test_closed_typed_interface(self):
        self.assertEqual(uq.parse("unicode upper 181"), ("unicode17_upper_latin1", 181))
        self.assertEqual(uq.parse("unicode lower 65"), ("unicode17_lower_latin1", 65))
        self.assertEqual(uq.parse("unicode category LATIN_CAPITAL_LETTER_A"),
                         ("ascii_category", "LATIN_CAPITAL_LETTER_A"))
        self.assertEqual(uq.parse("unicode bidi HYPHEN-MINUS"), ("ascii_bidi", "HYPHEN-MINUS"))

    def test_malformed_recognized_queries_refuse_and_unrelated_queries_do_not_match(self):
        for text in ("unicode upper 256", "unicode upper -1", "unicode upper 01",
                     "unicode upper 1\n", "unicode upper 1; pause", "unicode upper 1 extra",
                     "unicode import /tmp/data", "unicode bidi A\0", "unicode upper １",
                     "unicode category " + "A" * 97, "unicode", "unicode\tupper 1"):
            with self.subTest(text=text), self.assertRaises(uq.QueryError):
                uq.parse(text)
        for text in ("what can you do", "explain unicode", "Unicode upper 97", "unicodex upper 97"):
            self.assertIsNone(uq.parse(text))

    def test_all_external_fields_and_explicit_abstentions(self):
        reference = uq.Reference(SOURCE)
        counts = {name: 0 for name in uq.DATASETS}
        # Read the external fields independently of the query module.
        for row in SOURCE.read_text().splitlines():
            fields = row.split(";")
            key = int(fields[0], 16)
            for name, column in (("unicode17_upper_latin1", 12), ("unicode17_lower_latin1", 13)):
                expected = int(fields[column], 16) if fields[column] else None
                self.assertEqual(reference.expected((name, key)), expected)
                counts[name] += expected is not None
            if 32 <= key <= 126:
                token = fields[1].replace(" ", "_")
                for name, column in (("ascii_category", 2), ("ascii_bidi", 4)):
                    self.assertEqual(reference.expected((name, token)), fields[column])
                    counts[name] += 1
        self.assertEqual(list(counts.values()), [58, 56, 95, 95])
        self.assertIsNone(reference.expected(("ascii_category", "A")))
        self.assertEqual(reference.expected(("unicode17_upper_latin1", 255)), 376)

    def test_source_identity_and_file_types_fail_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "source"
            path.write_bytes(SOURCE.read_bytes()[:-1] + b"x")
            with self.assertRaises(uq.QueryError):
                uq.Reference(path)
            path.unlink()
            path.symlink_to(SOURCE)
            with self.assertRaises((uq.QueryError, OSError)):
                uq.Reference(path)

    def test_source_drift_is_detected_after_construction(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "source"
            path.write_bytes(SOURCE.read_bytes())
            reference = uq.Reference(path)
            path.write_bytes(b"changed")
            with self.assertRaises(uq.QueryError):
                reference.verify()

    def test_source_pin_and_symbol_vocabulary_match_existing_contract(self):
        reference = uq.Reference(SOURCE)
        self.assertEqual(reference.raw_sha256, hashlib.sha256(SOURCE.read_bytes()).hexdigest())
        self.assertEqual(reference.vocabulary_sha256,
                         "ba3fe8b2440a6065c04e714136c1ff742e7e7ab772902ff4558fe3b2699b6984")
        self.assertEqual(reference.ordinal(("ascii_category", "SPACE")), 92)


if __name__ == "__main__":
    unittest.main()
