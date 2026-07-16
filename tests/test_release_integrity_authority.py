#!/usr/bin/env python3
"""Static authority test for the single CNET 5.1.1 release gate."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[1]
makefile = (root / "Makefile").read_text(encoding="utf-8")
policy = (root / "docs" / "RELEASE_POLICY.md").read_text(encoding="utf-8")
version = (root / "VERSION").read_text(encoding="utf-8").strip()
header = (root / "include" / "cnet_version.h").read_text(encoding="utf-8")

failures: list[str] = []
match = re.search(
    r"^release_integrity:\s*\n(?P<body>(?:\t.*\n|\n)+)",
    makefile,
    flags=re.MULTILINE,
)
if not match:
    failures.append("Makefile has no executable release_integrity recipe")
    body = ""
else:
    body = match.group("body")

if not re.search(r"^mcp_protocol_survival:\s*", makefile, flags=re.MULTILINE):
    failures.append("Makefile references but does not define mcp_protocol_survival")

required_in_order = [
    "gguf_integrity",
    "model_runtime_integrity",
    "specialist_authority",
    "persistence_integrity",
    "mcp_protocol_survival",
    "release_package",
    "PORTABLE=1 ci_core",
    "priority_acceptance",
    "git diff --check",
    "git status --porcelain --untracked-files=no",
]
last = -1
for required in required_in_order:
    pos = body.find(required)
    if pos < 0:
        failures.append(f"release_integrity omits {required}")
    elif pos <= last:
        failures.append(f"release_integrity runs {required} out of order")
    else:
        last = pos

if "CNET_RELEASE_INTEGRITY_PASS" not in body:
    failures.append("release_integrity emits no terminal PASS marker")
if "|| echo" in body:
    failures.append("release_integrity swallows failure with || echo")
if "make release_integrity" not in policy:
    failures.append("release policy does not name make release_integrity")
if f'#define CNET_VERSION_STRING "{version}"' not in header:
    failures.append("VERSION and cnet_version.h disagree")

if failures:
    for failure in failures:
        print(f"RELEASE_INTEGRITY_AUTHORITY_FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)

print("RELEASE_INTEGRITY_AUTHORITY_PASS")
