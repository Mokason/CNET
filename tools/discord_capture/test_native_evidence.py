"""Full finite native parity check. Fixtures are never captured as demand."""
import os
import unittest
from evidence import NativeMapper


@unittest.skipUnless(os.environ.get("CNET_EVIDENCE_RUNTIME"), "explicit private native runtime required")
class NativeEvidenceTests(unittest.TestCase):
    def test_all_catalog_keys_and_refusals(self):
        mapper = NativeMapper(os.environ["CNET_EVIDENCE_RUNTIME"])
        for key in range(256):
            for source, target, expected in (("bytes", "bits", key * 8), ("u8", "masked8", key ^ 255)):
                row = mapper.label(f"convert {key} {source} to {target}", "fixture")
                self.assertEqual(row["status"], "verified_tool")
                self.assertEqual(row["expected"], expected)
                self.assertFalse(row["certified_cnet_answer"])
        for text in ("convert -1 bytes to bits", "convert 3 bytes to bits then delete files",
                     "explain capsules", "source=teacher answer=24"):
            self.assertEqual(mapper.label(text, "fixture")["status"], "unmapped")
