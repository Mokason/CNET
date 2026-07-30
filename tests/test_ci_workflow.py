#!/usr/bin/env python3
"""Validate the local make ci contract.

GitHub Actions CI was removed after a one-shot green proof to avoid
per-commit Actions spend. This gate still enforces that the *local*
portable CI graph remains complete (make ci deps, ROCm lane contract).
"""
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parents[1]
makefile = root / "Makefile"
workflow = root / ".github" / "workflows" / "ci.yml"

if not makefile.is_file():
    raise SystemExit("CI_WORKFLOW_FAIL: Makefile missing")

make_text = makefile.read_text(encoding="utf-8")
ci_rule = re.search(r"^ci:\s*(.+)$", make_text, re.MULTILINE)
if not ci_rule:
    raise SystemExit("CI_WORKFLOW_FAIL: make ci target missing")
ci_dependencies = set(ci_rule.group(1).split())
required_gates = {"test", "dotnet_cce_tests", "cce_train_bench"}
missing = sorted(required_gates - ci_dependencies)
if missing:
    raise SystemExit(
        "CI_WORKFLOW_FAIL: make ci missing full-suite gates: " + ", ".join(missing)
    )

# Cross-platform / ROCm lane still required as a *local* authority.
rocm_rule = re.search(r"^ci_rocm:\s*(.+)$", make_text, re.MULTILINE)
if not rocm_rule:
    raise SystemExit("CI_WORKFLOW_FAIL: make ci_rocm target missing (no GPU lane)")
if "ci" not in set(rocm_rule.group(1).split()):
    raise SystemExit("CI_WORKFLOW_FAIL: ci_rocm must also run the portable ci gate")
rocm_recipe = make_text[rocm_rule.end() :].split("\n\n", 1)[0]
if "hipgemm_res" not in rocm_recipe:
    raise SystemExit("CI_WORKFLOW_FAIL: ci_rocm must run the hipgemm_res device gate")
if "CNET_REQUIRE_ROCM=1" not in rocm_recipe:
    raise SystemExit(
        "CI_WORKFLOW_FAIL: ci_rocm must set CNET_REQUIRE_ROCM=1 so an absent "
        "device fails instead of silently skipping"
    )
if not re.search(r"^hipgemm_res:", make_text, re.MULTILINE):
    raise SystemExit("CI_WORKFLOW_FAIL: hipgemm_res target missing")

if workflow.is_file():
    # If someone reintroduces GHA, keep the old structural checks.
    try:
        import yaml
    except ImportError as exc:
        raise SystemExit(f"CI_WORKFLOW_FAIL: PyYAML required to audit GHA: {exc}") from exc
    try:
        doc = yaml.safe_load(workflow.read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001
        raise SystemExit(f"CI_WORKFLOW_FAIL: invalid YAML: {exc}") from exc
    if not isinstance(doc, dict):
        raise SystemExit("CI_WORKFLOW_FAIL: root must be a mapping")
    print("CI_WORKFLOW_LOCAL_PASS status=local_gates_ok hosted_workflow=PRESENT")
else:
    # A local structural check is not evidence that anything ran per-change on
    # a machine other than this one. Saying PASS here labelled an absence as a
    # result; the local gates are reported for what they are, and the hosted
    # workflow is explicitly WITHHELD. config/ci_contract.json records the same
    # status, and tests/test_ci_contract.py refuses to let it become a pass.
    print("CI_WORKFLOW_LOCAL_PASS status=local_gates_ok hosted_workflow=WITHHELD")
    print(
        "note: no executing hosted workflow. `make ci` / `make ci_rocm` are the "
        "local authority; per-change enforcement elsewhere is NOT established"
    )

sys.exit(0)
