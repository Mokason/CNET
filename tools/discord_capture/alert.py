"""Local notification throttling; suppression is successful, delivery failure is not."""
import argparse
import fcntl
import json
import math
import os
from pathlib import Path
import subprocess
import tempfile
import time

from journal import private_file, private_root


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("alert_state_duplicate")
        result[key] = value
    return result


def notify(root, boot, clock, send):
    """One successful send per 300 CLOCK_BOOTTIME seconds, serialized on disk.

    A crash between desktop delivery and durable publication can repeat a send;
    this is not exactly-once delivery. Invalid state fails loudly, never resets.
    """
    root = private_root(root)
    if not isinstance(boot, str) or not 1 <= len(boot) <= 64:
        raise ValueError("alert_clock")
    lock = root / "alert.lock"
    private_file(lock, create=True)
    fd = os.open(lock, os.O_RDWR | os.O_NOFOLLOW)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            return "busy"
        seconds = clock()
        if type(seconds) not in (int, float) or not math.isfinite(seconds) or seconds < 0:
            raise ValueError("alert_clock")
        path = root / "last_sent.json"
        if path.exists() or path.is_symlink():
            private_file(path)
            if path.stat().st_size > 4096:
                raise ValueError("alert_state_size")
            row = json.loads(path.read_bytes(), object_pairs_hook=unique_object)
            if (not isinstance(row, dict) or set(row) != {"boot", "seconds"}
                    or not isinstance(row["boot"], str) or not 1 <= len(row["boot"]) <= 64
                    or type(row["seconds"]) not in (int, float)
                    or not math.isfinite(row["seconds"]) or row["seconds"] < 0):
                raise ValueError("alert_state_schema")
            if row["boot"] == boot:
                elapsed = seconds - row["seconds"]
                if elapsed < 0:
                    raise ValueError("alert_clock_regression")
                if elapsed < 300:
                    return "suppressed"
        send()  # Caller supplies fixed argv, bounded timeout; failure propagates.
        completed = clock()
        if (type(completed) not in (int, float) or not math.isfinite(completed)
                or completed < seconds):
            raise ValueError("alert_clock_regression")
        staging_fd, staging = tempfile.mkstemp(prefix="sent-", suffix=".json", dir=root)
        with os.fdopen(staging_fd, "w") as output:
            json.dump(dict(boot=boot, seconds=completed), output, allow_nan=False)
            output.flush()
            os.fsync(output.fileno())
        os.replace(staging, path)
        directory = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
        return "sent"
    finally:
        os.close(fd)


def desktop_send():
    subprocess.run([
        "/usr/bin/notify-send", "--app-name=CNET", "--urgency=critical", "--expire-time=10000",
        "CNET capture needs attention",
        "Inspect cnet-capture-monitor.service and cnet-capture-watchdog.service in the user journal. "
        "Runbook: tools/discord_capture/README.md#alerts. No learning is authorized by capture health."
    ], check=True, timeout=8, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("state", type=Path)
    args = parser.parse_args()
    try:
        boot = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        result = notify(args.state, boot, lambda: time.clock_gettime(time.CLOCK_BOOTTIME), desktop_send)
    except subprocess.SubprocessError:
        result = "delivery_failed"
    except (OSError, RuntimeError, ValueError, OverflowError):
        result = "state_refused"
    print(json.dumps(dict(event="capture_alert", result=result)), flush=True)
    return 1 if result in {"delivery_failed", "state_refused"} else 0


if __name__ == "__main__":
    raise SystemExit(main())
