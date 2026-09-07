"""Explicit offline task mappings; never CNET-answer-derived correctness labels."""
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from journal import private_file, private_root

# Exact existing config/capsule_tools.example.tsv rules; offline evidence only.
CATALOG = [dict(input_tag="bytes", output_tag="bits", input_bits=8, output_bits=11,
                op="mul", operand=8, minimum=0, maximum=255),
           dict(input_tag="u8", output_tag="masked8", input_bits=8, output_bits=8,
                op="xor", operand=255, minimum=0, maximum=255)]
VERSION = "capture_independent_tool_v1"
TYPED = re.compile(r"capsule ([A-Za-z0-9_]{1,31}) ([A-Za-z0-9_]{1,31}) (0|[1-9][0-9]{0,4})")


class EvidenceError(ValueError):
    pass


def canonical(value):
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False) + "\n").encode()


def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def file_hash(path):
    private_file(path)
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


class Mapper:
    def __init__(self, parse, tool, pins):
        self.parse, self.tool, self.pins = parse, tool, pins
        self.catalog_sha256 = digest(CATALOG)

    def label(self, text, request_sha256):
        typed = self.parse(text)
        match = TYPED.fullmatch(typed) if isinstance(typed, str) else None
        row = dict(version=VERSION, request_sha256=request_sha256,
                   catalog_sha256=self.catalog_sha256, artifacts=self.pins,
                   status="unmapped", certified_cnet_answer=False)
        if match:
            source, target, key = match.groups()
            key = int(key)
            rule = next((r for r in CATALOG if r["input_tag"] == source
                         and r["output_tag"] == target and r["minimum"] <= key <= r["maximum"]), None)
            if rule:
                expected = key * rule["operand"] if rule["op"] == "mul" else key ^ rule["operand"]
                output = self.tool(rule["op"], rule["operand"], key)
                if output != (str(expected) + "\n").encode("ascii"):
                    raise EvidenceError("independent_tool_disagreement")
                row.update(status="verified_tool", input_tag=source, output_tag=target,
                           key=key, expected=expected, rule_sha256=digest(rule))
        row["receipt_sha256"] = digest(row)
        return row


class NativeMapper(Mapper):
    """Pinned, trusted native parser/library in a private runtime directory."""
    def __init__(self, runtime):
        runtime = private_root(runtime)
        library = runtime / "libcapture_intent.so"
        oracle = runtime / "cnet_capsule_tool"
        pins = {"parser": file_hash(library), "tool": file_hash(oracle),
                "checker": hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
        native = ctypes.CDLL(str(library))
        parse_fn = native.cnet_semantic_capsule_intent
        parse_fn.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
        parse_fn.restype = ctypes.c_int
        cache = {}

        def parse(text):
            if not isinstance(text, str) or not text.isascii() or len(text) >= 512 or "\0" in text:
                return None
            text = text.strip()
            if TYPED.fullmatch(text):
                return text
            result = ctypes.create_string_buffer(160)
            code = parse_fn(text.encode("ascii"), result, len(result))
            return result.value.decode("ascii") if code == 1 else None

        def tool(op, operand, key):
            identity = (op, operand, key)
            if identity not in cache:
                if len(cache) >= 512 or file_hash(oracle) != pins["tool"]:
                    raise EvidenceError("oracle_bound_or_identity")
                result = subprocess.run([str(oracle), op, str(operand), str(key)],
                                        stdin=subprocess.DEVNULL, capture_output=True,
                                        timeout=2, env={"LC_ALL": "C"}, close_fds=True)
                if result.returncode != 0 or result.stderr:
                    raise EvidenceError("oracle_refused")
                cache[identity] = result.stdout
            return cache[identity]

        super().__init__(parse, tool, pins)
