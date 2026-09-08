"""Opt-in Unicode external evidence; existing v1 receipts remain unchanged."""
import hashlib
from pathlib import Path

from evidence import digest
import unicode_queries as uq

VERSION = "capture_unicode_external_v1"


class UnicodeMapper:
    def __init__(self, legacy, source):
        self.legacy = legacy
        self.reference = uq.Reference(source)
        self.catalog_sha256 = digest(dict(legacy=legacy.catalog_sha256, version=VERSION,
                                         datasets=uq.DATASETS, raw_sha256=uq.RAW_SHA256,
                                         missing="abstain", symbol_names="exact_printable_ASCII"))
        self.pins = dict(legacy.pins, unicode_raw=uq.RAW_SHA256,
                         unicode_checker=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                         unicode_query=hashlib.sha256(Path(uq.__file__).read_bytes()).hexdigest())

    def label(self, text, request_sha256):
        self.reference.verify()
        try:
            query = uq.parse(text)
        except uq.QueryError:
            row = dict(version=VERSION, request_sha256=request_sha256,
                       catalog_sha256=self.catalog_sha256, artifacts=self.pins,
                       status="unmapped", certified_cnet_answer=False)
            row["receipt_sha256"] = digest(row)
            return row
        if query is None:
            return self.legacy.label(text, request_sha256)
        dataset, key = query
        expected = self.reference.expected(query)
        row = dict(version=VERSION, request_sha256=request_sha256,
                   catalog_sha256=self.catalog_sha256, artifacts=self.pins,
                   status="verified_tool", certified_cnet_answer=False,
                   input_tag="unicode17_external", output_tag=dataset,
                   key=self.reference.ordinal(query), query_key=key, expected=expected,
                   expected_abstention=expected is None,
                   rule_sha256=digest(dict(dataset=dataset, raw_sha256=uq.RAW_SHA256)))
        row["receipt_sha256"] = digest(row)
        return row
