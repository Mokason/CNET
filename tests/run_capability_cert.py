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
assume-unchanged count, evaluator argv, evaluator binary digest, declared
source-set digest, environment digest, run UUID and start time. A PASS that
cannot be tied back to the tree that produced it is not evidence.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import uuid
from typing import Any

SCHEMA_VERSION = 1
ALLOWED_EVALUATORS = {"make", "dotnet"}
CAPABILITY_ID = re.compile(r"^[a-z][a-z0-9_]{2,63}$")
CASE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
METRIC_SOURCES = {"heldout_receipt", "regex"}

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

    ``git status`` cannot see paths marked assume-unchanged, so the count of
    those is reported alongside rather than left as a silent blind spot.
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
        candidate = root / entry
        if candidate.is_file():
            digest.update(bytes.fromhex(sha256(candidate)))
        dirty_files += 1
    assume_unchanged = sum(
        1
        for line in _git(root, "ls-files", "-v").splitlines()
        if line[:1].islower()
    )
    return {
        "commit": commit,
        "worktree_sha256": digest.hexdigest(),
        "worktree_dirty_files": dirty_files,
        "assume_unchanged_files": assume_unchanged,
    }


def environment_binding(environment: dict[str, str]) -> dict[str, Any]:
    """Digest the whole environment; surface only the project's own knobs.

    The digest binds the run without copying arbitrary secrets into a report.
    """
    serialized = "\n".join(f"{key}={environment[key]}" for key in sorted(environment))
    knobs = {
        key: environment[key]
        for key in sorted(environment)
        if key.startswith(("CNET_", "CCE_"))
    }
    return {
        "env_sha256": hashlib.sha256(serialized.encode("utf-8")).hexdigest(),
        "env_variables": len(environment),
        "env_cnet_knobs": knobs,
    }


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
    """Verify the evaluator proved it consumed THIS fixture's declared cases."""
    problems: list[str] = []
    detail: dict[str, Any] = {"receipt_present": False}
    receipt = RECEIPT_RE.search(output)
    if receipt is None:
        problems.append("no HELDOUT_FIXTURE receipt in evaluator output")
        return False, problems, detail
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

    reads = {match.group(1): int(match.group(2)) for match in CASE_RE.finditer(output)}
    detail["case_reads"] = reads
    for case_id in case_ids:
        if case_id not in reads:
            problems.append(f"case {case_id} is absent from the receipt")
        elif reads[case_id] == 0:
            problems.append(f"case {case_id} was declared but never read")

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
    match = re.search(pattern, output)
    if not match or match.lastindex != 1:
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
    source_sha, sources = source_set_digest(root, manifest["evaluator_sources"])
    environment = os.environ.copy()
    environment["CNET_HELD_OUT_FIXTURE"] = str(fixture_path)
    timeout_seconds = int(manifest.get("timeout_seconds", 300))
    if timeout_seconds < 1 or timeout_seconds > 1800:
        raise ValueError(f"{manifest_path}: timeout_seconds outside 1..1800")
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
    evidence_path = repo_path(root, manifest["evidence_artifact"])
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    evidence_path.write_text(completed.stdout, encoding="utf-8")

    fixture_sha = sha256(fixture_path)
    case_ids = [case["id"] for case in fixture["cases"]]
    receipt_ok, receipt_problems, receipt = check_receipt(
        completed.stdout, capability_id, fixture_sha, case_ids
    )
    markers = [manifest["required_marker"], *fixture["expected_markers"]]
    missing_markers = [marker for marker in markers if marker not in completed.stdout]
    markers_ok = not missing_markers

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

    binary = manifest.get("evaluator_binary")
    binary_sha = None
    if binary:
        binary_path = repo_path(root, binary)
        binary_sha = sha256(binary_path) if binary_path.is_file() else "missing"
        if binary_sha == "missing":
            passed = False
            receipt_problems.append(f"declared evaluator binary is absent: {binary}")

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
        "evaluator_sources": sources,
        "evaluator_source_set_sha256": source_sha,
        "evaluator_binary": binary,
        "evaluator_binary_sha256": binary_sha,
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
    print(
        f"CAPABILITY_CERT_RUN run_id={run['run_id']} started={run['started_at']} "
        f"commit={run['commit']} worktree={run['worktree_sha256'][:16]} "
        f"dirty={run['worktree_dirty_files']} "
        f"assume_unchanged={run['assume_unchanged_files']}"
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
