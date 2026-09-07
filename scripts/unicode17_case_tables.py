#!/usr/bin/env python3
"""Linux offline extractor for pinned Unicode 17 explicit Latin-1 case changes.

Empty case fields abstain in this partial CNET contract, although Unicode's
default is identity. This is not a text case converter. Output uses the existing
local-table evidence format, not a new capsule format. See data/unicode17/README.md.
"""
import argparse
import hashlib
import os
from pathlib import Path
import stat
import sys

DEFAULT_SOURCE = Path(__file__).resolve().parent.parent / "data/unicode17/UnicodeData-Latin1.txt"
FULL_SHA256 = "2e1efc1dcb59c575eedf5ccae60f95229f706ee6d031835247d843c11d96470c"
SUBSET_SHA256 = "75dfecc13fe9b1202e3f7c787e4e7f2c848c97c8b092dd75b4f6a2b99990cdc4"
MAX_SOURCE_BYTES = 4_000_000


def read_source(path: Path) -> bytes:
    """Bound file reads; never block on a FIFO or follow a final symlink."""
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    with os.fdopen(fd, "rb") as source:
        metadata = os.fstat(source.fileno())
        if not stat.S_ISREG(metadata.st_mode) or not 0 < metadata.st_size <= MAX_SOURCE_BYTES:
            raise ValueError("unicode17_source_refused")
        return source.read(MAX_SOURCE_BYTES + 1)


def make_table(source: bytes, kind: str) -> bytes:
    if kind not in ("upper", "lower"):
        raise ValueError("unicode17_mapping_refused")
    if not 0 < len(source) <= MAX_SOURCE_BYTES:
        raise ValueError("unicode17_source_refused")
    digest = hashlib.sha256(source).hexdigest()
    if digest == FULL_SHA256:
        source = b"\n".join(source.split(b"\n", 256)[:256]) + b"\n"
    elif digest != SUBSET_SHA256:
        raise ValueError("unicode17_source_refused")
    if hashlib.sha256(source).hexdigest() != SUBSET_SHA256:
        raise ValueError("unicode17_source_refused")
    rows = source.decode("ascii").splitlines()
    if len(rows) != 256:
        raise ValueError("unicode17_source_refused")
    field = 12 if kind == "upper" else 13
    changes = []
    for key, row in enumerate(rows):
        fields = row.split(";")
        if len(fields) != 15 or fields[0] != f"{key:04X}":
            raise ValueError("unicode17_source_refused")
        if fields[field]:
            value = int(fields[field], 16)
            if value == key or not 0 <= value <= 65535:
                raise ValueError("unicode17_source_refused")
            changes.append(f"{key}\t{value}")
    header = ["CNET_LOCAL_TABLE_V1", f"dataset unicode17_{kind}_latin1", "authority verified_tool",
              "input_bits 8", "output_bits 16", f"rows {len(changes)}"]
    return ("\n".join(header + changes) + "\n").encode("ascii")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mapping", choices=("upper", "lower"))
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE,
                        help="exact pinned full UnicodeData.txt or bundled Latin-1 excerpt; never fetched")
    args = parser.parse_args()
    try:
        result = make_table(read_source(args.source), args.mapping)
    except (OSError, ValueError):
        print("unicode17_source_refused", file=sys.stderr)
        return 2
    sys.stdout.buffer.write(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
