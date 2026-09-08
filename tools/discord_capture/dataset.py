"""Private immutable DEVELOPMENT exports. No training or activation authority."""
import argparse
from collections import defaultdict
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import time

from evidence import NativeMapper, canonical, digest, file_hash
from journal import check_store, private_file, private_root, validate_store

WINDOW_NS = 1800 * 10**9
MAX_BYTES = 64 * 1024 * 1024
GAPS = {"segment_start", "gateway_close", "gateway_error", "sequence_gap", "clock_regression", "process_stop"}


def now_boot():
    return time.clock_gettime(time.CLOCK_BOOTTIME)


def check_deadline(deadline):
    if now_boot() > deadline:
        raise ValueError("export_deadline")


def encode_bounded(values, budget, deadline, lines=False):
    result = bytearray()
    encoder = json.JSONEncoder(sort_keys=True, separators=(",", ":"), allow_nan=False)
    for value in values if lines else [values]:
        for text in encoder.iterencode(value):
            check_deadline(deadline)
            raw = text.encode()
            if len(raw) > budget[0]:
                raise ValueError("export_total_size")
            budget[0] -= len(raw)
            result.extend(raw)
        if budget[0] < 1:
            raise ValueError("export_total_size")
        result.extend(b"\n")
        budget[0] -= 1
    return bytes(result)


def write_private(path, raw):
    if len(raw) > MAX_BYTES:
        raise ValueError("export_size")
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, "wb") as output:
        output.write(raw)
        output.flush()
        os.fsync(output.fileno())


def snapshot(root):
    root = private_root(root)
    check_store(root)
    deadline = now_boot() + 10
    db = sqlite3.connect((root / "capture.sqlite").as_uri() + "?mode=ro", uri=True, timeout=2)
    try:
        db.execute("PRAGMA query_only=ON")
        db.set_progress_handler(lambda: int(now_boot() > deadline), 1000)
        db.execute("BEGIN")
        policy = validate_store(db)
        db.row_factory = sqlite3.Row
        rows = [dict(r) for r in db.execute("SELECT * FROM requests ORDER BY ord LIMIT 100001")]
        events = [dict(r) for r in db.execute("SELECT * FROM events ORDER BY ord LIMIT 100001")]
        if len(rows) > 100000 or len(events) > 100000:
            raise ValueError("snapshot_count")
        return policy, rows, events
    finally:
        db.close()


def build_episodes(rows, labels, events, observed_ns, reviews=None, deadline=float("inf")):
    reviews = reviews or {}
    groups = defaultdict(list)
    by_window = defaultdict(list)
    for event in events:
        check_deadline(deadline)
        by_window[event["at_ns"] // WINDOW_NS].append(event)
    for row, label in zip(rows, labels, strict=True):
        check_deadline(deadline)
        groups[row["received_ns"] // WINDOW_NS].append((row, label))
    episodes = []
    for window, pairs in sorted(groups.items()):
        check_deadline(deadline)
        start, end = window * WINDOW_NS, (window + 1) * WINDOW_NS
        excluded = set()
        identities = [digest(row) for row, _ in pairs]
        identity = digest(dict(window=window, requests=identities))
        origin = reviews.get(identity, "unreviewed")
        if origin != "human":
            excluded.add("origin_" + origin)
        if observed_ns < end:
            excluded.add("window_open")
        segments = {row["segment"] for row, _ in pairs}
        if len(segments) != 1 or any(event["kind"] in GAPS for event in by_window[window]):
            excluded.add("continuity_gap")
        nearby = [e for w in (window - 1, window, window + 1) for e in by_window[w]]
        pulses = sorted(e["at_ns"] for e in nearby if e["segment"] in segments
                        and e["kind"] == "gateway_heartbeat" and start - 120 * 10**9 <= e["at_ns"] <= end + 120 * 10**9)
        if (not pulses or pulses[0] > start or pulses[-1] < end
                or any(b - a > 120 * 10**9 for a, b in zip(pulses, pulses[1:]))):
            excluded.add("heartbeat_coverage_missing")
        if any(row["completed_ns"] is None for row, _ in pairs):
            excluded.add("unfinished")
        if any(label["status"] != "verified_tool" for _, label in pairs):
            excluded.add("unmapped")
        if any(a[0]["received_ns"] > b[0]["received_ns"] for a, b in zip(pairs, pairs[1:])):
            excluded.add("clock_order")
        episodes.append(dict(window=window, start_ns=start, end_ns=end, requests=len(pairs),
                             episode_sha256=identity, origin=origin,
                             record_ordinals=[row["ord"] for row, _ in pairs],
                             excluded=sorted(excluded), split="development"))
    return episodes


def export_dataset(capture, destination, mapper, reviews=None):
    deadline = now_boot() + 30
    destination = Path(destination).absolute()
    private_root(destination.parent)
    if any((parent / ".git").exists() for parent in (destination, *destination.parents)):
        raise ValueError("export_inside_repository")
    destination.mkdir(mode=0o700)  # Exclusive: never rewrite an old export.
    policy, rows, events = snapshot(capture)
    observed_ns = time.time_ns()
    labels = []
    for row in rows:
        check_deadline(deadline)
        labels.append(mapper.label(row["delivered_text"], digest(row)))
    check_deadline(deadline)
    episodes = build_episodes(rows, labels, events, observed_ns, reviews, deadline)
    budget = [MAX_BYTES]
    files = {"requests.jsonl": encode_bounded(rows, budget, deadline, lines=True),
             "evidence.jsonl": encode_bounded(labels, budget, deadline, lines=True),
             "episodes.json": encode_bounded(episodes, budget, deadline),
             "events.json": encode_bounded(events, budget, deadline),
             "capture-policy.json": encode_bounded(json.loads(policy), budget, deadline)}
    for name, raw in files.items():
        check_deadline(deadline)
        write_private(destination / name, raw)
    manifest = dict(schema=1, purpose="development_only", observed_ns=observed_ns, files={k: hashlib.sha256(v).hexdigest() for k, v in files.items()},
                    catalog_sha256=mapper.catalog_sha256, artifacts=mapper.pins,
                    exporter_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                    requests=len(rows), verified_labels=sum(r["status"] == "verified_tool" for r in labels),
                    episodes=len(episodes), eligible_episodes=sum(not r["excluded"] for r in episodes),
                    training_eligible=False, confirmation_eligible=False,
                    reasons=["development_outcome_and_headroom_gate_required", "fresh_confirmation_required"])
    # Last publication is the completion marker. Consumers reject an absent manifest.
    raw_manifest = encode_bounded(manifest, budget, deadline)
    check_deadline(deadline)
    write_private(destination / "manifest.json", raw_manifest)
    fd = os.open(destination, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)
    return manifest


def validate_export(root):
    """Identity/schema only; a manifest never grants eligibility or authenticity."""
    root = private_root(root)
    private_file(root / "manifest.json")
    manifest = json.loads((root / "manifest.json").read_bytes())
    expected = {"requests.jsonl", "evidence.jsonl", "episodes.json", "events.json", "capture-policy.json"}
    if (manifest.get("schema") != 1 or manifest.get("purpose") != "development_only"
            or manifest.get("training_eligible") is not False or manifest.get("confirmation_eligible") is not False
            or set(manifest.get("files", {})) != expected):
        raise ValueError("export_manifest")
    for name, pin in manifest["files"].items():
        if file_hash(root / name) != pin:
            raise ValueError("export_identity")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("runtime", type=Path)
    parser.add_argument("--unicode-source", type=Path, help="opt in to the exact pinned Unicode excerpt; no network")
    parser.add_argument("--review", type=Path, help="owner-private episode-digest to human/test/automation mapping")
    args = parser.parse_args()
    try:
        reviews = None
        if args.review:
            private_file(args.review)
            reviews = json.loads(args.review.read_bytes())
            if not isinstance(reviews, dict) or len(reviews) > 10000 or any(v not in {"human", "test", "automation"} for v in reviews.values()):
                raise ValueError("origin_review")
        mapper = NativeMapper(args.runtime)
        if args.unicode_source:
            from unicode_evidence import UnicodeMapper
            mapper = UnicodeMapper(mapper, args.unicode_source)
        report = export_dataset(args.capture, args.destination, mapper, reviews)
        print(json.dumps({k: report[k] for k in ("requests", "verified_labels", "episodes", "eligible_episodes", "training_eligible", "reasons")}))
    except Exception:
        print('{"error":"evidence_export_refused","training_eligible":false}')
        raise SystemExit(1)
