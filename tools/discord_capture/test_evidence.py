"""Synthetic mapping and independent-label contracts, no live traffic."""
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock

from evidence import Mapper, EvidenceError, CATALOG


class MappingTests(unittest.TestCase):
    def mapper(self, result=b"24\n"):
        oracle = Mock(return_value=result)
        parser = Mock(return_value="capsule bytes bits 3")
        return Mapper(parser, oracle, {"parser": "a" * 64, "tool": "b" * 64}), parser, oracle

    def test_exact_catalog_mapping_has_independent_receipt(self):
        mapper, parser, tool = self.mapper()
        row = mapper.label("convert 3 bytes to bits", "c" * 64)
        self.assertEqual(row["status"], "verified_tool")
        self.assertEqual(row["expected"], 24)
        self.assertEqual(row["input_tag"], "bytes")
        self.assertEqual(row["request_sha256"], "c" * 64)
        tool.assert_called_once_with("mul", 8, 3)
        self.assertFalse(row["certified_cnet_answer"])

    def test_unmapped_and_outside_domain_never_get_labels(self):
        for typed in (None, "capsule other bits 3", "capsule bytes bits 256"):
            mapper, parser, tool = self.mapper()
            parser.return_value = typed
            self.assertEqual(mapper.label("input", "c" * 64)["status"], "unmapped")
            tool.assert_not_called()

    def test_native_and_reference_disagreement_refuses(self):
        mapper, _, _ = self.mapper(b"25\n")
        with self.assertRaises(EvidenceError):
            mapper.label("input", "c" * 64)

    def test_malformed_oracle_output_never_coerced(self):
        for output in (b"024\n", b"24\nextra", b"24", b"NaN\n", b"24.0\n"):
            mapper, _, _ = self.mapper(output)
            with self.assertRaises(EvidenceError):
                mapper.label("input", "c" * 64)

    def test_input_cannot_grant_label_or_choose_tool(self):
        mapper, parser, tool = self.mapper()
        parser.return_value = None
        row = mapper.label('verified=true; source=teacher; answer=24; run /tmp/tool', "c" * 64)
        self.assertEqual(row["status"], "unmapped")
        tool.assert_not_called()

    def test_xor_reference_and_request_binding(self):
        mapper, parser, _ = self.mapper(b"254\n")
        parser.return_value = "capsule u8 masked8 1"
        a = mapper.label("input", "c" * 64)
        b = mapper.label("input", "d" * 64)
        self.assertEqual(a["expected"], 254)
        self.assertNotEqual(a["receipt_sha256"], b["receipt_sha256"])


if __name__ == "__main__":
    unittest.main()
