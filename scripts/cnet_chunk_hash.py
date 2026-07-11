#!/usr/bin/env python3
"""Resumable chunk-tree identity for very large model artifacts.

A whole-file SHA-256 cannot resume because hashlib state is not serializable.
This tool hashes independent fixed-size chunks, fsyncs each completed record,
and hashes the ordered chunk manifest into a stable sha256-tree-v1 identity.
Changing size, mtime, or chunk size invalidates the partial manifest.
"""

from __future__ import annotations

import hashlib
import json
import os
import struct
import sys
from pathlib import Path
from typing import NoReturn

DEFAULT_CHUNK_BYTES = 1 << 30
READ_BYTES = 8 << 20


def die(message: str) -> NoReturn:
    print(f"cnet_chunk_hash: {message}", file=sys.stderr)
    raise SystemExit(1)


def load_records(state_path: Path, header: dict[str, int]) -> dict[int, tuple[int, str]]:
    if not state_path.exists():
        return {}
    try:
        with state_path.open("r", encoding="utf-8") as stream:
            first = json.loads(stream.readline())
            if first != header:
                return {}
            records: dict[int, tuple[int, str]] = {}
            for raw in stream:
                item = json.loads(raw)
                index = int(item["index"])
                size = int(item["size"])
                digest = str(item["sha256"])
                if index < 0 or size < 0 or len(digest) != 64:
                    return {}
                int(digest, 16)
                records[index] = (size, digest)
            return records
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError):
        return {}


def initialize_state(state_path: Path, header: dict[str, int]) -> None:
    state_path.parent.mkdir(parents=True, exist_ok=True)
    temp = state_path.with_name(f"{state_path.name}.tmp.{os.getpid()}")
    with temp.open("w", encoding="utf-8") as stream:
        stream.write(json.dumps(header, sort_keys=True, separators=(",", ":")) + "\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temp, state_path)


def append_record(state_path: Path, index: int, size: int, digest: str) -> None:
    record = {"index": index, "size": size, "sha256": digest}
    with state_path.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n")
        stream.flush()
        os.fsync(stream.fileno())


def hash_chunk(stream, offset: int, size: int) -> str:
    stream.seek(offset)
    digest = hashlib.sha256()
    remaining = size
    while remaining:
        block = stream.read(min(READ_BYTES, remaining))
        if not block:
            die(f"unexpected EOF at offset {offset + size - remaining}")
        digest.update(block)
        remaining -= len(block)
    return digest.hexdigest()


def write_identity(identity_path: Path, identity: str) -> None:
    identity_path.parent.mkdir(parents=True, exist_ok=True)
    temp = identity_path.with_name(f"{identity_path.name}.tmp.{os.getpid()}")
    with temp.open("w", encoding="utf-8") as stream:
        stream.write(identity + "\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temp, identity_path)


def main() -> int:
    if len(sys.argv) != 3:
        die("usage: cnet_chunk_hash.py MODEL IDENTITY_FILE")
    model = Path(sys.argv[1]).resolve()
    identity_path = Path(sys.argv[2]).resolve()
    if not model.is_file():
        die(f"model not found: {model}")
    chunk_bytes = DEFAULT_CHUNK_BYTES
    try:
        chunk_bytes = int(os.environ.get("CNET_HASH_CHUNK_BYTES", str(DEFAULT_CHUNK_BYTES)))
    except ValueError:
        die("CNET_HASH_CHUNK_BYTES must be an integer")
    if chunk_bytes <= 0:
        die("CNET_HASH_CHUNK_BYTES must be positive")

    stat = model.stat()
    header = {
        "version": 1,
        "size": stat.st_size,
        "mtime_ns": stat.st_mtime_ns,
        "chunk_bytes": chunk_bytes,
    }
    state_path = Path(f"{identity_path}.chunks.jsonl")
    records = load_records(state_path, header)
    chunks = (stat.st_size + chunk_bytes - 1) // chunk_bytes

    valid = True
    for index, (size, _) in records.items():
        expected = min(chunk_bytes, stat.st_size - index * chunk_bytes)
        if index >= chunks or size != expected:
            valid = False
            break
    if not valid or (not state_path.exists()) or not records:
        # An empty file legitimately has zero records; still rewrite its header.
        initialize_state(state_path, header)
        records = {}

    reused = len(records)
    hashed = 0
    with model.open("rb", buffering=0) as stream:
        for index in range(chunks):
            if index in records:
                continue
            offset = index * chunk_bytes
            size = min(chunk_bytes, stat.st_size - offset)
            digest = hash_chunk(stream, offset, size)
            append_record(state_path, index, size, digest)
            records[index] = (size, digest)
            hashed += 1
            print(f"chunk={index + 1}/{chunks} bytes={size} sha256={digest}", flush=True)

    root = hashlib.sha256()
    root.update(b"cnet-sha256-tree-v1\0")
    root.update(struct.pack(">QQ", stat.st_size, chunk_bytes))
    for index in range(chunks):
        size, digest = records[index]
        root.update(struct.pack(">QQ", index, size))
        root.update(bytes.fromhex(digest))
    identity = f"sha256-tree-v1:{root.hexdigest()}"
    write_identity(identity_path, identity)
    print(f"identity={identity} chunks={chunks} reused={reused} hashed={hashed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
