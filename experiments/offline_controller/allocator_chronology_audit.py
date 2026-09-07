"""Read-only legacy-log diagnostics. Never approves evidence or prints payloads."""
import argparse
from datetime import datetime
import hashlib
import json
import math
import os
import re
import stat
import time

MAX_BYTES = 64 * 1024 * 1024
MAX_LINE = 64 * 1024
SECONDS = 10
STAMP = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z")


def now():
    try:
        return time.clock_gettime(time.CLOCK_BOOTTIME)
    except (AttributeError, OSError) as error:
        raise ValueError("boottime_unavailable") from error


def reject_constant(_value):
    raise ValueError("nonfinite_json")


def finite_float(value):
    value = float(value)
    if not math.isfinite(value):
        raise ValueError("nonfinite_json")
    return value


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate_json_key")
        result[key] = value
    return result


def count_object(raw, report, previous):
    try:
        row = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_pairs,
                         parse_constant=reject_constant, parse_float=finite_float)
    except (ValueError, RecursionError):
        report["invalid_json"] += 1
        return previous
    if not isinstance(row, dict):
        report["nonobjects"] += 1
        return previous
    report["objects"] += 1
    # Presence counts only: these names do not authenticate a caller or label.
    for output, keys in (
        ("query_rows", ("query", "prompt")),
        ("answer_rows", ("answer", "out", "tgt")),
        ("caller_origin_rows", ("caller_origin", "request_origin")),
        ("request_id_rows", ("request_id",)),
        ("episode_id_rows", ("episode_id",)),
        ("typed_identity_rows", ("dataset", "domain", "capability_id")),
        ("label_receipt_rows", ("label_receipt_sha256",)),
    ):
        report[output] += int(any(key in row for key in keys))
    stamp = row.get("ts")
    try:
        if not isinstance(stamp, str) or not STAMP.fullmatch(stamp):
            raise ValueError("timestamp")
        datetime.strptime(stamp, "%Y-%m-%dT%H:%M:%SZ")
    except ValueError:
        report["invalid_timestamps"] += 1
        return previous
    if previous is not None and stamp < previous:
        report["timestamp_regressions"] += 1
    report["min_timestamp"] = min(report["min_timestamp"] or stamp, stamp)
    report["max_timestamp"] = max(report["max_timestamp"] or stamp, stamp)
    return stamp


def identity(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def inspect(path):
    start = now()
    report = dict.fromkeys((
        "lines", "objects", "invalid_json", "nonobjects", "oversized_lines",
        "unterminated_lines", "invalid_timestamps", "timestamp_regressions",
        "query_rows", "answer_rows", "caller_origin_rows", "request_id_rows",
        "episode_id_rows", "typed_identity_rows", "label_receipt_rows"), 0)
    report.update(event="allocator_chronology_legacy_audit", training_eligible=False,
                  gain_claim="WITHHELD", min_timestamp=None, max_timestamp=None,
                  limitation="field_presence_does_not_establish_evidence_custody")
    digest = hashlib.sha256()
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
        with os.fdopen(fd, "rb") as stream:
            before = os.fstat(stream.fileno())
            if not stat.S_ISREG(before.st_mode) or before.st_uid != os.getuid() or before.st_nlink != 1:
                raise ValueError("source_type_or_owner")
            if before.st_size > MAX_BYTES:
                raise ValueError("source_size")
            report["bytes"] = before.st_size
            report["group_or_world_writable"] = bool(before.st_mode & 0o022)
            report["group_or_world_readable"] = bool(before.st_mode & 0o044)
            remaining, previous = before.st_size, None
            while remaining:
                report["lines"] += 1
                count, oversized, terminated = 0, False, False
                raw = b""
                while remaining:
                    if now() - start > SECONDS:
                        raise ValueError("audit_deadline")
                    part = stream.readline(min(MAX_LINE + 1, remaining))
                    if not part:
                        raise ValueError("source_changed")
                    digest.update(part)
                    remaining -= len(part)
                    count += len(part)
                    oversized = count > MAX_LINE
                    if not oversized:
                        raw += part
                    terminated = part.endswith(b"\n")
                    if terminated:
                        break
                report["oversized_lines"] += int(oversized)
                report["unterminated_lines"] += int(not terminated)
                if not oversized and terminated:
                    previous = count_object(raw, report, previous)
            # A virtual regular file may advertise zero size yet return bytes.
            # Growth or a non-snapshot source must not produce a false digest.
            if stream.read(1):
                raise ValueError("source_changed")
            if now() - start > SECONDS:
                raise ValueError("audit_deadline")
            if identity(before) != identity(os.fstat(stream.fileno())) or identity(before) != identity(os.stat(path, follow_symlinks=False)):
                raise ValueError("source_changed")
    except OSError as error:
        raise ValueError("source_io_refused") from error
    report["sha256"] = digest.hexdigest()
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source")
    args = parser.parse_args()
    try:
        report = inspect(args.source)
    except (ValueError, AttributeError) as error:
        # Never include supplied path or input bytes in diagnostics.
        code = str(error) if isinstance(error, ValueError) else "boottime_unavailable"
        print(json.dumps({"event": "allocator_chronology_audit_refused", "reason": code,
                          "training_eligible": False, "gain_claim": "WITHHELD"}))
        return 2
    print(json.dumps(report, sort_keys=True))
    return 0  # Completed diagnostic, NOT evidence qualification.


if __name__ == "__main__":
    raise SystemExit(main())
