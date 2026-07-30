#!/usr/bin/env python3
"""An evaluator must be BUILT before it is believed, or refused before it runs.

WHY THIS EXISTS. `make capability_cert` passed in the writer's worktree and
exited 2 in a fresh detached worktree of the *same commit*. The difference was
not in any tracked file: it was `dotnet/Cce.Llm.Tests/obj`, an ignored
build-state directory that the writer's tree happened to have and a fresh
checkout never does. Reproduced exactly, at 6f9c859, in a disposable worktree:

    $ dotnet test dotnet/Cce.Llm.Tests/CNET.Cce.Llm.Tests.csproj --no-restore \\
        --filter FullyQualifiedName~AutoRecall_GenuineEmpty_TellsModelNotToInvent \\
        --logger 'console;verbosity=normal'
    ##EXIT=0        0 bytes of output

Zero bytes, exit 0, no test run. `--no-restore` against a project that was never
restored is the silent-success mode `tests/run_capability_cert.py` names in its
own docstring -- and nothing in the tree ever built the thing it then asked to
run. The causality gate did fail closed (an evaluator that does not run cannot
emit a consumption receipt), so nothing was ever falsely certified; what did not
exist was any way for a fresh checkout to be certifiable at all.

The rule this module enforces has two halves:

* DECLARATION. An evaluator that is not itself a build command must declare
  `evaluator_prepare`: the argv, non-shell and allowlisted, that makes it
  runnable from a clean checkout. `make TARGET` is exempt because make *is* a
  build system -- it rebuilds its target from its prerequisites and fails loudly
  when it cannot, which is why every `make` capability survived the fresh
  worktree untouched. `dotnet test --no-restore` is not, and proved it.
* READINESS. `evaluator_prepare` runs to success, and afterwards the declared
  `evaluator_binary` must exist and be no older than every declared
  `evaluator_source`. A prepare that "succeeds" without producing the binary, or
  that leaves output older than the sources that define what the evaluator
  does, is stale output; using it silently is the failure this closes.

Nothing here lowers a floor or relaxes an expectation. A tree that cannot build
its evaluator now says so, by name, before the evaluator runs.

Usage: python3 scripts/capability_evaluator_prereq.py [capability_id ...]
Exit 0 = every capability is ready, 1 = at least one is not.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
MANIFEST_DIR = ROOT / "config" / "capability_manifests"

# Same allowlist the evaluator argv itself is held to: no shell, no interpreter
# that would take a string and run it.
ALLOWED_PREPARE = {"make", "dotnet"}

# Evaluator argv[0] values that ARE a build: invoking them on a target rebuilds
# it from its prerequisites and fails loudly when it cannot. Anything outside
# this set has to say how it gets built.
SELF_BUILDING = {"make"}

# A prepare step builds; it does not train, mine or serve. Ten minutes is far
# above `dotnet build` on this project (measured 5.5s cold and offline, in a
# fresh worktree with no NuGet assets) and far below anything that would hide a
# hang: an unbounded wait is how a stuck build reads as a slow one.
PREPARE_TIMEOUT_SECONDS = 600


def repo_path(root: Path, value: str) -> Path:
    """Resolve a manifest-declared path, refusing anything outside the tree."""
    candidate = (root / value).resolve()
    if candidate != root and root not in candidate.parents:
        raise ValueError(f"path escapes the repository: {value}")
    return candidate


def declaration_problems(manifest: dict[str, Any]) -> list[str]:
    """Static rule check. Runs no command and touches no file."""
    problems: list[str] = []
    evaluator = manifest.get("evaluator")
    prepare = manifest.get("evaluator_prepare")
    binary = manifest.get("evaluator_binary")

    if prepare is not None:
        if (
            not isinstance(prepare, list)
            or not prepare
            or not all(
                isinstance(arg, str) and arg and "\x00" not in arg for arg in prepare
            )
        ):
            problems.append("evaluator_prepare must be a nonempty argv of strings")
        elif prepare[0] not in ALLOWED_PREPARE:
            problems.append(
                f"evaluator_prepare command {prepare[0]!r} is not allowlisted "
                f"({sorted(ALLOWED_PREPARE)})"
            )
        if not binary:
            problems.append(
                "evaluator_prepare requires evaluator_binary: a build step whose "
                "output is not declared cannot be checked for having produced it"
            )

    if isinstance(evaluator, list) and evaluator:
        if evaluator[0] not in SELF_BUILDING and prepare is None:
            problems.append(
                f"evaluator {evaluator[0]!r} does not build its own binary, so the "
                "manifest must declare evaluator_prepare; without it a fresh "
                "checkout runs whatever stale or missing output it finds"
            )
    return problems


def _mtime_ns(path: Path) -> int:
    return path.stat().st_mtime_ns


def freshness_problems(root: Path, manifest: dict[str, Any]) -> list[str]:
    """Does the declared binary exist, and is it at least as new as its sources?

    Applied only where `evaluator_prepare` is declared. A `make` evaluator's
    binary is produced by the evaluator run itself, and make -- not this file --
    is the authority on whether it is up to date with respect to the
    prerequisites its own rule declares.
    """
    problems: list[str] = []
    binary = manifest.get("evaluator_binary")
    if not binary:
        return ["evaluator_binary is not declared, so readiness cannot be checked"]
    binary_path = repo_path(root, binary)
    if not binary_path.is_file():
        return [f"declared evaluator binary is absent after prepare: {binary}"]
    built_at = _mtime_ns(binary_path)
    for value in sorted(manifest.get("evaluator_sources", [])):
        source_path = repo_path(root, value)
        if not source_path.is_file():
            problems.append(f"declared evaluator source is missing: {value}")
            continue
        if _mtime_ns(source_path) > built_at:
            problems.append(
                f"declared evaluator binary {binary} is older than its declared "
                f"source {value}: the run would use stale output"
            )
    return problems


def run_prepare(
    root: Path, manifest: dict[str, Any]
) -> tuple[int | None, str]:
    """Run the declared prepare argv. Returns (returncode, combined output)."""
    prepare = manifest.get("evaluator_prepare")
    if prepare is None:
        return None, ""
    environment = os.environ.copy()
    # A nested make must not inherit this run's jobserver or goal list.
    environment.pop("MAKEFLAGS", None)
    environment.pop("MFLAGS", None)
    completed = subprocess.run(
        list(prepare),
        cwd=root,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=PREPARE_TIMEOUT_SECONDS,
        check=False,
    )
    return completed.returncode, completed.stdout


def ensure_ready(root: Path, manifest: dict[str, Any]) -> list[str]:
    """Prepare the evaluator if it declares how, then prove it is usable.

    Returns the list of problems; empty means ready. Never raises for an
    ordinary build failure -- a failure has to be reportable by the caller that
    is about to refuse, not an exception that reads as a crash.
    """
    problems = declaration_problems(manifest)
    if problems:
        return problems
    if manifest.get("evaluator_prepare") is None:
        return []
    try:
        code, output = run_prepare(root, manifest)
    except subprocess.TimeoutExpired:
        return [
            "evaluator_prepare timed out after "
            f"{PREPARE_TIMEOUT_SECONDS}s: "
            + " ".join(manifest["evaluator_prepare"])
        ]
    except OSError as exc:
        return [f"evaluator_prepare could not be executed: {exc}"]
    if code != 0:
        tail = "\n".join(output.strip().splitlines()[-12:])
        return [
            "evaluator_prepare failed (rc="
            f"{code}): {' '.join(manifest['evaluator_prepare'])}\n{tail}"
        ]
    return freshness_problems(root, manifest)


def main(argv: list[str]) -> int:
    wanted = set(argv[1:])
    manifests = sorted(MANIFEST_DIR.glob("*.json"))
    if not manifests:
        print("CAPABILITY_EVALUATOR_PREREQ_FAIL reason=no_manifests")
        return 1
    failures = 0
    checked = 0
    prepared = 0
    for manifest_path in manifests:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        capability_id = manifest.get("capability_id", manifest_path.stem)
        if wanted and capability_id not in wanted:
            continue
        checked += 1
        declares = manifest.get("evaluator_prepare") is not None
        problems = ensure_ready(ROOT, manifest)
        for problem in problems:
            print(f"CAPABILITY_PREREQ id={capability_id} problem={problem}")
        if problems:
            failures += 1
            continue
        if declares:
            prepared += 1
        print(
            f"CAPABILITY_PREREQ id={capability_id} "
            f"prepare={'declared' if declares else 'self_building'} "
            f"binary={'fresh' if declares else 'built_by_evaluator'}"
        )
    unknown = sorted(
        wanted
        - {
            json.loads(path.read_text(encoding="utf-8")).get(
                "capability_id", path.stem
            )
            for path in manifests
        }
    )
    for capability_id in unknown:
        print(f"CAPABILITY_PREREQ id={capability_id} problem=no such manifest")
        failures += 1
    if not checked:
        print("CAPABILITY_EVALUATOR_PREREQ_FAIL reason=nothing_checked")
        return 1
    if failures:
        print(f"CAPABILITY_EVALUATOR_PREREQ_FAIL failures={failures}")
        return 1
    print(
        "CAPABILITY_EVALUATOR_PREREQ_PASS "
        f"capabilities={checked} prepared={prepared} "
        f"scope={'selected' if wanted else 'all'}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
