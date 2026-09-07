"""Bounded local capture alerts and real elapsed observation, never training."""
import argparse
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import secrets
import sqlite3
import subprocess
import time

from dataset import write_private
from evidence import canonical
from journal import private_file, private_root, check_store, validate_store

LEGACY_FILES = (
    Path("/home/marble/.local/share/cnet-minimal/var/miss_log.jsonl"),
    Path("/home/marble/.local/share/cnet-minimal/versions/CNET-Minimal-273250e/var/discord_last.json"))
LEGACY_ALIAS = Path("/home/marble/.local/share/cnet-minimal/current/var/discord_last.json")


def assess(sample):
    if sample.get("sample_failed"):
        return ["monitor_refused"]
    alerts = []
    if not sample["connected"]:
        alerts.append("bridge_down")
    age = sample["heartbeat_age"]
    if age is None or not math.isfinite(age) or age < 0 or age > 180:
        alerts.append("heartbeat_stale")
    if sample["pending_age"] > 180:
        alerts.append("request_stuck")
    if sample["db_bytes"] >= 0.8 * 64 * 1024**2 or max(sample["requests"], sample["events"]) >= 80000:
        alerts.append("storage_near_limit")
    if sample["free_bytes"] < 256 * 1024**2:
        alerts.append("disk_reserve_low")
    if sample["recent_starts"] >= 3:
        alerts.append("reconnect_burst")
    if not sample["legacy_private"]:
        alerts.append("legacy_privacy")
    return alerts


def legacy_private():
    try:
        if LEGACY_ALIAS.resolve() != LEGACY_FILES[1]:
            return False
        for path in LEGACY_FILES:
            private_file(path)
            private_root(path.parent)
        return True
    except (OSError, ValueError, RuntimeError):
        return False


def sample_capture(root):
    root = private_root(root)
    check_store(root)
    now = time.time_ns()
    db = sqlite3.connect((root / "capture.sqlite").as_uri() + "?mode=ro", uri=True, timeout=2)
    try:
        db.execute("PRAGMA query_only=ON")
        db.execute("BEGIN")
        policy = validate_store(db)
        segment = db.execute("SELECT segment FROM events WHERE kind='segment_start' ORDER BY ord DESC LIMIT 1").fetchone()
        if not segment:
            raise ValueError("missing_segment")
        heartbeat = db.execute("SELECT max(at_ns) FROM events WHERE segment=? AND kind='gateway_heartbeat'", segment).fetchone()[0]
        count, pending = db.execute("SELECT count(*),min(CASE WHEN completed_ns IS NULL THEN received_ns END) FROM requests").fetchone()
        events, starts = db.execute("SELECT count(*),coalesce(sum(kind='segment_start' AND at_ns>=?),0) FROM events", (now-300*10**9,)).fetchone()
        ended = db.execute("SELECT count(*) FROM events WHERE segment=? AND kind IN ('gateway_close','gateway_error')", segment).fetchone()[0]
        last_gap = db.execute("SELECT coalesce(max(ord),0) FROM events WHERE kind IN ('segment_start','sequence_gap','clock_regression','gateway_close','gateway_error','process_stop')").fetchone()[0]
    finally:
        db.close()
    service = subprocess.run(["systemctl", "--user", "show", "cnet-discord-peer.service", "-p", "ActiveState", "--value"],
                             capture_output=True, timeout=5, text=True, check=True)
    space = os.statvfs(root)
    return dict(connected=service.stdout.strip() == "active" and ended == 0,
                heartbeat_age=None if heartbeat is None else (now-heartbeat)/10**9,
                pending_age=0 if pending is None else max(0, (now-pending)/10**9),
                db_bytes=(root/"capture.sqlite").stat().st_size, requests=count, events=events,
                free_bytes=space.f_bavail*space.f_frsize, recent_starts=starts,
                legacy_private=legacy_private(), segment=segment[0], last_gap=last_gap,
                policy_sha256=hashlib.sha256(policy.encode()).hexdigest())


def release_pin():
    root = Path(__file__).parent
    files = ("monitor.py", "gateway.py", "journal.py", "dataset.py", "evidence.py")
    return hashlib.sha256(canonical({name: hashlib.sha256((root / name).read_bytes()).hexdigest()
                                     for name in files})).hexdigest()


def record_observation(root, sample, boot_id, boot_seconds):
    root = private_root(root)
    if not math.isfinite(boot_seconds) or boot_seconds < 0:
        raise ValueError("observation_clock")
    private_file(root / "observer.lock", create=True)
    fd = os.open(root / "observer.lock", os.O_RDWR | os.O_NOFOLLOW)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        latest, history = root / "latest.json", root / "observations.jsonl"
        previous = None
        if latest.exists() or latest.is_symlink():
            private_file(latest)
            previous = json.loads(latest.read_bytes())
        elif history.exists():
            raise ValueError("observation_state_missing")
        alerts = assess(sample)
        observer_pin = release_pin()
        delta = 0 if previous is None else boot_seconds - previous["boot_seconds"]
        if previous and (previous["boot_id"] != boot_id or not 0 < delta <= 90
                         or previous["observer_sha256"] != observer_pin):
            alerts.append("observation_gap")
        if previous and any(previous["metrics"].get(key) != sample.get(key)
                            for key in ("segment", "last_gap", "policy_sha256")):
            alerts.append("capture_discontinuity")
        healthy = 0
        if previous and not previous["alerts"] and not alerts:
            healthy = previous["healthy_seconds"] + delta
        row = dict(schema=1, event="capture_health", alerts=alerts, metrics=sample,
                   boot_id=boot_id, boot_seconds=boot_seconds, healthy_seconds=healthy,
                   observer_sha256=observer_pin, at_ns=time.time_ns(),
                   observation_72h=healthy >= 72*3600, product_acceptance=False,
                   runbook="tools/discord_capture/README.md#alerts")
        raw = canonical(row)
        private_file(history, create=True)
        if history.stat().st_size + len(raw) > 32*1024**2:
            raise ValueError("observation_history_full")
        with os.fdopen(os.open(history, os.O_WRONLY | os.O_APPEND | os.O_NOFOLLOW), "ab") as output:
            output.write(raw)
            output.flush()
            os.fsync(output.fileno())
        # A crashed publication may leave an orphan, but cannot block the next tick.
        staging = root / ("next-" + secrets.token_hex(16) + ".json")
        write_private(staging, raw)
        os.replace(staging, latest)
        directory = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
        return row
    finally:
        os.close(fd)


def check_observer(root, boot_id, boot_seconds):
    root = private_root(root)
    private_file(root / "latest.json")
    row = json.loads((root / "latest.json").read_bytes())
    age = boot_seconds - row["boot_seconds"]
    if (row["boot_id"] != boot_id or not math.isfinite(age) or not 0 <= age <= 120
            or row["observer_sha256"] != release_pin() or row["alerts"]):
        raise ValueError("observer_stale_or_unhealthy")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("state", type=Path)
    parser.add_argument("--watchdog", action="store_true")
    args = parser.parse_args()
    try:
        boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        seconds = time.clock_gettime(time.CLOCK_BOOTTIME)
        if args.watchdog:
            check_observer(args.state, boot, seconds)
            print('{"event":"capture_watchdog","healthy":true}')
            raise SystemExit(0)
        try:
            sampled = sample_capture(args.capture)
        except Exception:
            sampled = {"sample_failed": True}
        result = record_observation(args.state, sampled, boot, seconds)
        print(json.dumps(result, sort_keys=True))
        raise SystemExit(2 if result["alerts"] else 0)
    except Exception:
        print('{"event":"capture_health","alerts":["monitor_refused"],"product_acceptance":false}', flush=True)
        raise SystemExit(1)
