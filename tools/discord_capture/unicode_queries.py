"""Closed Unicode queries and independent labels from the pinned external excerpt.

This is an IO/evidence adapter, not a knowledge store or capsule implementation.
Empty explicit-case fields and names outside the 95-token vocabulary abstain.
"""
import hashlib
import os
from pathlib import Path
import re
import stat

RAW_SHA256 = "75dfecc13fe9b1202e3f7c787e4e7f2c848c97c8b092dd75b4f6a2b99990cdc4"
RAW_BYTES = 15707
DATASETS = ("unicode17_upper_latin1", "unicode17_lower_latin1", "ascii_category", "ascii_bidi")
NUMERIC = re.compile(r"unicode (upper|lower) (0|[1-9][0-9]{0,2})")
SYMBOLIC = re.compile(r"unicode (category|bidi) ([A-Z][A-Z0-9_-]{0,95})")


class QueryError(ValueError):
    """Fixed diagnostic codes only; never embed request text."""


def parse(text):
    """None means unrelated; a recognized malformed command is a refusal."""
    if not isinstance(text, str) or not re.match(r"unicode(?:\s|$)", text):
        return None
    if len(text) <= 128 and text.isascii():
        match = NUMERIC.fullmatch(text)
        if match and int(match[2]) <= 255:
            return "unicode17_" + match[1] + "_latin1", int(match[2])
        match = SYMBOLIC.fullmatch(text)
        if match:
            return "ascii_" + match[1], match[2]
    raise QueryError("unicode_query_refused")


def read_source(path):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    with os.fdopen(fd, "rb") as source:
        info = os.fstat(source.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_size != RAW_BYTES:
            raise QueryError("unicode_source_refused")
        raw = source.read(RAW_BYTES + 1)
    if len(raw) != RAW_BYTES or hashlib.sha256(raw).hexdigest() != RAW_SHA256:
        raise QueryError("unicode_source_refused")
    return raw


class Reference:
    """External labels only. Never populated from a daemon answer or proposal."""
    def __init__(self, path):
        self.path = Path(path)
        raw = read_source(self.path)
        self.raw_sha256 = RAW_SHA256
        self.values = {name: {} for name in DATASETS}
        for line in raw.decode("ascii").splitlines():
            fields = line.split(";")
            key = int(fields[0], 16)
            for name, column in zip(DATASETS[:2], (12, 13), strict=True):
                if fields[column]:
                    self.values[name][key] = int(fields[column], 16)
            if 32 <= key <= 126:
                token = fields[1].replace(" ", "_")
                for name, column in zip(DATASETS[2:], (2, 4), strict=True):
                    self.values[name][token] = fields[column]
        tokens = sorted(self.values["ascii_category"])
        self.ordinals = {token: i for i, token in enumerate(tokens)}
        self.vocabulary_sha256 = hashlib.sha256("".join(token + "\n" for token in tokens).encode("ascii")).hexdigest()

    def verify(self):
        read_source(self.path)

    def expected(self, query):
        dataset, key = query
        return self.values[dataset].get(key)

    def ordinal(self, query):
        dataset, key = query
        return key if dataset in DATASETS[:2] else self.ordinals.get(key)
