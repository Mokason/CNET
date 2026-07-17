#!/usr/bin/env python3
"""Fail closed when CNET's Apache-2.0 metadata becomes inconsistent."""
from pathlib import Path
import hashlib
import re
import sys
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[1]
failures: list[str] = []

license_path = root / "LICENSE"
license_text = license_path.read_text(encoding="utf-8") if license_path.exists() else ""
required_license_fragments = [
    "Apache License",
    "Version 2.0, January 2004",
    "TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION",
    "2. Grant of Copyright License.",
    "3. Grant of Patent License.",
    "4. Redistribution.",
    "7. Disclaimer of Warranty.",
    "8. Limitation of Liability.",
    "END OF TERMS AND CONDITIONS",
]
for fragment in required_license_fragments:
    if fragment not in license_text:
        failures.append(f"LICENSE omits canonical fragment: {fragment}")
license_sha256 = hashlib.sha256(license_text.encode("utf-8")).hexdigest()
if license_sha256 != "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30":
    failures.append(f"LICENSE is not the pinned canonical Apache-2.0 text: {license_sha256}")

readme = (root / "README.md").read_text(encoding="utf-8")
if not re.search(r"## License\s+.*Apache-2\.0.*\[LICENSE\]\(LICENSE\)", readme, re.DOTALL):
    failures.append("README does not identify Apache-2.0 and link LICENSE")

policy = (root / "docs" / "RELEASE_POLICY.md").read_text(encoding="utf-8")
if "Apache-2.0" not in policy:
    failures.append("release policy does not identify Apache-2.0")
if "remain private" not in policy:
    failures.append("release policy does not preserve private status")
if "no public license is granted" in policy:
    failures.append("release policy still denies the configured license grant")

project_path = root / "dotnet" / "Cce" / "Cce.csproj"
try:
    project = ET.parse(project_path)
    expressions = [
        (node.text or "").strip()
        for node in project.findall(".//PackageLicenseExpression")
    ]
except (ET.ParseError, OSError) as exc:
    failures.append(f"cannot parse Cce.csproj: {exc}")
    expressions = []
if expressions != ["Apache-2.0"]:
    failures.append("Cce.csproj PackageLicenseExpression must be Apache-2.0")

makefile = (root / "Makefile").read_text(encoding="utf-8")
if not re.search(r"^license_metadata_test:\s*", makefile, re.MULTILINE):
    failures.append("Makefile has no license_metadata_test target")
release = re.search(
    r"^release_integrity:[^\n]*\n(?P<body>(?:\t.*\n|\n)+)",
    makefile,
    re.MULTILINE,
)
runner_path = root / "tests" / "run_release_integrity.sh"
runner = runner_path.read_text(encoding="utf-8") if runner_path.exists() else ""
if not release or "bash tests/run_release_integrity.sh" not in release.group("body"):
    failures.append("release_integrity does not delegate to the strict runner")
if "run_make license_metadata_test" not in runner:
    failures.append("release_integrity does not run license_metadata_test")

if failures:
    for failure in failures:
        print(f"LICENSE_METADATA_FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)

print("LICENSE_METADATA_PASS")
