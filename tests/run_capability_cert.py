#!/usr/bin/env python3
"""Run committed held-out capability manifests without invoking a shell."""

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
from typing import Any

SCHEMA_VERSION = 1
ALLOWED_EVALUATORS = {"make", "dotnet"}
CAPABILITY_ID = re.compile(r"^[a-z][a-z0-9_]{2,63}$")


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


def validate_manifest(root: Path, path: Path) -> tuple[dict[str, Any], Path, dict[str, Any]]:
    manifest = load_json_object(path)
    required = {
        "schema_version",
        "capability_id",
        "held_out_fixture",
        "evaluator",
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
    fixture_path = repo_path(root, manifest["held_out_fixture"])
    fixture = load_json_object(fixture_path)
    if fixture.get("capability_id") != capability_id:
        raise ValueError(f"{fixture_path}: capability_id mismatch")
    cases = fixture.get("cases")
    markers = fixture.get("expected_markers")
    if not isinstance(cases, list) or not cases:
        raise ValueError(f"{fixture_path}: held-out cases must be nonempty")
    if not isinstance(markers, list) or not markers or not all(
        isinstance(item, str) and item for item in markers
    ):
        raise ValueError(f"{fixture_path}: expected_markers must be nonempty strings")
    return manifest, fixture_path, fixture


def extract_metric(manifest: dict[str, Any], output: str) -> float:
    pattern = manifest.get("metric_regex")
    if pattern is None:
        return 1.0
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


def run_manifest(root: Path, manifest_path: Path) -> dict[str, Any]:
    manifest, fixture_path, fixture = validate_manifest(root, manifest_path)
    capability_id = manifest["capability_id"]
    environment = os.environ.copy()
    environment["CNET_HELD_OUT_FIXTURE"] = str(fixture_path)
    timeout_seconds = int(manifest.get("timeout_seconds", 300))
    if timeout_seconds < 1 or timeout_seconds > 1800:
        raise ValueError(f"{manifest_path}: timeout_seconds outside 1..1800")
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
    evidence_path = repo_path(root, manifest["evidence_artifact"])
    evidence_path.parent.mkdir(parents=True, exist_ok=True)
    evidence_path.write_text(completed.stdout, encoding="utf-8")
    markers = [manifest["required_marker"], *fixture["expected_markers"]]
    markers_ok = all(marker in completed.stdout for marker in markers)
    metric = extract_metric(manifest, completed.stdout) if markers_ok else 0.0
    floor = float(manifest["absolute_floor"])
    regression_floor = (
        float(manifest["baseline_metric"]) - float(manifest["regression_budget"])
    )
    passed = (
        completed.returncode == 0
        and markers_ok
        and metric >= floor
        and metric >= regression_floor
    )
    return {
        "capability_id": capability_id,
        "status": "certified" if passed else "failed",
        "return_code": completed.returncode,
        "metric": metric,
        "absolute_floor": floor,
        "regression_floor": regression_floor,
        "fixture_sha256": sha256(fixture_path),
        "manifest_sha256": sha256(manifest_path),
        "evidence_sha256": sha256(evidence_path),
        "evidence_log": str(evidence_path.relative_to(root)),
        "markers_ok": markers_ok,
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
    results: list[dict[str, Any]] = []
    seen_capabilities: set[str] = set()
    for manifest_path in manifests:
        try:
            result = run_manifest(root, manifest_path)
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
            }
        results.append(result)
        print(
            f"CAPABILITY id={result['capability_id']} "
            f"status={result['status']} metric={result.get('metric', 0.0):.3f}"
        )
    certified = sum(result["status"] == "certified" for result in results)
    report = {
        "schema_version": SCHEMA_VERSION,
        "certified": certified,
        "total": len(results),
        "results": results,
    }
    write_json_atomic(output_path, report)
    if certified != len(results):
        print(f"CAPABILITY_CERT_FAIL certified={certified}/{len(results)}")
        return 1
    print(f"CAPABILITY_CERT_PASS certified={certified}/{len(results)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
