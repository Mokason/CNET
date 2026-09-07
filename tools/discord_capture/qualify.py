"""Rechecked development-only readiness and prospective control bounds."""
import argparse
from collections import Counter
import json
from pathlib import Path
import sys

from dataset import build_episodes, check_deadline, now_boot, validate_export
from evidence import CATALOG, NativeMapper, digest

# Reuse the existing equal-budget planner; deployment copies these exact sources.
controller = Path(__file__).resolve().parents[2] / "experiments" / "offline_controller"
if controller.is_dir():
    sys.path.insert(0, str(controller))
from allocator_sequence_pilot import POLICIES, plan


def compare_direct(labels):
    if not 1 <= len(labels) <= 2048:
        raise ValueError("qualification_episode_size")
    # Fixed before observing demand: 16 disjoint 32-row direct acquisitions.
    tasks = [dict(cost=32, age=0) for _ in range(16)]
    requirements = []
    for label in labels:
        if label.get("status") != "verified_tool":
            raise ValueError("qualification_unmapped")
        rule = next((i for i, r in enumerate(CATALOG)
                     if (r["input_tag"], r["output_tag"]) ==
                     (label["input_tag"], label["output_tag"])), None)
        if rule is None or type(label["key"]) is not int or not 0 <= label["key"] <= 255:
            raise ValueError("qualification_catalog")
        requirements.append((1 << (rule * 8 + label["key"] // 32),))
    controls = [plan(tasks, requirements, policy, jobs=8, work=256) for policy in POLICIES]
    strongest = max(row["value"] for row in controls)
    exact = next(row for row in controls if row["policy"] == "exact")
    return dict(scope="prospective_direct_catalog_only", native_outcomes_measured=False,
                jobs=8, row_work=256, controls=controls,
                headroom_upper_vs_strongest=(exact["upper_bound"] - strongest) / len(labels),
                bound_complete=exact["complete"])


def qualify(root, mapper):
    deadline = now_boot() + 60
    root = Path(root)
    manifest = validate_export(root)
    if manifest["catalog_sha256"] != mapper.catalog_sha256 or manifest["artifacts"] != mapper.pins:
        raise ValueError("qualification_artifacts")
    rows = [json.loads(line) for line in (root / "requests.jsonl").read_bytes().splitlines()]
    stored = [json.loads(line) for line in (root / "evidence.jsonl").read_bytes().splitlines()]
    if len(rows) > 100000 or len(stored) != len(rows):
        raise ValueError("qualification_rows")
    labels = []
    for row, old in zip(rows, stored, strict=True):
        check_deadline(deadline)
        fresh = mapper.label(row["delivered_text"], digest(row))
        if fresh != old:
            raise ValueError("qualification_receipt")
        labels.append(fresh)
    events = json.loads((root / "events.json").read_bytes())
    published = json.loads((root / "episodes.json").read_bytes())
    reviews = {ep["episode_sha256"]: ep["origin"] for ep in published}
    if any(value not in {"human", "test", "automation", "unreviewed"} for value in reviews.values()):
        raise ValueError("qualification_origin")
    episodes = build_episodes(rows, labels, events, manifest["observed_ns"], reviews, deadline)
    if episodes != published:
        raise ValueError("qualification_episodes")
    eligible = [ep for ep in episodes if not ep["excluded"]]
    by_ordinal = {row["ord"]: label for row, label in zip(rows, labels, strict=True)}
    if len(by_ordinal) != len(rows):
        raise ValueError("qualification_duplicate")
    comparisons = []
    for episode in eligible:
        check_deadline(deadline)
        comparison = compare_direct([by_ordinal[ordinal] for ordinal in episode["record_ordinals"]])
        comparisons.append(dict(window=episode["window"], **comparison))
    check_deadline(deadline)
    reasons = ["native_outcomes_not_measured", "four_family_evidence_missing", "fresh_confirmation_required"]
    if not eligible:
        reasons.insert(0, "no_eligible_episodes")
    elif all(row["headroom_upper_vs_strongest"] < 0.05 for row in comparisons):
        reasons.insert(0, "insufficient_prospective_headroom")
    else:
        reasons.insert(0, "native_development_trajectory_required")
    # Whole ordered windows only. Both partitions remain EXPOSED development.
    cutoff = len(eligible) * 2 // 3
    splits = dict(development_fit=[ep["window"] for ep in eligible[:cutoff]],
                  development_validation=[ep["window"] for ep in eligible[cutoff:]], confirmation=[])
    return dict(schema=1, purpose="development_only", dataset_sha256=digest(manifest),
                requests=len(rows), verified_labels=sum(r["status"] == "verified_tool" for r in labels),
                episodes=len(episodes), eligible_episodes=len(eligible),
                excluded_requests=sum(ep["requests"] for ep in episodes if ep["excluded"]),
                exclusions=dict(Counter(reason for ep in episodes for reason in ep["excluded"])),
                splits=splits, comparisons=comparisons, withheld_reasons=reasons,
                amd_fitting_eligible=False, confirmation_eligible=False, product_acceptance=False,
                gate=dict(absolute_gain=0.05, paired95_lower="positive", family_means="nonnegative"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dataset", type=Path)
    parser.add_argument("runtime", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(qualify(args.dataset, NativeMapper(args.runtime)), sort_keys=True))
    except Exception:
        print('{"error":"qualification_refused","amd_fitting_eligible":false}')
        raise SystemExit(1)
