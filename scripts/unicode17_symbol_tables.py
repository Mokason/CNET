#!/usr/bin/env python3
"""Offline exact ASCII-name category/bidi evidence from a pinned Unicode excerpt.

These are literal finite vocabulary lookups, not natural-language understanding.
No network, runtime Unicode database, inferred labels or caller-replaceable pins.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys

DEFAULT_SOURCE = Path(__file__).resolve().parents[1] / "data/unicode17/UnicodeData-Latin1.txt"
UPSTREAM_URL = "https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt"
UPSTREAM_SHA256 = "2e1efc1dcb59c575eedf5ccae60f95229f706ee6d031835247d843c11d96470c"
RAW_SHA256 = "75dfecc13fe9b1202e3f7c787e4e7f2c848c97c8b092dd75b4f6a2b99990cdc4"
MAX_SOURCE_BYTES = 15707
DATASETS = {"ascii_category": (2, "General_Category"), "ascii_bidi": (4, "Bidi_Class")}


def digest(source):
    return hashlib.sha256(source).hexdigest()


def read_source(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    with os.fdopen(fd, "rb") as source:
        metadata = os.fstat(source.fileno())
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_size != MAX_SOURCE_BYTES:
            raise ValueError("unicode17_symbol_source_refused")
        return source.read(MAX_SOURCE_BYTES + 1)


def make_table(source, dataset):
    if dataset not in DATASETS:
        raise ValueError("unicode17_symbol_dataset_refused")
    if len(source) != MAX_SOURCE_BYTES or digest(source) != RAW_SHA256:
        raise ValueError("unicode17_symbol_source_refused")
    rows = source.decode("ascii").splitlines()
    if len(rows) != 256:
        raise ValueError("unicode17_symbol_source_refused")
    labels = []
    for codepoint, row in enumerate(rows):
        fields = row.split(";")
        if len(fields) != 15 or fields[0] != f"{codepoint:04X}":
            raise ValueError("unicode17_symbol_source_refused")
        if 32 <= codepoint <= 126:
            token = fields[1].replace(" ", "_")
            label = fields[DATASETS[dataset][0]]
            if not re.fullmatch(r"[A-Z][A-Z0-9_-]*", token) or not re.fullmatch(r"[A-Za-z]+", label):
                raise ValueError("unicode17_symbol_source_refused")
            labels.append((token, label))
    labels.sort()
    if len(labels) != 95 or len({token for token, _ in labels}) != 95:
        raise ValueError("unicode17_symbol_source_refused")
    header = ["CNET_LOCAL_SYMBOLS_V1", "dataset " + dataset, "authority verified_tool",
              "input_bits 8", "output_bits 16", "rows 95"]
    table = ("\n".join(header + [token + "\t" + label for token, label in labels]) + "\n").encode("ascii")
    if len(table) > 4096:
        raise ValueError("unicode17_symbol_source_refused")
    return table


def make_provenance(source):
    datasets = {}
    for dataset, (field, property_name) in DATASETS.items():
        table = make_table(source, dataset)
        vocabulary = b"".join(line.split(b"\t")[0] + b"\n" for line in table.splitlines()[6:])
        datasets[dataset] = dict(artifact=dataset + ".symbols.tsv", source_sha256=digest(table),
            vocabulary_sha256=digest(vocabulary), bytes=len(table), rows=95,
            unicode_field_zero_based=field, unicode_property=property_name)
    provenance = dict(schema_version=1, unicode_version="17.0.0", upstream_url=UPSTREAM_URL,
        upstream_sha256=UPSTREAM_SHA256, upstream_bytes=2198209,
        raw_artifact="UnicodeData-Latin1.txt", raw_sha256=digest(source), raw_bytes=len(source),
        raw_scope="exact_first_256_LF_terminated_UnicodeData_rows_U+0000..U+00FF",
        source_authority="owner_reviewed_official_HTTPS_acquisition_2026-09-07",
        derivation="Unicode_name_field_1_ASCII_spaces_to_underscores_then_ASCII_sort",
        coverage=dict(first_codepoint=32, last_codepoint=126, tokens=95,
                      unknown_tokens="abstain", normalization="none_except_source_name_spaces_to_underscores"),
        vocabulary_hash_format="ASCII_sorted_tokens_each_followed_by_LF_no_header_or_labels",
        evidence_authority="verified_tool", labels_from_CNET=False, datasets=datasets,
        scope="literal_printable_ASCII_name_to_external_property_lookup_only",
        withheld=["natural_language_understanding", "generalization", "full_Unicode_coverage"],
        license="LICENSE")
    return (json.dumps(provenance, indent=2, sort_keys=True, ensure_ascii=True) + "\n").encode("ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", choices=(*DATASETS, "provenance"))
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE,
                        help="exact pinned 256-line UnicodeData excerpt; never fetched")
    args = parser.parse_args()
    try:
        source = read_source(args.source)
        output = make_provenance(source) if args.dataset == "provenance" else make_table(source, args.dataset)
    except (OSError, ValueError):
        print("unicode17_symbol_source_refused", file=sys.stderr)
        return 2
    sys.stdout.buffer.write(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
