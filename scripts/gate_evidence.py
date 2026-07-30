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

A later review found a fifth: the binding walked ``git status``, which does not
report paths marked assume-unchanged or skip-worktree, so 83 tracked CNET files
-- ``src/nn.c``, ``include/nn.h`` and ``src/legacy/main.c`` among them -- could
be rewritten mid-run without moving the digest. Their *count* was recorded,
which proves how many blind spots existed, not that nothing moved inside one.
Those paths are now bound by content (see ``special_index_binding``).

Everything here is content-addressed, captured twice, and serialized with
``json.dumps``. Nothing here follows a symlink: a link is bound by where it
points, and only regular files are opened.

Usage: python3 scripts/gate_evidence.py GATE LOG MARKER -- CMD [ARG...]
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import time
import uuid

# Terminal words a gate marker can end with. A log asserting two of them for the
# same prefix has not asserted one.
TERMINAL_WORDS = ("PASS", "FAIL", "WITHHELD", "BLOCKED", "NO_VERDICT", "AMBIGUOUS")

# Special-index content is hashed twice per gate. CNET's 83 such paths hold
# ~1.3 MB, so this ceiling is ~400x the real cost and still far below anything
# that would stall a gate. Crossing it REFUSES rather than silently sampling:
# a binding that quietly stopped covering part of the tree is the exact failure
# this whole mechanism exists to prevent.
SPECIAL_INDEX_BYTE_CAP = 512 * 1024 * 1024


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def path_identity(path: Path) -> bytes:
    """Content identity that never follows a link and never opens a device.

    ``Path.is_file()`` follows symlinks, so a link to ``/dev/zero`` inside a
    bound set would read until the gate died, and a link to somewhere outside
    the repository would bind bytes this tree does not own. A link is bound by
    WHERE IT POINTS; only a regular file is ever opened.
    """
    try:
        info = path.lstat()
    except OSError:
        return b"absent"
    if stat.S_ISLNK(info.st_mode):
        try:
            return b"symlink:" + os.readlink(path).encode("utf-8", "surrogateescape")
        except OSError:
            return b"absent"
    if not stat.S_ISREG(info.st_mode):
        return b"special:%o" % stat.S_IFMT(info.st_mode)
    try:
        return bytes.fromhex(sha256_file(path))
    except OSError:
        return b"absent"


def special_index_binding(root: Path) -> tuple[str, int, int]:
    """Digest every path git was told to stop watching, plus its bytes.

    ``git status`` deliberately omits assume-unchanged (lowercase flag) and
    skip-worktree (``S``) entries -- suppressing them is exactly what those bits
    are for. Recording only their COUNT, as this did before, proves how many
    blind spots there were, not that nothing moved inside one. In CNET that gap
    covered 83 tracked paths including ``src/nn.c``, ``include/nn.h`` and
    ``src/legacy/main.c``.

    An ordinary cached path (flag ``H``) is skipped here on purpose: any change
    to one appears in ``git status`` and is already bound by the dirty walk, so
    hashing all 2000 of them twice per gate would buy nothing. The flag letter
    is part of the digest, so setting or clearing a bit is itself drift.
    """
    digest = hashlib.sha256()
    entries: list[tuple[str, str]] = []
    for record in git(root, "ls-files", "-v", "-z").split("\0"):
        if len(record) < 3 or record[1] != " " or record[0] == "H":
            continue
        entries.append((record[2:], record[0]))
    total = 0
    for entry, flag in sorted(entries):
        candidate = root / entry
        try:
            total += candidate.lstat().st_size
        except OSError:
            pass
        digest.update(flag.encode("utf-8"))
        digest.update(entry.encode("utf-8", "surrogateescape"))
        digest.update(path_identity(candidate))
    return digest.hexdigest(), len(entries), total


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


def tracked_binding(root: Path) -> tuple[str, int]:
    """Digest the status of every changed tracked path AND its bytes."""
    digest = hashlib.sha256()
    dirty = 0
    for line in git(root, "status", "--porcelain=v1").splitlines():
        if len(line) < 4:
            continue
        entry = line[3:].split(" -> ")[-1].strip().strip('"')
        digest.update(line[:3].encode("utf-8"))
        digest.update(entry.encode("utf-8"))
        digest.update(path_identity(root / entry))
        dirty += 1
    return digest.hexdigest(), dirty


def untracked_binding(root: Path) -> str:
    """Digest every untracked, non-ignored path AND its bytes."""
    digest = hashlib.sha256()
    listing = git(root, "ls-files", "--others", "--exclude-standard", "-z")
    for entry in sorted(item for item in listing.split("\0") if item):
        digest.update(entry.encode("utf-8"))
        digest.update(path_identity(root / entry))
    return digest.hexdigest()


def capture(root: Path) -> dict[str, object]:
    tracked, dirty = tracked_binding(root)
    special, special_count, special_bytes = special_index_binding(root)
    return {
        "commit": git(root, "rev-parse", "HEAD").strip() or "unknown",
        "worktree_sha256": tracked,
        "worktree_dirty_files": dirty,
        "special_index_sha256": special,
        "special_index_files": special_count,
        "special_index_bytes": special_bytes,
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
    if int(pre["special_index_bytes"]) > SPECIAL_INDEX_BYTE_CAP:
        print(
            f"GATE_FAIL gate={gate} reason=special_index_too_large:"
            f"{pre['special_index_bytes']}>{SPECIAL_INDEX_BYTE_CAP} "
            "refusing to produce a binding that does not cover the tree"
        )
        return 1
    print(
        f"GATE_RUN gate={gate} run_id={run_id} started={started} "
        f"commit={pre['commit']} worktree={str(pre['worktree_sha256'])[:16]} "
        f"untracked={str(pre['untracked_sha256'])[:16]} "
        f"special_index={str(pre['special_index_sha256'])[:16]} "
        f"dirty={pre['worktree_dirty_files']} "
        f"special_index_files={pre['special_index_files']}",
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
