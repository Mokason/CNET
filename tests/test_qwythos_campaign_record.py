#!/usr/bin/env python3
"""Verify the committed Qwythos campaign record without rewriting history."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
manifest_path = root / "qwythos_english_v1.cnb.manifest.json"
makefile = (root / "Makefile").read_text(encoding="utf-8")
failures: list[str] = []


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


try:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
except (OSError, json.JSONDecodeError) as exc:
    print(f"QWYTHOS_CAMPAIGN_RECORD_FAIL: unreadable manifest: {exc}", file=sys.stderr)
    raise SystemExit(1)

required = {
    "provenance_version", "build_rev", "source_dirty", "executable_sha256",
    "model", "model_sha256", "window_source", "window_sha256",
    "oracle_golden", "oracle_golden_sha256", "base", "base_sha256",
}
missing = sorted(required - manifest.keys())
if missing:
    failures.append(f"manifest missing fields: {', '.join(missing)}")
if manifest.get("provenance_version") != 1:
    failures.append("unsupported provenance_version")
if manifest.get("source_dirty") != 1:
    failures.append("historical record must retain source_dirty=1 truth")
for field in ("executable_sha256", "model_sha256", "window_sha256",
              "oracle_golden_sha256", "base_sha256"):
    value = manifest.get(field, "")
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{64}", value) is None:
        failures.append(f"{field} is not lowercase SHA-256")

for path_field, hash_field in (
    ("window_source", "window_sha256"),
    ("oracle_golden", "oracle_golden_sha256"),
):
    artifact = root / str(manifest.get(path_field, ""))
    if not artifact.is_file():
        failures.append(f"committed record artifact missing: {path_field}")
    elif sha256(artifact) != manifest.get(hash_field):
        failures.append(f"{path_field} content disagrees with {hash_field}")

base = root / str(manifest.get("base", ""))
base_present = base.is_file()
if base_present and sha256(base) != manifest.get("base_sha256"):
    failures.append("local base content disagrees with base_sha256")

sha_file = root / "qwythos_english_v1.cnb.sha256"
try:
    recorded_base_hash = sha_file.read_text(encoding="utf-8").split()[0]
except (OSError, IndexError) as exc:
    failures.append(f"base sha256 sidecar unreadable: {exc}")
else:
    if recorded_base_hash != manifest.get("base_sha256"):
        failures.append("base SHA sidecar disagrees with manifest")

tracked_executable = subprocess.run(
    ["git", "ls-files", "--error-unmatch", "bin/flagship_run"],
    cwd=root, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
).returncode == 0
if tracked_executable:
    failures.append("historical flagship executable unexpectedly became tracked")

match = re.search(
    r"^campaign_provenance:[^\n]*\n(?P<body>(?:\t.*\n|\n)+)",
    makefile, flags=re.MULTILINE,
)
body = match.group("body") if match else ""
if not match:
    failures.append("Makefile has no campaign_provenance target")
if "test_qwythos_campaign_record.py" not in body:
    failures.append("campaign_provenance does not run record-integrity verification")
if "bin/flagship_run" in body:
    failures.append("campaign_provenance compares historical record to live executable")

if failures:
    for failure in failures:
        print(f"QWYTHOS_CAMPAIGN_RECORD_FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)

print(
    "QWYTHOS_CAMPAIGN_RECORD_PASS "
    f"base_present={int(base_present)} replayable=0 "
    "reason=dirty_source_and_historical_executable_not_retained"
)
