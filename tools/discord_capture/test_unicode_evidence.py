"""External Unicode label extension; fixtures never qualify as real demand."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock

from evidence import Mapper, digest
from unicode_evidence import UnicodeMapper
from unicode_queries import QueryError

SOURCE = Path(__file__).resolve().parents[2] / "data/unicode17/UnicodeData-Latin1.txt"


class UnicodeEvidenceTests(unittest.TestCase):
    def mapper(self):
        legacy = Mapper(lambda text: "capsule bytes bits 3" if text == "legacy" else None,
                        lambda *args: b"24\n", {"fixture": "a" * 64})
        return UnicodeMapper(legacy, SOURCE), legacy

    def test_four_queries_have_independent_bound_receipts(self):
        mapper, _ = self.mapper()
        for text, expected in (("unicode upper 181", 924), ("unicode lower 65", 97),
                               ("unicode category DIGIT_ZERO", "Nd"), ("unicode bidi DIGIT_ZERO", "EN")):
            row = mapper.label(text, "b" * 64)
            self.assertEqual(row["status"], "verified_tool")
            self.assertEqual(row["expected"], expected)
            self.assertFalse(row["certified_cnet_answer"])
            self.assertFalse(row["expected_abstention"])
            receipt = row.pop("receipt_sha256")
            self.assertEqual(receipt, digest(row))
            self.assertEqual(row["request_sha256"], "b" * 64)

    def test_external_abstention_is_not_a_generated_training_value(self):
        mapper, _ = self.mapper()
        for text in ("unicode upper 65", "unicode category A"):
            row = mapper.label(text, "b" * 64)
            self.assertEqual(row["status"], "verified_tool")
            self.assertTrue(row["expected_abstention"])
            self.assertIsNone(row["expected"])

    def test_legacy_receipts_are_byte_semantically_unchanged(self):
        mapper, legacy = self.mapper()
        self.assertEqual(mapper.label("legacy", "b" * 64), legacy.label("legacy", "b" * 64))
        self.assertNotEqual(mapper.catalog_sha256, legacy.catalog_sha256)

    def test_malformed_query_is_unmapped_not_export_crash_or_tool_selection(self):
        mapper, _ = self.mapper()
        for text in ("unicode import /tmp/source", "unicode upper 1; execute", "unicode upper 256"):
            self.assertEqual(mapper.label(text, "b" * 64)["status"], "unmapped")

    def test_recognized_refusal_never_delegates_to_a_permissive_legacy_mapper(self):
        mapper, legacy = self.mapper()
        legacy.label = Mock(return_value=dict(status="verified_tool", expected=24))
        row = mapper.label("unicode import /tmp/source", "b" * 64)
        self.assertEqual(row["status"], "unmapped")
        legacy.label.assert_not_called()

    def test_source_drift_refuses_export_even_after_labels_cached(self):
        mapper, legacy = self.mapper()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "source"
            path.write_bytes(SOURCE.read_bytes())
            mapper = UnicodeMapper(legacy, path)
            mapper.label("unicode upper 97", "b" * 64)
            path.write_bytes(b"bad")
            with self.assertRaises(QueryError):
                mapper.label("unicode upper 97", "b" * 64)

    def test_extended_episode_cannot_be_scored_as_arithmetic_allocator_headroom(self):
        from qualify import compare_direct
        mapper, _ = self.mapper()
        with self.assertRaisesRegex(ValueError, "qualification_catalog"):
            compare_direct([mapper.label("unicode category DIGIT_ZERO", "b" * 64)])


if __name__ == "__main__":
    unittest.main()
