#!/usr/bin/env python3
"""CI must enumerate what it proves, and never call an absence a pass.

`make ci_core` ran two scientific gates and nothing recorded what CI was
supposed to cover, so a gate could stop being enforced with no signal. Worse,
`tests/test_ci_workflow.py` printed `CI_WORKFLOW_PASS status=gha_disabled` when
there was no hosted workflow at all — a local structural check labelled as a
workflow pass.

This gate reads `config/ci_contract.json` and enforces the contract against the
Makefile:

  * every gate the contract marks `required` exists as a Makefile target AND is
    a prerequisite of `ci_core`;
  * every gate marked `blocked` or `withheld` carries a reason and is NOT a
    prerequisite of `ci_core`, so a gate that cannot run here cannot be reported
    as part of a passing CI;
  * `ci_core` declares no scientific gate the contract does not mention;
  * the hosted-workflow status is `withheld` or `blocked` while no workflow file
    exists, and may never be `pass`.

Usage: python3 tests/test_ci_contract.py
Exit 0 = the contract and the Makefile agree, 1 = they do not.
"""

from __future__ import annotations

import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / "config" / "ci_contract.json"
MAKEFILE = ROOT / "Makefile"
WORKFLOWS = ROOT / ".github" / "workflows"

# Prerequisites of ci_core that are build/hygiene plumbing rather than gates
# making a scientific claim. The contract enumerates claims; these are listed
# here so "ci_core declares nothing the contract omits" stays checkable.
INFRASTRUCTURE = {
    "ci_config_gate",
    "warning_debt_strict",
    "release_warning_gate",
    "flagship_prefix_cache",
    "campaign_provenance_unit",
    "execution_tiers_doc_gate",
    "alt_paths_gate",
    "artifact_isa_gate",
    "runtime_artifact_hygiene",
    "ci_contract_gate",
}

failures: list[str] = []


def check(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)
        print(f"FAIL: {message}")


def main() -> int:
    if not CONTRACT.is_file():
        print("FAIL: config/ci_contract.json is missing")
        return 1
    contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
    make_text = MAKEFILE.read_text(encoding="utf-8", errors="replace")

    rule = re.search(r"^ci_core:[ \t]*(.+)$", make_text, re.MULTILINE)
    if rule is None:
        print("FAIL: make ci_core target missing")
        return 1
    prerequisites = set(rule.group(1).split())

    targets = set(re.findall(r"^([A-Za-z0-9_./-]+):", make_text, re.MULTILINE))

    required: set[str] = set()
    for gate in contract["gates"]:
        name = gate["target"]
        status = gate["status"]
        check(name in targets, f"contract names {name}, which is not a Makefile target")
        check(
            isinstance(gate.get("proves"), str) and gate["proves"],
            f"{name}: every gate must say what it proves",
        )
        if status == "required":
            required.add(name)
            check(
                name in prerequisites,
                f"{name} is required by the contract but is not a ci_core prerequisite",
            )
        elif status in ("blocked", "withheld"):
            check(
                isinstance(gate.get("reason"), str) and gate["reason"],
                f"{name}: a {status} gate must record why",
            )
            check(
                name not in prerequisites,
                f"{name} is {status} and must NOT be a ci_core prerequisite -- a gate "
                "that cannot run here cannot be part of a passing CI",
            )
        else:
            check(False, f"{name}: unknown status {status!r}")

    undeclared = sorted(prerequisites - required - INFRASTRUCTURE)
    check(
        not undeclared,
        f"ci_core declares gates the contract does not mention: {undeclared}",
    )

    hosted = contract["hosted_workflow"]
    check(
        hosted["status"] in ("withheld", "blocked", "required"),
        f"hosted_workflow status {hosted['status']!r} is not a recognised status",
    )
    workflow_present = WORKFLOWS.is_dir() and any(WORKFLOWS.iterdir())
    if not workflow_present:
        check(
            hosted["status"] in ("withheld", "blocked"),
            "no hosted workflow exists, so its status must be withheld or blocked, "
            f"not {hosted['status']!r}",
        )
    check(
        hosted["status"] != "pass",
        "an absent hosted workflow is never a pass",
    )

    for claim in contract.get("withheld_claims", []):
        check(
            claim["status"] in ("withheld", "blocked", "failed"),
            f"claim {claim['claim']!r} has status {claim['status']!r}, which would "
            "read as established",
        )
        check(
            isinstance(claim.get("reason"), str) and claim["reason"],
            f"claim {claim['claim']!r} must record why it is not established",
        )

    if failures:
        print(f"CI_CONTRACT_FAIL failures={len(failures)}")
        return 1
    blocked = sum(1 for g in contract["gates"] if g["status"] == "blocked")
    print(
        f"CI_CONTRACT_PASS required={len(required)} blocked={blocked} "
        f"withheld_claims={len(contract.get('withheld_claims', []))} "
        f"hosted_workflow={hosted['status']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
