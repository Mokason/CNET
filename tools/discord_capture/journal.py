"""Owner-scoped ingress evidence, not correctness labels or training authority."""
import argparse
import datetime as dt
import fcntl
import json
import os
from pathlib import Path
import re
import sqlite3
import stat
import time
import uuid

MAX_BYTES = 64 * 1024 * 1024
MAX_REQUESTS = 100000
MAX_EVENTS = 100000
MAX_TEXT = 16384
EVENTS = {"segment_start", "gateway_ready", "gateway_close", "gateway_error",
          "sequence_gap", "clock_regression", "process_stop", "gateway_heartbeat"}
SCHEMA = {
    "policy": "CREATE TABLE policy (id INTEGER PRIMARY KEY CHECK(id=1), body TEXT NOT NULL)",
    "events": "CREATE TABLE events (ord INTEGER PRIMARY KEY, segment TEXT NOT NULL, at_ns INTEGER NOT NULL, kind TEXT NOT NULL)",
    "requests": "CREATE TABLE requests (ord INTEGER PRIMARY KEY, id TEXT NOT NULL UNIQUE, segment TEXT NOT NULL, received_ns INTEGER NOT NULL, boot_ns INTEGER NOT NULL, discord_ts TEXT NOT NULL, raw_text TEXT NOT NULL, delivered_text TEXT NOT NULL, peer_status TEXT, reply_status TEXT, completed_ns INTEGER, elapsed_ns INTEGER)",
}


class CaptureError(RuntimeError):
    """Bounded reason code only; never include payloads or secrets."""


def snowflake(value):
    return isinstance(value, str) and re.fullmatch(r"[1-9][0-9]{5,19}", value)


def private_root(root):
    root = Path(root).absolute()
    if root.resolve() != root:
        raise CaptureError("directory_symlink")
    info = root.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise CaptureError("directory_permissions")
    return root


def private_file(path, create=False):
    created = False
    if create:
        try:
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            os.close(fd)
            created = True
        except FileExistsError:
            pass
    info = path.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
            or info.st_mode & 0o077 or info.st_nlink != 1 or info.st_size > MAX_BYTES):
        raise CaptureError("file_boundary")
    return created


def check_store(root):
    private_file(root / "capture.sqlite")
    for suffix in ("-journal", "-wal", "-shm"):
        path = root / ("capture.sqlite" + suffix)
        if path.exists() or path.is_symlink():
            private_file(path)


def validate_store(db):
    if db.execute("PRAGMA page_size").fetchone() != (4096,):
        raise CaptureError("page_size")
    if dict(db.execute("SELECT name,sql FROM sqlite_master WHERE type='table'")) != SCHEMA:
        raise CaptureError("schema_changed")
    if db.execute("SELECT count(*) FROM sqlite_master WHERE type IN ('trigger','view')").fetchone() != (0,):
        raise CaptureError("schema_changed")
    rows = db.execute("SELECT body FROM policy").fetchall()
    try:
        policy = json.loads(rows[0][0])
        if (len(rows) != 1 or set(policy) != {"schema", "origin", "owner", "channel", "channel_type"}
                or policy["schema"] != 1 or policy["origin"] != "discord_gateway"
                or policy["channel_type"] != 1 or not snowflake(policy["owner"])
                or not snowflake(policy["channel"])):
            raise ValueError()
    except (IndexError, ValueError, TypeError):
        raise CaptureError("policy_invalid") from None
    return rows[0][0]


class Journal:
    def __init__(self, root, owner, channel):
        self.db = None
        self.lock = None
        if not snowflake(owner) or not snowflake(channel):
            raise CaptureError("scope_policy")
        self.owner, self.channel = owner, channel
        self.segment = uuid.uuid4().hex
        self.last_sequence = None
        self.root = private_root(root)
        try:
            lockpath = self.root / "capture.lock"
            private_file(lockpath, create=True)
            self.lock = os.open(lockpath, os.O_RDWR | os.O_NOFOLLOW)
            try:
                fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise CaptureError("writer_active") from None
            created = private_file(self.root / "capture.sqlite", create=True)
            check_store(self.root)
            self.db = sqlite3.connect(self.root / "capture.sqlite", timeout=2)
            policy = json.dumps(dict(schema=1, origin="discord_gateway", owner=owner,
                                     channel=channel, channel_type=1), sort_keys=True)
            if not created:
                if validate_store(self.db) != policy:
                    raise CaptureError("policy_changed")
            self.db.execute("PRAGMA journal_mode=DELETE")
            self.db.execute("PRAGMA synchronous=EXTRA")
            self.db.execute("PRAGMA page_size=4096")
            self.db.execute("PRAGMA max_page_count=16384")
            if created:
                with self.db:
                    for sql in SCHEMA.values():
                        self.db.execute(sql)
                    self.db.execute("INSERT INTO policy VALUES(1,?)", (policy,))
            validate_store(self.db)
            self.event("segment_start")
        except BaseException:
            self.close()
            raise

    def selected(self, data):
        if not isinstance(data, dict):
            return False
        author = data.get("author")
        reference = data.get("message_reference")
        return (isinstance(author, dict) and author.get("id") == self.owner
                and author.get("bot", False) is False and not author.get("system", False)
                and data.get("channel_id") == self.channel
                and "guild_id" not in data and "webhook_id" not in data
                and type(data.get("type")) is int and data["type"] in (0, 19)
                and not data.get("message_snapshots")
                and not (isinstance(reference, dict) and reference.get("type") == 1))

    def event(self, kind):
        if kind not in EVENTS:
            raise CaptureError("event_kind")
        check_store(self.root)
        with self.db:
            self._insert_event(kind, time.time_ns())

    def _insert_event(self, kind, at_ns):
        if self.db.execute("SELECT count(*) FROM events").fetchone()[0] >= MAX_EVENTS:
            raise CaptureError("event_quota")
        self.db.execute("INSERT INTO events(segment,at_ns,kind) VALUES(?,?,?)",
                        (self.segment, at_ns, kind))

    def observe_sequence(self, sequence):
        if type(sequence) is not int or sequence < 0:
            raise CaptureError("gateway_sequence")
        if self.last_sequence is not None and sequence != self.last_sequence + 1:
            self.event("sequence_gap")
        self.last_sequence = sequence

    def begin(self, data, delivered):
        """Commit before exactly one client invocation. False means no invocation.

        Caller checks selected() separately to distinguish exclusions from duplicates.
        No replay of unfinished attempts: the downstream may have accepted them.
        """
        if not self.selected(data):
            return False
        raw, stamp, identity = data.get("content"), data.get("timestamp"), data.get("id")
        if not snowflake(identity):
            raise CaptureError("message_id")
        for text in (raw, delivered):
            if not isinstance(text, str) or not text.strip() or len(text.encode("utf-8")) > MAX_TEXT:
                raise CaptureError("message_text")
        try:
            parsed = dt.datetime.fromisoformat(stamp.replace("Z", "+00:00"))
            if parsed.utcoffset() != dt.timedelta(0):
                raise ValueError()
        except (ValueError, TypeError, AttributeError):
            raise CaptureError("message_timestamp") from None
        check_store(self.root)
        with self.db:
            if self.db.execute("SELECT 1 FROM requests WHERE id=?", (identity,)).fetchone():
                return False
            if self.db.execute("SELECT count(*) FROM requests").fetchone()[0] >= MAX_REQUESTS:
                raise CaptureError("request_quota")
            now = time.time_ns()
            previous = self.db.execute("SELECT received_ns FROM requests ORDER BY ord DESC LIMIT 1").fetchone()
            if previous and now < previous[0]:
                # In this transaction: never commit the gap without the request or vice versa.
                self._insert_event("clock_regression", now)
            self.db.execute("""INSERT INTO requests(id,segment,received_ns,boot_ns,discord_ts,
                            raw_text,delivered_text) VALUES(?,?,?,?,?,?,?)""",
                            (identity, self.segment, now, time.clock_gettime_ns(time.CLOCK_BOOTTIME),
                             stamp, raw, delivered))
        return True

    def finish(self, identity, peer_status, reply_status):
        # Neither a completed peer response nor a delivered reply means correctness.
        if peer_status not in {"peer_ok", "peer_error", "peer_unknown"}:
            raise CaptureError("peer_status")
        if reply_status not in {"reply_sent", "reply_failed", "reply_unknown"}:
            raise CaptureError("reply_status")
        check_store(self.root)
        with self.db:
            changed = self.db.execute("""UPDATE requests SET peer_status=?,reply_status=?,
                completed_ns=?,elapsed_ns=?-boot_ns WHERE id=? AND segment=? AND completed_ns IS NULL""",
                (peer_status, reply_status, time.time_ns(),
                 time.clock_gettime_ns(time.CLOCK_BOOTTIME), identity, self.segment)).rowcount
            if changed != 1:
                raise CaptureError("completion_mismatch")

    def close(self):
        if self.db is not None:
            self.db.close()
            self.db = None
        if self.lock is not None:
            os.close(self.lock)
            self.lock = None


def readiness(root):
    """Read-only aggregate diagnostic. Never exports requests or enables learning."""
    root = private_root(root)
    check_store(root)
    db = sqlite3.connect((root / "capture.sqlite").as_uri() + "?mode=ro", uri=True, timeout=2)
    try:
        db.execute("PRAGMA query_only=ON")
        validate_store(db)
        count, unfinished, failures, reply_failures = db.execute("""SELECT count(*),
            coalesce(sum(completed_ns IS NULL),0), coalesce(sum(peer_status='peer_error'),0),
            coalesce(sum(reply_status='reply_failed'),0) FROM requests""").fetchone()
        events = dict(db.execute("SELECT kind,count(*) FROM events GROUP BY kind"))
        return dict(schema=1, requests=count, unfinished=unfinished, peer_failures=failures,
                    reply_failures=reply_failures, segments=events.get("segment_start", 0),
                    ready_segments=events.get("gateway_ready", 0),
                    sequence_gaps=events.get("sequence_gap", 0),
                    clock_regressions=events.get("clock_regression", 0),
                    unmapped=count, verified_labels=0, training_eligible=False,
                    reasons=["mapping_and_independent_verification_required",
                             "whole_episode_development_required", "fresh_confirmation_required"],
                    continuity="observed_segments_only_no_backfill")
    finally:
        db.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(readiness(args.root), sort_keys=True))
    except (OSError, sqlite3.Error, CaptureError):
        print('{"error":"capture_readiness_refused","training_eligible":false}')
        raise SystemExit(1)
