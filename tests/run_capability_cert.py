#!/usr/bin/env python3
"""Run committed held-out capability manifests without invoking a shell.

The runner certifies a capability only when the evaluator PROVES it consumed the
declared fixture. Before the 2026-07-30 re-analysis this file exported
``CNET_HELD_OUT_FIXTURE`` and no evaluator read it, so the declared cases were
decorative: the fixture SHA-256 recorded here proved which file existed, never
which cases ran, and an ungraded capability scored ``1.0`` from marker presence
alone. Both holes are closed here:

* every evaluator must emit a consumption receipt (``HELDOUT_FIXTURE`` /
  ``HELDOUT_CASE`` / ``HELDOUT_METRIC``) binding the exact fixture bytes it
  parsed, every declared case id, and a consumed count equal to the declared
  case count. An evaluator that ignores the fixture — or does not run at all,
  which ``dotnet test --no-restore`` will happily do while exiting 0 — cannot
  produce one;
* there is no default metric. A manifest declares ``metric_source``, and either
  the receipt or a declared regex supplies the number.

The report binds each result to the run: commit, working-tree digest,
special-index digest, evaluator argv, evaluator binary digest, declared
source-set digest, environment digest, run UUID and start time. A PASS that
cannot be tied back to the tree that produced it is not evidence.

A later review found the working-tree digest walked ``git status``, which does
not report assume-unchanged or skip-worktree paths -- 83 of them in CNET,
including the trainer. Only their count was recorded, which names a blind spot
without closing it. ``special_index_binding`` now binds their bytes.

A later one still found that the silent-success mode named above had no
counterpart in the tree: nothing ever *built* the dotnet evaluator, so a fresh
checkout ran ``--no-restore`` against an unrestored project, got zero bytes and
exit 0, and was refused -- correctly, and permanently. Every manifest whose
evaluator is not itself a build command now declares ``evaluator_prepare``, it
runs before the evaluator, and its declared binary must exist and be no older
than the sources that define what the evaluator does
(``scripts/capability_evaluator_prereq.py``).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
import time
import uuid
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from capability_evaluator_prereq import (  # noqa: E402
    declaration_problems,
    ensure_ready,
)

SCHEMA_VERSION = 1
ALLOWED_EVALUATORS = {"make", "dotnet"}
CAPABILITY_ID = re.compile(r"^[a-z][a-z0-9_]{2,63}$")
CASE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
METRIC_SOURCES = {"heldout_receipt", "regex"}

# Special-index content is hashed three times per capability. CNET's 83 such
# paths hold ~1.3 MB, so this ceiling is far above the real cost and far below
# anything that would stall a run. Crossing it REFUSES rather than silently
# sampling: a binding that quietly stopped covering part of the tree is the
# exact failure this mechanism exists to prevent.
SPECIAL_INDEX_BYTE_CAP = 512 * 1024 * 1024

RECEIPT_RE = re.compile(
    r"^HELDOUT_FIXTURE capability=(\S+) sha256=([0-9a-f]{64}) "
    r"cases=(\d+) consumed=(\d+)$",
    re.MULTILINE,
)
CASE_RE = re.compile(r"^HELDOUT_CASE id=(\S+) reads=(\d+)$", re.MULTILINE)
RECEIPT_METRIC_RE = re.compile(
    r"^HELDOUT_METRIC cases_passed=(\d+) cases_declared=(\d+) "
    r"metric=([0-9.]+) errors=(\d+)$",
    re.MULTILINE,
)


# Terminal words a verdict marker can end with. A log asserting two of them for
# the same prefix has not asserted one.
TERMINAL_WORDS = ("PASS", "FAIL", "WITHHELD", "BLOCKED", "NO_VERDICT", "AMBIGUOUS")


def looks_terminal(marker: str) -> bool:
    """Is this string shaped like a verdict rather than a field probe?"""
    return any(marker.endswith("_" + word) for word in TERMINAL_WORDS)


def marker_lines(text: str, marker: str) -> int:
    """Count lines that ARE this marker, not lines that merely contain it."""
    pattern = re.compile(r"^" + re.escape(marker) + r"(?:\s|$)", re.MULTILINE)
    return len(pattern.findall(text))


def terminal_marker_ok(output: str, marker: str) -> tuple[bool, list[str]]:
    """Exactly one anchored whole-line marker, and no rival verdict beside it.

    The runner used to ask ``marker in output``. A raw substring test certifies
    on ``CAP_X_PASSED``, on ``NOT_CAP_X_PASS``, on a mid-sentence mention, and
    on a log that prints ``CAP_X_PASS`` once and ``CAP_X_FAIL`` right after it;
    it also cannot see a duplicate, so two concatenated evaluator runs read as
    one. Cardinality is part of a verdict, so it is checked here.

    A marker that does not end in a terminal word (a fixture's progress
    assertion, say) is only required to appear exactly once -- demanding a
    verdict family of it would invent a rule the manifest never made.
    """
    problems: list[str] = []
    count = marker_lines(output, marker)
    if count == 0:
        problems.append(f"required marker {marker!r} is not present on its own line")
    elif count > 1:
        problems.append(
            f"marker {marker!r} appears {count} times; exactly one is a verdict"
        )
    prefix = None
    for word in TERMINAL_WORDS:
        if marker.endswith("_" + word):
            prefix = marker[: -len(word) - 1]
            break
    if prefix:
        for word in TERMINAL_WORDS:
            other = f"{prefix}_{word}"
            if other != marker and marker_lines(output, other):
                problems.append(
                    f"output also asserts {other!r}; a run cannot claim two verdicts"
                )
    return not problems, problems


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def repo_path(root: Path, value: str) -> Path:
    candidate = (root / value).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as exc:
        raise ValueError(f"path escapes repository: {value}") from exc
    return candidate


def load_json_object(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as stream:
        value = json.load(stream)
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


# --- run binding ----------------------------------------------------------


def _git(root: Path, *args: str) -> str:
    try:
        completed = subprocess.run(
            ["git", *args],
            cwd=root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            timeout=60,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return completed.stdout if completed.returncode == 0 else ""


def git_binding(root: Path) -> dict[str, Any]:
    """Commit plus an exact digest of how the working tree differs from it.

    ``git status`` cannot see paths marked assume-unchanged or skip-worktree.
    An earlier version reported the COUNT of those alongside the digest, which
    names the blind spot without closing it: a run could rewrite ``src/nn.c``
    and still produce an identical binding. Their bytes are now bound too.
    """
    commit = _git(root, "rev-parse", "HEAD").strip() or "unknown"
    status = _git(root, "status", "--porcelain=v1")
    digest = hashlib.sha256()
    dirty_files = 0
    for line in status.splitlines():
        if len(line) < 4:
            continue
        entry = line[3:].split(" -> ")[-1].strip().strip('"')
        digest.update(line[:3].encode("utf-8"))
        digest.update(entry.encode("utf-8"))
        digest.update(path_identity(root / entry))
        dirty_files += 1
    special, special_files, special_bytes = special_index_binding(root)
    return {
        "commit": commit,
        "worktree_sha256": digest.hexdigest(),
        "worktree_dirty_files": dirty_files,
        "special_index_sha256": special,
        "special_index_files": special_files,
        "special_index_bytes": special_bytes,
    }


# Knobs whose VALUES may be written into a report. Each is a documented,
# non-secret switch or path; everything else CNET_*/CCE_* is recorded by NAME
# only. The previous version serialized every CNET_*/CCE_* value, which is a
# leak waiting for the first operator who names a token or an auth header with
# a project prefix -- and `logs/capability_cert.json` is a file people paste.
ENV_VALUE_ALLOWLIST = frozenset(
    {
        "CNET_HELD_OUT_FIXTURE",
        "CNET_COVERAGE_ABSTAIN",
        "CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE",
        "CNET_BASE_PATH",
        "CNET_PROMOTE_EVAL_DELTA",
        "CNET_ORACLE_INT8",
        "CNET_TRAIN_FAST",
        "CNET_GGUF_MMAP",
        "CNET_REQUIRE_REAL_MODEL",
        "CCE_CLASSIFICATION_LANE_REQUIRE",
    }
)


def environment_binding(environment: dict[str, str]) -> dict[str, Any]:
    """Bind the environment without copying any of it into the report.

    The digest covers every variable, so the run is bound. Only allowlisted
    non-secret knobs have their values recorded; every other CNET_*/CCE_* knob
    contributes its NAME and a per-value digest, which is enough to notice that
    it changed without disclosing what it is.
    """
    serialized = "\n".join(f"{key}={environment[key]}" for key in sorted(environment))
    cnet_names = sorted(
        key for key in environment if key.startswith(("CNET_", "CCE_"))
    )
    allowlisted = {
        key: environment[key] for key in cnet_names if key in ENV_VALUE_ALLOWLIST
    }
    redacted = {
        key: "sha256:" + hashlib.sha256(environment[key].encode("utf-8")).hexdigest()
        for key in cnet_names
        if key not in ENV_VALUE_ALLOWLIST
    }
    return {
        "env_sha256": hashlib.sha256(serialized.encode("utf-8")).hexdigest(),
        "env_variables": len(environment),
        "env_knob_names": cnet_names,
        "env_allowlisted_knobs": allowlisted,
        "env_redacted_knobs": redacted,
    }


# --- pre/post state binding ------------------------------------------------


def _digest_file(path: Path) -> str:
    return sha256(path) if path.is_file() else "absent"


def path_identity(path: Path) -> bytes:
    """Content identity that never follows a link and never opens a device.

    `Path.is_file()` follows symlinks, so a link to `/dev/zero` inside a bound
    set would read until the run died, and a link pointing outside the
    repository would bind bytes this tree does not own. A link is bound by
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
        return bytes.fromhex(sha256(path))
    except OSError:
        return b"absent"


def special_index_binding(root: Path) -> tuple[str, int, int]:
    """Digest every path git was told to stop watching, plus its bytes.

    `git status` deliberately omits assume-unchanged (lowercase flag) and
    skip-worktree (`S`) entries -- suppressing them is what those bits are for.
    Recording only their COUNT, as this did before, proves how many blind spots
    there were, not that nothing moved inside one. In CNET that gap covered 83
    tracked paths, including the trainer and its public header.

    Ordinary cached paths (flag `H`) are skipped on purpose: a change to one
    appears in `git status` and is already bound by the dirty walk. The flag
    letter is part of the digest, so setting or clearing a bit is drift too.
    """
    digest = hashlib.sha256()
    entries: list[tuple[str, str]] = []
    for record in _git(root, "ls-files", "-v", "-z").split("\0"):
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


def untracked_binding(root: Path) -> str:
    """Digest of every untracked, non-ignored file's path and content.

    `git status --porcelain` names untracked paths but the earlier binding
    hashed only the status TEXT for them, so a new source file could change
    content between the pre and post capture without moving a single digest.
    """
    digest = hashlib.sha256()
    listing = _git(root, "ls-files", "--others", "--exclude-standard", "-z")
    for entry in sorted(item for item in listing.split("\0") if item):
        digest.update(entry.encode("utf-8"))
        digest.update(path_identity(root / entry))
    return digest.hexdigest()


def capture_state(
    root: Path,
    sources: list[str],
    fixture_path: Path,
    binary: str | None,
    include_binary: bool,
) -> dict[str, Any]:
    """Everything the certificate claims about the tree, captured at one instant.

    Taken twice per capability and compared: a mutation of a declared source,
    the fixture, the evaluator binary, a dirty tracked file, or an untracked
    file between the two captures invalidates the run. A one-sided hash proves
    only what was true at one instant, which is not what a certificate asserts.
    """
    state: dict[str, Any] = dict(git_binding(root))
    state["untracked_sha256"] = untracked_binding(root)
    source_sha, resolved = source_set_digest(root, sources)
    state["evaluator_source_set_sha256"] = source_sha
    state["evaluator_sources"] = resolved
    state["fixture_sha256"] = _digest_file(fixture_path)
    if include_binary and binary:
        state["evaluator_binary_sha256"] = _digest_file(repo_path(root, binary))
    return state


def compare_states(before: dict[str, Any], after: dict[str, Any]) -> list[str]:
    """Names of the bound components that moved between two captures."""
    drift: list[str] = []
    for key in sorted(set(before) | set(after)):
        if before.get(key) != after.get(key):
            drift.append(key)
    return drift


def source_set_digest(root: Path, sources: list[str]) -> tuple[str, list[str]]:
    """Digest the declared files that make this evaluator consume the fixture.

    A declared source that does not exist is an error: an under-bound report is
    worse than a red gate, because it looks like evidence.
    """
    digest = hashlib.sha256()
    resolved: list[str] = []
    for value in sorted(sources):
        path = repo_path(root, value)
        if not path.is_file():
            raise ValueError(f"declared evaluator source is missing: {value}")
        digest.update(value.encode("utf-8"))
        digest.update(bytes.fromhex(sha256(path)))
        resolved.append(value)
    return digest.hexdigest(), resolved


# --- manifest / fixture validation ----------------------------------------


def validate_manifest(root: Path, path: Path) -> tuple[dict[str, Any], Path, dict[str, Any]]:
    manifest = load_json_object(path)
    required = {
        "schema_version",
        "capability_id",
        "held_out_fixture",
        "evaluator",
        "evaluator_sources",
        "metric_source",
        "required_marker",
        "title",
        "owner",
        "evidence_artifact",
        "failure_envelope",
        "absolute_floor",
        "baseline_metric",
        "regression_budget",
    }
    missing = sorted(required - manifest.keys())
    if missing:
        raise ValueError(f"{path}: missing keys: {', '.join(missing)}")
    if manifest["schema_version"] != SCHEMA_VERSION:
        raise ValueError(f"{path}: unsupported schema_version")
    capability_id = manifest["capability_id"]
    if not isinstance(capability_id, str) or not CAPABILITY_ID.fullmatch(capability_id):
        raise ValueError(f"{path}: invalid capability_id")
    evaluator = manifest["evaluator"]
    if (
        not isinstance(evaluator, list)
        or not evaluator
        or not all(isinstance(arg, str) and arg and "\x00" not in arg for arg in evaluator)
        or evaluator[0] not in ALLOWED_EVALUATORS
    ):
        raise ValueError(f"{path}: evaluator must be a non-shell allowlisted argv")
    sources = manifest["evaluator_sources"]
    if (
        not isinstance(sources, list)
        or not sources
        or not all(isinstance(value, str) and value for value in sources)
    ):
        raise ValueError(f"{path}: evaluator_sources must be a nonempty path list")
    metric_source = manifest["metric_source"]
    if metric_source not in METRIC_SOURCES:
        raise ValueError(
            f"{path}: metric_source must be one of {sorted(METRIC_SOURCES)}"
        )
    if metric_source == "regex" and not manifest.get("metric_regex"):
        raise ValueError(f"{path}: metric_source=regex requires metric_regex")
    if metric_source == "heldout_receipt" and manifest.get("metric_regex"):
        raise ValueError(
            f"{path}: metric_source=heldout_receipt must not also declare "
            "metric_regex; one number, one source"
        )
    marker = manifest["required_marker"]
    if not isinstance(marker, str) or not marker:
        raise ValueError(f"{path}: required_marker must be nonempty")
    for field in ("title", "owner", "failure_envelope"):
        if not isinstance(manifest[field], str) or not manifest[field]:
            raise ValueError(f"{path}: {field} must be nonempty")
    evidence_artifact = repo_path(root, manifest["evidence_artifact"])
    if evidence_artifact.suffix != ".log":
        raise ValueError(f"{path}: evidence_artifact must be a log path")
    for field in ("absolute_floor", "baseline_metric", "regression_budget"):
        value = manifest[field]
        if not isinstance(value, (int, float)) or value < 0:
            raise ValueError(f"{path}: {field} must be nonnegative")
    binary = manifest.get("evaluator_binary")
    if binary is not None and (not isinstance(binary, str) or not binary):
        raise ValueError(f"{path}: evaluator_binary must be a nonempty path")
    # An evaluator that does not build itself must say how it gets built. A
    # manifest that stays silent is one whose gate can only pass on a machine
    # that happens to hold the right ignored build state.
    declaration = declaration_problems(manifest)
    if declaration:
        raise ValueError(f"{path}: " + "; ".join(declaration))
    fixture_path = repo_path(root, manifest["held_out_fixture"])
    fixture = load_json_object(fixture_path)
    if fixture.get("capability_id") != capability_id:
        raise ValueError(f"{fixture_path}: capability_id mismatch")
    cases = fixture.get("cases")
    markers = fixture.get("expected_markers")
    if not isinstance(cases, list) or not cases:
        raise ValueError(f"{fixture_path}: held-out cases must be nonempty")
    # Stable per-case ids are what make the receipt checkable at all.
    ids: list[str] = []
    for case in cases:
        if not isinstance(case, dict):
            raise ValueError(f"{fixture_path}: every case must be an object")
        case_id = case.get("id")
        if not isinstance(case_id, str) or not CASE_ID.fullmatch(case_id):
            raise ValueError(f"{fixture_path}: every case needs a stable id")
        ids.append(case_id)
    if len(set(ids)) != len(ids):
        raise ValueError(f"{fixture_path}: case ids must be unique")
    if not isinstance(markers, list) or not markers or not all(
        isinstance(item, str) and item for item in markers
    ):
        raise ValueError(f"{fixture_path}: expected_markers must be nonempty strings")
    return manifest, fixture_path, fixture


# --- receipt / metric -----------------------------------------------------


def check_receipt(
    output: str, capability_id: str, fixture_sha: str, case_ids: list[str]
) -> tuple[bool, list[str], dict[str, Any]]:
    """Verify the evaluator proved it consumed THIS fixture's declared cases.

    Cardinality is part of the proof. `re.search` takes the FIRST match, so an
    evaluator that emitted two receipts -- or a run that concatenated two
    evaluators' output -- was judged on whichever appeared first, and duplicate
    per-case lines collapsed silently into a dict. Exactly one fixture receipt,
    exactly one metric receipt, and exactly one line per declared case, with no
    line for a case the fixture does not declare.
    """
    problems: list[str] = []
    detail: dict[str, Any] = {"receipt_present": False}
    receipts = RECEIPT_RE.findall(output)
    if not receipts:
        problems.append("no HELDOUT_FIXTURE receipt in evaluator output")
        return False, problems, detail
    if len(receipts) > 1:
        problems.append(
            f"{len(receipts)} HELDOUT_FIXTURE receipts in one run; exactly one is a proof"
        )
        detail["receipt_count"] = len(receipts)
        return False, problems, detail
    receipt = RECEIPT_RE.search(output)
    assert receipt is not None
    detail["receipt_present"] = True
    detail["receipt_capability"] = receipt.group(1)
    detail["receipt_fixture_sha256"] = receipt.group(2)
    detail["receipt_cases"] = int(receipt.group(3))
    detail["receipt_consumed"] = int(receipt.group(4))
    if receipt.group(1) != capability_id:
        problems.append(
            f"receipt names capability {receipt.group(1)}, expected {capability_id}"
        )
    if receipt.group(2) != fixture_sha:
        problems.append(
            f"receipt binds fixture {receipt.group(2)}, supplied {fixture_sha}"
        )
    if int(receipt.group(3)) != len(case_ids):
        problems.append(
            f"receipt declares {receipt.group(3)} cases, fixture has {len(case_ids)}"
        )
    if int(receipt.group(4)) != len(case_ids):
        problems.append(
            f"only {receipt.group(4)}/{len(case_ids)} declared cases were consumed"
        )

    case_lines: list[tuple[str, int]] = [
        (match.group(1), int(match.group(2))) for match in CASE_RE.finditer(output)
    ]
    reads: dict[str, int] = {}
    seen_twice: list[str] = []
    for case_id, count in case_lines:
        if case_id in reads:
            seen_twice.append(case_id)
        reads[case_id] = count
    detail["case_reads"] = reads
    detail["case_line_count"] = len(case_lines)
    for case_id in sorted(set(seen_twice)):
        problems.append(f"case {case_id} is reported more than once")
    for case_id in sorted(set(reads) - set(case_ids)):
        problems.append(
            f"receipt reports case {case_id}, which the fixture does not declare"
        )
    if len(case_lines) != len(case_ids):
        problems.append(
            f"receipt has {len(case_lines)} case lines for {len(case_ids)} declared cases"
        )
    for case_id in case_ids:
        if case_id not in reads:
            problems.append(f"case {case_id} is absent from the receipt")
        elif reads[case_id] == 0:
            problems.append(f"case {case_id} was declared but never read")

    metric_lines = RECEIPT_METRIC_RE.findall(output)
    if len(metric_lines) > 1:
        problems.append(
            f"{len(metric_lines)} HELDOUT_METRIC lines in one run; exactly one is a proof"
        )
        detail["metric_line_count"] = len(metric_lines)
        return False, problems, detail
    metric_line = RECEIPT_METRIC_RE.search(output)
    if metric_line is None:
        problems.append("no HELDOUT_METRIC line in evaluator output")
    else:
        detail["cases_passed"] = int(metric_line.group(1))
        detail["cases_declared"] = int(metric_line.group(2))
        detail["receipt_metric"] = float(metric_line.group(3))
        detail["receipt_errors"] = int(metric_line.group(4))
        if detail["receipt_errors"]:
            problems.append(
                f"evaluator reported {detail['receipt_errors']} fixture read errors"
            )
        if detail["cases_declared"] != len(case_ids):
            problems.append(
                f"receipt metric covers {detail['cases_declared']} cases, "
                f"fixture has {len(case_ids)}"
            )
        if detail["cases_passed"] != len(case_ids):
            problems.append(
                f"{detail['cases_passed']}/{len(case_ids)} declared cases passed"
            )
    return not problems, problems, detail


def extract_metric(manifest: dict[str, Any], output: str, receipt: dict[str, Any]) -> float:
    """The certified number. There is no default: an unmeasured metric is 0.0."""
    if manifest["metric_source"] == "heldout_receipt":
        if "receipt_metric" not in receipt:
            raise ValueError("metric_source=heldout_receipt but no receipt metric")
        return float(receipt["receipt_metric"])
    pattern = manifest["metric_regex"]
    if not isinstance(pattern, str) or len(pattern) > 256:
        raise ValueError("metric_regex must be a bounded string")
    compiled = re.compile(pattern)
    matches = list(compiled.finditer(output))
    if not matches:
        raise ValueError("metric_regex did not match the evaluator output")
    # `re.search` silently took the first of several. Two matches mean two
    # candidate metrics and no way to know which the certificate is about.
    if len(matches) > 1:
        raise ValueError(
            f"metric_regex matched {len(matches)} times; exactly one is a measurement"
        )
    match = matches[0]
    if match.lastindex != 1:
        raise ValueError("metric_regex did not produce exactly one capture")
    return float(match.group(1))


def write_json_atomic(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=f".{path.name}.", dir=path.parent
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(value, stream, indent=2, sort_keys=True)
            stream.write("\n")
        os.replace(temporary_name, path)
    except BaseException:
        try:
            os.unlink(temporary_name)
        except FileNotFoundError:
            pass
        raise


def run_manifest(root: Path, manifest_path: Path, run: dict[str, Any]) -> dict[str, Any]:
    manifest, fixture_path, fixture = validate_manifest(root, manifest_path)
    capability_id = manifest["capability_id"]
    binary = manifest.get("evaluator_binary")
    sources = list(manifest["evaluator_sources"])
    environment = os.environ.copy()
    environment["CNET_HELD_OUT_FIXTURE"] = str(fixture_path)
    timeout_seconds = int(manifest.get("timeout_seconds", 300))
    if timeout_seconds < 1 or timeout_seconds > 1800:
        raise ValueError(f"{manifest_path}: timeout_seconds outside 1..1800")

    # Make the evaluator runnable, or refuse before running it. This happens
    # before the pre-capture on purpose: a prepare step writes build output,
    # and build output is ignored, so binding it as "state that must not move"
    # would fail every run that legitimately compiled something.
    ready_problems = ensure_ready(root, manifest)
    if ready_problems:
        return {
            "capability_id": capability_id,
            "status": "failed",
            "return_code": None,
            "metric": 0.0,
            "metric_source": manifest["metric_source"],
            "receipt_ok": False,
            "receipt_problems": [
                f"evaluator is not ready: {problem}" for problem in ready_problems
            ],
            "markers_ok": False,
            "missing_markers": [],
            "evaluator_argv": list(manifest["evaluator"]),
            "evaluator_prepare": manifest.get("evaluator_prepare"),
            "evaluator_binary": binary,
            "run_id": run["run_id"],
        }

    # Capture BEFORE the evaluator runs. The binary is excluded here because the
    # recipe legitimately builds it; everything else must be identical after.
    pre = capture_state(root, sources, fixture_path, binary, include_binary=False)
    started = time.time()
    completed = subprocess.run(
        manifest["evaluator"],
        cwd=root,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout_seconds,
        check=False,
    )
    duration = time.time() - started

    # Capture immediately after the evaluator exits: this is the state that
    # actually produced the receipt, binary included.
    post_run = capture_state(root, sources, fixture_path, binary,
                             include_binary=True)

    evidence_path = repo_path(root, manifest["evidence_artifact"])
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    evidence_path.write_text(completed.stdout, encoding="utf-8")

    fixture_sha = sha256(fixture_path)
    case_ids = [case["id"] for case in fixture["cases"]]
    receipt_ok, receipt_problems, receipt = check_receipt(
        completed.stdout, capability_id, fixture_sha, case_ids
    )
    # The manifest's required_marker IS the verdict, so it is held to the
    # anchored, single-occurrence, no-rival-verdict rule. A fixture's
    # expected_markers are field probes ("acc_on=", "semantic=2") that are meant
    # to match inside a line, so they stay substring checks -- EXCEPT when one
    # is shaped like a verdict itself, which would otherwise be a way to smuggle
    # a terminal claim through the weaker path.
    missing_markers: list[str] = []
    marker_problems: list[str] = []
    found, issues = terminal_marker_ok(completed.stdout, manifest["required_marker"])
    if not found:
        missing_markers.append(manifest["required_marker"])
        marker_problems.extend(issues)
    for marker in fixture["expected_markers"]:
        if looks_terminal(marker):
            found, issues = terminal_marker_ok(completed.stdout, marker)
            if not found:
                missing_markers.append(marker)
                marker_problems.extend(issues)
        elif marker not in completed.stdout:
            missing_markers.append(marker)
    markers_ok = not missing_markers
    receipt_problems.extend(marker_problems)

    # The metric is only meaningful once the receipt proves which cases ran.
    metric = 0.0
    metric_error: str | None = None
    if receipt_ok and markers_ok:
        try:
            metric = extract_metric(manifest, completed.stdout, receipt)
        except ValueError as exc:
            metric_error = str(exc)
    floor = float(manifest["absolute_floor"])
    regression_floor = (
        float(manifest["baseline_metric"]) - float(manifest["regression_budget"])
    )
    passed = (
        completed.returncode == 0
        and receipt_ok
        and markers_ok
        and metric_error is None
        and metric >= floor
        and metric >= regression_floor
    )

    binary_sha = post_run.get("evaluator_binary_sha256")
    if binary and binary_sha in (None, "absent"):
        passed = False
        receipt_problems.append(f"declared evaluator binary is absent: {binary}")

    # Capture again once every digest above has been taken. A source, fixture,
    # binary, dirty tracked file or untracked file that moved after the receipt
    # was accepted invalidates the certificate: the report would describe a tree
    # that no longer exists.
    post_bind = capture_state(root, sources, fixture_path, binary,
                              include_binary=True)
    pre_drift = [
        key for key in compare_states(pre, post_run)
        if key != "evaluator_binary_sha256"
    ]
    post_drift = compare_states(post_run, post_bind)
    binding_drift = sorted(set(pre_drift) | set(post_drift))
    if binding_drift:
        passed = False
        receipt_problems.append(
            "bound state changed during the run: " + ", ".join(binding_drift)
        )

    return {
        "capability_id": capability_id,
        "status": "certified" if passed else "failed",
        "return_code": completed.returncode,
        "metric": metric,
        "metric_source": manifest["metric_source"],
        "metric_error": metric_error,
        "absolute_floor": floor,
        "regression_floor": regression_floor,
        "fixture_sha256": fixture_sha,
        "manifest_sha256": sha256(manifest_path),
        "evidence_sha256": sha256(evidence_path),
        "evidence_log": str(evidence_path.relative_to(root)),
        "markers_ok": markers_ok,
        "missing_markers": missing_markers,
        "receipt_ok": receipt_ok,
        "receipt_problems": receipt_problems,
        "receipt": receipt,
        "declared_case_ids": case_ids,
        "evaluator_argv": list(manifest["evaluator"]),
        "evaluator_prepare": manifest.get("evaluator_prepare"),
        "evaluator_sources": sources,
        "evaluator_source_set_sha256": post_run["evaluator_source_set_sha256"],
        "evaluator_binary": binary,
        "evaluator_binary_sha256": binary_sha,
        "binding_pre": pre,
        "binding_post_run": post_run,
        "binding_post_bind": post_bind,
        "binding_stable": not binding_drift,
        "binding_drift": binding_drift,
        "duration_seconds": round(duration, 3),
        "run_id": run["run_id"],
        **environment_binding(environment),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--manifest-dir", default="config/capability_manifests"
    )
    parser.add_argument(
        "--output", default="logs/capability_cert.json"
    )
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    manifest_dir = repo_path(root, args.manifest_dir)
    output_path = repo_path(root, args.output)
    manifests = sorted(manifest_dir.glob("*.json"))
    if not manifests:
        print("CAPABILITY_CERT_FAIL reason=no_manifests", file=sys.stderr)
        return 1
    run = {
        "run_id": str(uuid.uuid4()),
        "started_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        **git_binding(root),
    }
    if run["special_index_bytes"] > SPECIAL_INDEX_BYTE_CAP:
        print(
            "CAPABILITY_CERT_FAIL reason=special_index_too_large:"
            f"{run['special_index_bytes']}>{SPECIAL_INDEX_BYTE_CAP} "
            "refusing to certify against a binding that does not cover the tree",
            file=sys.stderr,
        )
        return 1
    print(
        f"CAPABILITY_CERT_RUN run_id={run['run_id']} started={run['started_at']} "
        f"commit={run['commit']} worktree={run['worktree_sha256'][:16]} "
        f"dirty={run['worktree_dirty_files']} "
        f"special_index={run['special_index_sha256'][:16]} "
        f"special_index_files={run['special_index_files']}"
    )
    results: list[dict[str, Any]] = []
    seen_capabilities: set[str] = set()
    for manifest_path in manifests:
        try:
            result = run_manifest(root, manifest_path, run)
            if result["capability_id"] in seen_capabilities:
                raise ValueError(
                    f"duplicate capability_id: {result['capability_id']}"
                )
            seen_capabilities.add(result["capability_id"])
        except (OSError, ValueError, json.JSONDecodeError, subprocess.TimeoutExpired) as exc:
            result = {
                "capability_id": manifest_path.stem,
                "status": "failed",
                "error": str(exc),
                "run_id": run["run_id"],
            }
        results.append(result)
        for problem in result.get("receipt_problems", []):
            print(f"CAPABILITY_RECEIPT id={result['capability_id']} problem={problem}")
        for marker in result.get("missing_markers", []):
            print(
                f"CAPABILITY_MARKER id={result['capability_id']} missing={marker!r}"
            )
        if result.get("error"):
            print(f"CAPABILITY_ERROR id={result['capability_id']} {result['error']}")
        print(
            f"CAPABILITY id={result['capability_id']} "
            f"status={result['status']} metric={result.get('metric', 0.0):.3f} "
            f"source={result.get('metric_source', 'none')} "
            f"receipt={'ok' if result.get('receipt_ok') else 'missing'}"
        )
    certified = sum(result["status"] == "certified" for result in results)
    report = {
        "schema_version": SCHEMA_VERSION,
        "certified": certified,
        "total": len(results),
        "run": run,
        "results": results,
    }
    write_json_atomic(output_path, report)
    if certified != len(results):
        print(f"CAPABILITY_CERT_FAIL certified={certified}/{len(results)}")
        return 1
    print(
        f"CAPABILITY_CERT_PASS certified={certified}/{len(results)} "
        f"run_id={run['run_id']} commit={run['commit']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
