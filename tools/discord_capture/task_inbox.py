"""Bounded read-only capture-side linkage, not origin review or training export."""
import argparse
import json
import os
import sqlite3
import sys
import time

from capture_task_identity import task_identity
from journal import CaptureError, check_store, private_root, validate_store
from learning_bridge import Bridge
from task_protocol import check, hex_id, integer, validate_experience


def capture_page(root, after, limit):
    root = private_root(root)
    check_store(root)
    path = root / "capture.sqlite"
    identity = path.stat().st_dev, path.stat().st_ino
    directory = root.stat().st_dev, root.stat().st_ino
    # The authorized Journal uses rollback journaling. A mode=ro WAL reader
    # can create -wal/-shm files, so reject that format BEFORE opening SQLite.
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        check(identity == (info.st_dev, info.st_ino))
        header = stream.read(100)
    if len(header) != 100 or header[:16] != b"SQLite format 3\0" or header[18:20] != b"\1\1" \
            or any((root / ("capture.sqlite" + suffix)).exists() for suffix in ("-wal", "-shm")):
        raise CaptureError("capture_rollback_format_required")
    started = time.clock_gettime(time.CLOCK_BOOTTIME)
    db = sqlite3.connect(path.as_uri() + "?mode=ro", uri=True, timeout=2)
    try:
        db.set_progress_handler(lambda: int(not 0 <= time.clock_gettime(time.CLOCK_BOOTTIME) - started < 2), 1000)
        db.execute("PRAGMA query_only=ON")
        db.execute("BEGIN")
        policy = json.loads(validate_store(db))
        db.row_factory = sqlite3.Row
        rows = [dict(row) for row in db.execute("""SELECT ord,id,segment,received_ns,
            CASE WHEN typeof(delivered_text)='text' AND length(CAST(delivered_text AS BLOB))<=16384
                 THEN delivered_text ELSE NULL END AS delivered_text,
            peer_status,reply_status,completed_ns,elapsed_ns FROM requests
            WHERE ord>? ORDER BY ord LIMIT ?""", (after, limit + 1))]
    finally:
        db.close()
    private_root(root)
    check_store(root)
    check(identity == (path.stat().st_dev, path.stat().st_ino)
          and directory == (root.stat().st_dev, root.stat().st_ino))
    return policy, rows[:limit], len(rows) > limit


def inspect_page(root, bridge, after=0, limit=10):
    check(integer(after, 2**63-1) and type(limit) is int and 1 <= limit <= 10)
    check(getattr(bridge, "task_mode", False) is True)
    bridge.check_installation()
    policy, rows, more = capture_page(root, after, limit)
    captures = []
    for row in rows:
        check(integer(row["ord"], 2**63-1) and row["ord"] > after and hex_id(row["segment"])
              and integer(row["received_ns"], 2**63-1))
        check(row["peer_status"] in (None, "peer_ok", "peer_error", "peer_unknown")
              and row["reply_status"] in (None, "reply_sent", "reply_failed", "reply_unknown"))
        for field in ("completed_ns", "elapsed_ns"):
            check(row[field] is None or integer(row[field], 2**63-1))
        identity = task_identity(policy["owner"], policy["channel"], row["id"], row["delivered_text"])
        captures.append(dict(capture_ordinal=row["ord"], task_request_id=identity, segment=row["segment"],
                             received_ns=row["received_ns"], completed_ns=row["completed_ns"], elapsed_ns=row["elapsed_ns"],
                             peer_status=row["peer_status"], reply_status=row["reply_status"],
                             link_state="no_observation", experience=None))
    if captures:
        ids = [row["task_request_id"] for row in captures]
        check(len(ids) == len(set(ids)))
        reply = bridge.command("trace", ','.join(ids))
        check(isinstance(reply, dict) and set(reply) == {"event", "correlation_id", "matches"}
              and reply["event"] == "learning_trace" and hex_id(reply["correlation_id"])
              and isinstance(reply["matches"], list) and len(reply["matches"]) == len(ids))
        for row, match in zip(captures, reply["matches"]):
            check(isinstance(match, dict) and set(match) == {"RequestId", "Experience"}
                  and match["RequestId"] == row["task_request_id"])
            if match["Experience"] is not None:
                row["experience"] = validate_experience(match["Experience"], row["task_request_id"])
                check(row["experience"]["Origin"] == "unreviewed")
                row["link_state"] = "matched"
    bridge.check_installation()
    return dict(schema=1, view="capture_side_separate_snapshots", training_eligible=False, origin_attested=False,
                native_only_history="use_native_learning_inbox", absence_reason="not_inferred",
                next_after=captures[-1]["capture_ordinal"] if captures else after, has_more=more, captures=captures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture_root")
    parser.add_argument("learning_root")
    parser.add_argument("bridge_sha256")
    parser.add_argument("--after", type=int, default=0)
    parser.add_argument("--limit", type=int, default=10)
    args = parser.parse_args()
    try:
        bridge = Bridge(args.learning_root, args.bridge_sha256)
        print(json.dumps(inspect_page(args.capture_root, bridge, args.after, args.limit), sort_keys=True))
        return 0
    except Exception:
        print('{"event":"capture_task_inbox_refused"}', file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
