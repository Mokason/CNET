#!/usr/bin/env python3
"""Run a gate so its log cannot be mistaken for a stale one, or for another run.

WHY THIS EXISTS. Headline gate logs live at stable paths under an ignored
``logs/`` tree and carried no run identity, so "the marker is present" proved
only that *some* process once wrote it.

WHY IT IS PYTHON. The first version was POSIX ``sh`` and the review found four
defects that all come from that choice:

* it hashed ``git status`` **text**, so a tracked file whose bytes changed while
  its status line stayed ``" M path"`` produced an identical binding, and an
  untracked file's content was never hashed at all;
* it had no post-state, so anything could move after the marker was accepted;
* it recorded the command as ``"$*"``, which loses argv boundaries — ``["a b"]``
  and ``["a", "b"]`` serialize identically — and cannot survive a quote,
  backslash or newline in an argument;
* it emitted JSON by ``printf``, so any of those characters produced a file that
  is not JSON; and it matched the marker as a **substring**, so ``X_PASSED``,
  ``NOT_X_PASS`` and a log stating both ``X_PASS`` and ``X_FAIL`` all passed.

Everything here is content-addressed, captured twice, and serialized with
``json.dumps``.

Usage: python3 scripts/gate_evidence.py GATE LOG MARKER -- CMD [ARG...]
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid

# Terminal words a gate marker can end with. A log asserting two of them for the
# same prefix has not asserted one.
TERMINAL_WORDS = ("PASS", "FAIL", "WITHHELD", "BLOCKED", "NO_VERDICT", "AMBIGUOUS")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git(root: Path, *args: str) -> str:
    try:
        done = subprocess.run(
            ["git", *args],
            cwd=root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=120,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return done.stdout if done.returncode == 0 else ""


def tracked_binding(root: Path) -> tuple[str, int, int]:
    """Digest the status of every changed tracked path AND its bytes."""
    digest = hashlib.sha256()
    dirty = 0
    for line in git(root, "status", "--porcelain=v1").splitlines():
        if len(line) < 4:
            continue
        entry = line[3:].split(" -> ")[-1].strip().strip('"')
        digest.update(line[:3].encode("utf-8"))
        digest.update(entry.encode("utf-8"))
        candidate = root / entry
        digest.update(
            bytes.fromhex(sha256_file(candidate)) if candidate.is_file() else b"absent"
        )
        dirty += 1
    assume_unchanged = sum(
        1 for line in git(root, "ls-files", "-v").splitlines() if line[:1].islower()
    )
    return digest.hexdigest(), dirty, assume_unchanged


def untracked_binding(root: Path) -> str:
    """Digest every untracked, non-ignored path AND its bytes."""
    digest = hashlib.sha256()
    listing = git(root, "ls-files", "--others", "--exclude-standard", "-z")
    for entry in sorted(item for item in listing.split("\0") if item):
        digest.update(entry.encode("utf-8"))
        candidate = root / entry
        digest.update(
            bytes.fromhex(sha256_file(candidate)) if candidate.is_file() else b"absent"
        )
    return digest.hexdigest()


def capture(root: Path) -> dict[str, object]:
    tracked, dirty, assume_unchanged = tracked_binding(root)
    return {
        "commit": git(root, "rev-parse", "HEAD").strip() or "unknown",
        "worktree_sha256": tracked,
        "worktree_dirty_files": dirty,
        "assume_unchanged_files": assume_unchanged,
        "untracked_sha256": untracked_binding(root),
    }


def environment_binding(environment: dict[str, str]) -> dict[str, object]:
    """A digest and the knob NAMES. No values: a report is a file people paste."""
    serialized = "\n".join(f"{k}={environment[k]}" for k in sorted(environment))
    return {
        "env_sha256": hashlib.sha256(serialized.encode("utf-8")).hexdigest(),
        "env_variables": len(environment),
        "env_knob_names": sorted(
            k for k in environment if k.startswith(("CNET_", "CCE_"))
        ),
    }


def marker_lines(text: str, marker: str) -> int:
    """Count lines that ARE this marker, not lines that contain it."""
    pattern = re.compile(r"^" + re.escape(marker) + r"(?:\s|$)", re.MULTILINE)
    return len(pattern.findall(text))


def conflicting_verdicts(text: str, marker: str) -> list[str]:
    """Other terminal verdicts for the same marker prefix."""
    prefix = marker
    for word in TERMINAL_WORDS:
        suffix = "_" + word
        if marker.endswith(suffix):
            prefix = marker[: -len(suffix)]
            break
    else:
        return []
    found = []
    for word in TERMINAL_WORDS:
        other = f"{prefix}_{word}"
        if other == marker:
            continue
        if marker_lines(text, other):
            found.append(other)
    return found


def main(argv: list[str]) -> int:
    if len(argv) < 6 or argv[4] != "--":
        print(
            "usage: gate_evidence.py GATE LOG MARKER -- CMD [ARG...]",
            file=sys.stderr,
        )
        return 2
    gate, log_arg, marker = argv[1], argv[2], argv[3]
    command = argv[5:]
    root = Path(__file__).resolve().parents[1]
    log = Path(log_arg)
    log.parent.mkdir(parents=True, exist_ok=True)

    # No stale authority: if the producer never runs there is no log to read.
    for stale in (log, Path(str(log) + ".evidence.json")):
        try:
            stale.unlink()
        except FileNotFoundError:
            pass

    run_id = str(uuid.uuid4())
    started = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    pre = capture(root)
    print(
        f"GATE_RUN gate={gate} run_id={run_id} started={started} "
        f"commit={pre['commit']} worktree={str(pre['worktree_sha256'])[:16]} "
        f"untracked={str(pre['untracked_sha256'])[:16]} "
        f"dirty={pre['worktree_dirty_files']} "
        f"assume_unchanged={pre['assume_unchanged_files']}",
        flush=True,
    )

    try:
        done = subprocess.run(
            command,
            cwd=root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )
        output, status = done.stdout, done.returncode
    except OSError as exc:
        output, status = f"gate_evidence: cannot run {command!r}: {exc}\n", 127

    log.write_text(output, encoding="utf-8")
    sys.stdout.write(output)
    sys.stdout.flush()

    post = capture(root)
    drift = sorted(k for k in set(pre) | set(post) if pre.get(k) != post.get(k))
    present = marker_lines(output, marker)
    conflicts = conflicting_verdicts(output, marker)

    binding = {
        "gate": gate,
        "run_id": run_id,
        "started_at": started,
        "command": command,          # a real JSON array: argv boundaries survive
        "exit_status": status,
        "evidence_log": str(log),
        "evidence_sha256": sha256_file(log) if log.is_file() else "absent",
        "required_marker": marker,
        "marker_lines": present,
        "marker_present": present == 1,
        "conflicting_markers": conflicts,
        "binding_pre": pre,
        "binding_post": post,
        "binding_stable": not drift,
        "binding_drift": drift,
        **environment_binding(dict(os.environ)),
    }
    Path(str(log) + ".evidence.json").write_text(
        json.dumps(binding, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    evidence = f"{log}.evidence.json"
    if status != 0:
        print(f"GATE_FAIL gate={gate} reason=producer_exit_{status} evidence={evidence}")
        return status
    if present == 0:
        print(f"GATE_FAIL gate={gate} reason=missing_marker:{marker} evidence={evidence}")
        return 1
    if present > 1:
        print(
            f"GATE_FAIL gate={gate} reason=marker_repeated_{present}x:{marker} "
            f"evidence={evidence}"
        )
        return 1
    if conflicts:
        print(
            f"GATE_FAIL gate={gate} reason=conflicting_verdicts:{','.join(conflicts)} "
            f"evidence={evidence}"
        )
        return 1
    if drift:
        print(
            f"GATE_FAIL gate={gate} reason=bound_state_changed:{','.join(drift)} "
            f"evidence={evidence}"
        )
        return 1

    print(
        f"GATE_PASS gate={gate} run_id={run_id} commit={pre['commit']} "
        f"evidence_sha256={binding['evidence_sha256']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
