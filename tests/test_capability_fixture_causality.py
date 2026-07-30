#!/usr/bin/env python3
"""Held-out capability fixtures must be CAUSAL, not decorative.

The re-analysis (2026-07-30) established that `CNET_HELD_OUT_FIXTURE` was
exported by the runner and read by nobody: every evaluator ran its own
hard-coded cases, so a declared held-out case could be edited freely while the
certificate stayed green. The fixture SHA-256 in the report then proved only
which file existed, never which cases were evaluated.

This gate proves the opposite property directly, per capability:

  CONTROL   the evaluator run against the pristine fixture exits 0 AND emits a
            consumption receipt naming the capability, the exact fixture
            SHA-256, every declared case id, and a consumed count equal to the
            declared case count.
  MUTATION  one semantic expected value is changed while every string in
            `expected_markers` is left byte-identical. The evaluator must exit
            non-zero.

An evaluator that does not run at all cannot emit the receipt, so this also
catches the silent-success mode (`dotnet test --no-restore` against an
unrestored project exits 0 having run zero tests).

That mode was not hypothetical, and catching it is not the same as surviving
it: at 6f9c859 a fresh detached worktree failed this gate with five failures
for `honest_memory_retrieval` -- no receipt, no case consumed, three mutations
returning rc=0 -- because nothing in the tree ever built the evaluator it then
asked to run. The gate was right and the tree was unbuildable. So every
capability is now made ready first, by its own declared prepare step, and a
capability that cannot be made ready is refused BY NAME before its evaluator
runs (see `scripts/capability_evaluator_prereq.py`).

Usage: python3 tests/test_capability_fixture_causality.py [capability_id ...]
Exit 0 = every capability is causal, 1 = at least one is decorative.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
MANIFEST_DIR = ROOT / "config" / "capability_manifests"

sys.path.insert(0, str(ROOT / "scripts"))
from capability_evaluator_prereq import ensure_ready  # noqa: E402

# Semantic mutations per capability: (case index, key, replacement value).
# Every value here is an EXPECTATION, a FLOOR or a declared SHAPE the evaluator
# must honour, and none appears in the fixture's `expected_markers`, so a
# marker-scanning runner cannot notice the change.
#
# One mutation per capability proves the fixture is consumed. It does NOT prove
# every declared field is causal -- a field nobody reads can sit in the fixture
# looking like a commitment. So each field a capability retains gets its own
# entry, mutated one at a time.
MUTATIONS: dict[str, list[tuple[int, str, Any]]] = {
    "calibrated_abstention": [
        (0, "expected", "answer_with_evidence"),
        (0, "margin", 0.9),
        (1, "expected", "abstain"),
        (1, "reliability", 0.10),
        (1, "samples", 1),
        (2, "expected", "claim_bound"),
        (2, "evidence_refs", ["evidence://memory/fact-17"]),
    ],
    "cce_classification": [
        (0, "minimum_accuracy", 0.999),
        (0, "classes", 3),
        (0, "held_out_samples", 999),
        (0, "diff_mode", "APPROX"),
        (0, "gradient_clip", 1.0),
        (0, "label_rule", "argmin_pair_sum_first_eight_dimensions"),
        (0, "minimum_distinct_classes", 5),
        (0, "minimum_lift_over_majority", 0.99),
    ],
    "honest_memory_retrieval": [
        (0, "expected", "hit"),
        (0, "stored", "the launch code is amber-lark-3"),
        (0, "query", "the sky is blue"),
    ],
    "hybrid_skill_serve": [
        (0, "expected_authority", "certified"),
        (0, "backend", "residual_compatible"),
        (0, "query", "totally different words entirely"),
        (0, "expected_proposals", "semantic-candidate:nope"),
        (1, "expected_authority", "provisional"),
        (1, "backend", "hermetic"),
    ],
    # Every field this capability retains, one at a time. The two baselines are
    # the reason this list exists: before R9 they were only required to satisfy
    # `on > off`, so both could be edited freely.
    "json_toolcall_adapter": [
        (0, "minimum_accuracy_with_adapter", 0.999),
        (0, "adapter_on_baseline", 0.30),
        (0, "adapter_off_baseline", 0.70),
        (0, "baseline_tolerance", -1.0),
        (0, "metric", "acc_off"),
        (0, "skill", "json_toolcall_v1"),
        (0, "held_out_pairs_from", "somewhere else entirely"),
        (0, "certify_before_serve", False),
    ],
    "sleep_consolidation": [
        (0, "expected_semantic_promotions", 4),
        (0, "expected_procedural_promotions", 3),
        (0, "episodes", 6),
        (0, "duplicate_episodes", 4),
        (0, "minimum_pruned", 9),
        (0, "minimum_merges", 9),
        (0, "minimum_graduated", 9),
    ],
}

RECEIPT = re.compile(
    r"^HELDOUT_FIXTURE capability=(\S+) sha256=([0-9a-f]{64}) "
    r"cases=(\d+) consumed=(\d+)$",
    re.MULTILINE,
)
CASE_LINE = re.compile(r"^HELDOUT_CASE id=(\S+) reads=(\d+)$", re.MULTILINE)

failures: list[str] = []


def check(condition: bool, message: str) -> bool:
    if not condition:
        failures.append(message)
        print(f"FAIL: {message}")
    return bool(condition)


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run_evaluator(argv: list[str], fixture: Path, timeout: int):
    environment = os.environ.copy()
    environment["CNET_HELD_OUT_FIXTURE"] = str(fixture)
    # Nested make must not inherit the parent's jobserver or targets.
    environment.pop("MAKEFLAGS", None)
    environment.pop("MFLAGS", None)
    return subprocess.run(
        argv,
        cwd=ROOT,
        env=environment,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=False,
    )


def mutate(
    fixture: dict[str, Any], capability_id: str, mutation: tuple[int, str, Any]
) -> dict[str, Any]:
    index, key, value = mutation
    mutated = json.loads(json.dumps(fixture))
    case = mutated["cases"][index]
    if key not in case:
        raise KeyError(f"{capability_id}: case {index} has no key {key!r}")
    if case[key] == value:
        raise ValueError(f"{capability_id}: mutation is a no-op for {key!r}")
    case[key] = value
    return mutated


def declared_field_keys(fixture: dict[str, Any]) -> set[tuple[int, str]]:
    """Every (case index, field) a fixture declares, except the case id."""
    return {
        (index, key)
        for index, case in enumerate(fixture["cases"])
        for key in case
        if key != "id"
    }


def check_capability(manifest_path: Path) -> None:
    manifest = json.loads(manifest_path.read_text())
    capability_id = manifest["capability_id"]
    fixture_path = ROOT / manifest["held_out_fixture"]
    fixture = json.loads(fixture_path.read_text())
    argv = manifest["evaluator"]
    timeout = int(manifest.get("timeout_seconds", 300))
    cases = fixture["cases"]

    if not check(
        all(isinstance(case.get("id"), str) and case["id"] for case in cases),
        f"{capability_id}: every declared case carries a stable string id",
    ):
        return
    ids = [case["id"] for case in cases]
    if not check(
        len(set(ids)) == len(ids), f"{capability_id}: case ids are unique"
    ):
        return

    # Build first, or refuse first. Running an evaluator whose output is
    # missing or stale is how a fresh checkout got five failures out of a
    # correct gate: the evaluator exited 0 having done nothing at all.
    ready_problems = ensure_ready(ROOT, manifest)
    for problem in ready_problems:
        check(False, f"{capability_id}: evaluator is not ready: {problem}")
    if ready_problems:
        return
    print(f"--- {capability_id}: evaluator prepared")

    print(f"--- {capability_id}: control run (pristine fixture)")
    control = run_evaluator(argv, fixture_path, timeout)
    check(
        control.returncode == 0,
        f"{capability_id}: pristine fixture certifies (rc={control.returncode})",
    )
    receipt = RECEIPT.search(control.stdout)
    if check(
        receipt is not None,
        f"{capability_id}: evaluator emits a HELDOUT_FIXTURE receipt",
    ):
        assert receipt is not None
        want_sha = sha256_file(fixture_path)
        check(
            receipt.group(1) == capability_id,
            f"{capability_id}: receipt names the capability "
            f"(saw {receipt.group(1)})",
        )
        check(
            receipt.group(2) == want_sha,
            f"{capability_id}: receipt binds the exact fixture sha256 "
            f"(saw {receipt.group(2)}, want {want_sha})",
        )
        check(
            receipt.group(3) == str(len(cases)),
            f"{capability_id}: receipt declares {len(cases)} cases "
            f"(saw {receipt.group(3)})",
        )
        check(
            receipt.group(4) == str(len(cases)),
            f"{capability_id}: every declared case was consumed "
            f"(saw {receipt.group(4)}/{len(cases)})",
        )
    seen = {match.group(1) for match in CASE_LINE.finditer(control.stdout)}
    for case_id in ids:
        check(
            case_id in seen,
            f"{capability_id}: case {case_id} reported as consumed",
        )

    # Coverage, in two parts, because a fixture can carry a field nobody reads
    # that looks like a commitment:
    #   * every declared field NAME must have a mutation somewhere, and
    #   * every declared CASE must have at least one.
    # Name-level rather than (case, field)-level on purpose: a field can be an
    # input that only changes the verdict in one of several cases -- e.g.
    # `reliability` cannot flip a case whose margin already forces abstention --
    # and demanding a verdict-flipping mutation in every slot would mean
    # deleting real inputs to satisfy the gate.
    mutations = MUTATIONS[capability_id]
    declared = declared_field_keys(fixture)
    covered_names = {key for _, key, _ in mutations}
    covered_cases = {index for index, _, _ in mutations}
    uncovered_names = sorted({key for _, key in declared} - covered_names)
    uncovered_cases = sorted({index for index, _ in declared} - covered_cases)
    check(
        not uncovered_names,
        f"{capability_id}: every declared field name has a mutation "
        f"(uncovered: {uncovered_names})",
    )
    check(
        not uncovered_cases,
        f"{capability_id}: every declared case has a mutation "
        f"(uncovered: {uncovered_cases})",
    )

    for index, key, value in mutations:
        print(f"--- {capability_id}: mutation run (case[{index}].{key})")
        mutated = mutate(fixture, capability_id, (index, key, value))
        markers_before = json.dumps(fixture.get("expected_markers"), sort_keys=True)
        markers_after = json.dumps(mutated.get("expected_markers"), sort_keys=True)
        check(
            markers_before == markers_after,
            f"{capability_id}: mutating {key} leaves expected_markers untouched",
        )
        with tempfile.TemporaryDirectory(prefix="cnet-causality-") as directory:
            path = Path(directory) / fixture_path.name
            path.write_text(json.dumps(mutated, indent=2) + "\n", encoding="utf-8")
            result = run_evaluator(argv, path, timeout)
        check(
            result.returncode != 0,
            f"{capability_id}: mutating case[{index}].{key} -> {value!r} must "
            f"fail the evaluator (rc={result.returncode})",
        )


def main(argv: list[str]) -> int:
    wanted = set(argv[1:])
    manifests = sorted(MANIFEST_DIR.glob("*.json"))
    if not manifests:
        print("FAIL: no capability manifests")
        return 1
    checked = 0
    for manifest_path in manifests:
        capability_id = json.loads(manifest_path.read_text())["capability_id"]
        if wanted and capability_id not in wanted:
            continue
        checked += 1
        if capability_id not in MUTATIONS:
            failures.append(f"{capability_id}: no declared semantic mutation")
            print(f"FAIL: {capability_id}: no declared semantic mutation")
            continue
        check_capability(manifest_path)
    unknown = wanted - {
        json.loads(path.read_text())["capability_id"] for path in manifests
    }
    for capability_id in sorted(unknown):
        failures.append(f"{capability_id}: no such capability manifest")
        print(f"FAIL: {capability_id}: no such capability manifest")
    if not checked:
        print("FAIL: no capability was checked")
        return 1
    if failures:
        print(f"CAPABILITY_FIXTURE_CAUSALITY_FAIL failures={len(failures)}")
        return 1
    # Report what this invocation actually checked, never the manifest count:
    # `... capabilities=6` after checking one is exactly the vanity metric this
    # gate exists to eliminate.
    total_mutations = sum(
        len(MUTATIONS[json.loads(path.read_text())["capability_id"]])
        for path in manifests
        if json.loads(path.read_text())["capability_id"] in MUTATIONS
        and (not wanted or json.loads(path.read_text())["capability_id"] in wanted)
    )
    print(
        "CAPABILITY_FIXTURE_CAUSALITY_PASS "
        f"capabilities={checked} mutations_rejected={total_mutations} "
        f"scope={'selected' if wanted else 'all'}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
