#!/usr/bin/env python3
"""Behavioral authority test for the single CNET 5.1.1 release gate."""
from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
makefile = (root / "Makefile").read_text(encoding="utf-8")
policy = (root / "docs" / "RELEASE_POLICY.md").read_text(encoding="utf-8")
version = (root / "VERSION").read_text(encoding="utf-8").strip()
header = (root / "include" / "cnet_version.h").read_text(encoding="utf-8")
runner_path = root / "tests" / "run_release_integrity.sh"
runner = runner_path.read_text(encoding="utf-8") if runner_path.is_file() else ""

failures: list[str] = []
match = re.search(
    r"^release_integrity:[^\n]*\n(?P<body>(?:\t.*\n|\n)+)",
    makefile,
    flags=re.MULTILINE,
)
if not match:
    failures.append("Makefile has no executable release_integrity recipe")
    body = ""
else:
    body = match.group("body")

if "bash tests/run_release_integrity.sh" not in body:
    failures.append("release_integrity does not delegate to the fail-closed runner")
if not runner:
    failures.append("tests/run_release_integrity.sh is missing")
if not re.search(r"^mcp_protocol_survival:\s*", makefile, flags=re.MULTILINE):
    failures.append("Makefile references but does not define mcp_protocol_survival")

required_in_order = [
    "release_integrity_authority",
    "license_metadata_test",
    "real_model_control_plane_test",
    "phase123_benchmark_test",
    "phase5_integration_test",
    "gguf_integrity",
    "model_runtime_integrity",
    "specialist_authority",
    "persistence_integrity",
    "mcp_protocol_survival",
    "release_package",
    "PORTABLE=1 ci_core",
    "SKIP_RELEASE_PACKAGE=1 priority_acceptance",
    "git diff --check",
    "git diff --cached --check",
]
last = -1
for required in required_in_order:
    pos = runner.find(required)
    if pos < 0:
        failures.append(f"release runner omits {required}")
    elif pos <= last:
        failures.append(f"release runner runs {required} out of order")
    else:
        last = pos

status_positions = [
    found.start()
    for found in re.finditer("git status --porcelain --untracked-files=no", runner)
]
if len(status_positions) != 2:
    failures.append("release runner must check a clean tracked tree at start and end")
elif status_positions[0] > runner.find("release_integrity_authority"):
    failures.append("release runner clean-tree preflight runs too late")
elif status_positions[1] < runner.find("priority_acceptance"):
    failures.append("release runner clean-tree closure runs too early")
if "logs/verified-today.release.md" not in runner or \
        "git restore -- docs/verified-today.generated.md" not in runner:
    failures.append("release runner does not preserve and clean generated evidence")
if "set -Eeuo pipefail" not in runner:
    failures.append("release runner is not strict Bash")
if "CNET_RELEASE_INTEGRITY_PASS" not in runner:
    failures.append("release runner emits no terminal PASS marker")
if "|| echo" in runner:
    failures.append("release runner swallows failure with || echo")
if "make release_integrity" not in policy:
    failures.append("release policy does not name make release_integrity")
if f'#define CNET_VERSION_STRING "{version}"' not in header:
    failures.append("VERSION and cnet_version.h disagree")


def git(repo: Path, *args: str) -> None:
    subprocess.run(
        ["git", *args], cwd=repo, check=True,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def behavioral_failure_injection() -> None:
    if not runner:
        return
    with tempfile.TemporaryDirectory(prefix="cnet_release_authority_") as td:
        repo = Path(td)
        (repo / "tests").mkdir()
        copied_runner = repo / "tests" / "run_release_integrity.sh"
        shutil.copy2(runner_path, copied_runner)
        copied_runner.chmod(0o755)
        fake_make = repo / "fake-make"
        fake_make.write_text(
            "#!/usr/bin/env bash\n"
            "set -u\n"
            "printf '%s\\n' \"$*\" >> \"$CNET_RELEASE_CALL_LOG\"\n"
            "fail=${CNET_RELEASE_FAIL_TARGET:-}\n"
            "if [[ -n \"$fail\" && \" $* \" == *\" $fail \"* ]]; then exit 17; fi\n",
            encoding="utf-8",
        )
        fake_make.chmod(0o755)
        git(repo, "init", "-q")
        git(repo, "add", "tests/run_release_integrity.sh")
        subprocess.run(
            ["git", "-c", "user.name=CNET Gate Test",
             "-c", "user.email=gate-test@invalid", "commit", "-qm", "fixture"],
            cwd=repo, check=True,
        )
        call_log = repo / "calls.log"
        env = os.environ.copy()
        env.update({
            "CNET_RELEASE_MAKE": str(fake_make),
            "CNET_RELEASE_CALL_LOG": str(call_log),
            "CNET_RELEASE_FAIL_TARGET": "phase123_benchmark_test",
        })
        failed = subprocess.run(
            ["bash", "tests/run_release_integrity.sh"], cwd=repo, env=env,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        calls = call_log.read_text(encoding="utf-8").splitlines()
        failed_log = repo / "logs" / "release_integrity.log"
        if failed.returncode != 17:
            failures.append(
                f"injected make failure returned {failed.returncode}, expected 17"
            )
        if not any("phase123_benchmark_test" in call for call in calls):
            failures.append("failure injection never reached its target")
        if any("phase5_integration_test" in call for call in calls):
            failures.append("release runner continued after injected failure")
        if not failed_log.is_file():
            failures.append("release runner did not atomically publish failure log")
        elif "CNET_RELEASE_INTEGRITY_PASS" in failed_log.read_text(encoding="utf-8"):
            failures.append("release runner wrote PASS after injected failure")

        call_log.unlink(missing_ok=True)
        env.pop("CNET_RELEASE_FAIL_TARGET", None)
        passed = subprocess.run(
            ["bash", "tests/run_release_integrity.sh"], cwd=repo, env=env,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        success_calls = call_log.read_text(encoding="utf-8").splitlines()
        success_log = (repo / "logs" / "release_integrity.log").read_text(
            encoding="utf-8"
        )
        if passed.returncode != 0:
            failures.append(f"injected success path returned {passed.returncode}")
        if not success_calls or not any(
            "priority_acceptance" in call for call in success_calls
        ):
            failures.append("injected success path did not reach final make target")
        if not success_log.rstrip().endswith("CNET_RELEASE_INTEGRITY_PASS"):
            failures.append("injected success path lacks terminal PASS marker")


behavioral_failure_injection()

if failures:
    for failure in failures:
        print(f"RELEASE_INTEGRITY_AUTHORITY_FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)

print("RELEASE_INTEGRITY_AUTHORITY_PASS")
