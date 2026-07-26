#!/usr/bin/env python3
import pathlib
import re
import sys
import yaml

path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else ".github/workflows/ci.yml")
if not path.is_file():
    raise SystemExit("CI_WORKFLOW_FAIL: workflow missing")
try:
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
except yaml.YAMLError as exc:
    raise SystemExit(f"CI_WORKFLOW_FAIL: invalid YAML: {exc}") from exc
if not isinstance(doc, dict):
    raise SystemExit("CI_WORKFLOW_FAIL: root must be a mapping")
triggers = doc.get("on")
if not isinstance(triggers, dict) or "workflow_dispatch" not in triggers:
    raise SystemExit("CI_WORKFLOW_FAIL: manual workflow_dispatch trigger required")
for automatic in ("push", "pull_request"):
    if automatic not in triggers:
        raise SystemExit(f"CI_WORKFLOW_FAIL: automatic {automatic} trigger required")
if doc.get("permissions") != {"contents": "read"}:
    raise SystemExit("CI_WORKFLOW_FAIL: permissions must be contents: read")
jobs = doc.get("jobs")
if not isinstance(jobs, dict) or not jobs:
    raise SystemExit("CI_WORKFLOW_FAIL: at least one job required")
runs = []
for job in jobs.values():
    if not isinstance(job, dict) or "timeout-minutes" not in job:
        raise SystemExit("CI_WORKFLOW_FAIL: every job needs a timeout")
    for step in job.get("steps", []):
        if isinstance(step, dict) and isinstance(step.get("run"), str):
            runs.append(step["run"])
joined = "\n".join(runs)
if "make" not in joined or "PORTABLE=1" not in joined or " ci" not in joined:
    raise SystemExit("CI_WORKFLOW_FAIL: portable make ci command missing")
if "cuda" in joined.lower() or "nvidia" in joined.lower():
    raise SystemExit("CI_WORKFLOW_FAIL: CPU CI must not install CUDA/NVIDIA")
makefile = path.parents[2] / "Makefile"
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
print("CI_WORKFLOW_PASS")
