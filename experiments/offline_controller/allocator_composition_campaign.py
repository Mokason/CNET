#!/usr/bin/env python3
"""Independent certified-composition acquisition labels, not production admission.

Confirmation outcomes are retained privately and only their integrity digest is
printed. No candidate is fitted or scored here. Whole graph instances, rather
than renamed rows, are the split unit; motifs and the finite XOR oracle repeat.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import random
import shutil
import tempfile

from allocator_campaign import Runtime, Reply, bounded, canonical, freeze_binary, save, sha

PROTOCOL = {
    "kind": "certified_composition_acquisition_v1", "feature_version": 1,
    "development_episodes": 32, "confirmation_episodes": 32,
    "development_seed": 690606311, "confirmation_seed": 690606313,
    "family_names": ["direct", "shared-prefix", "chain-completion", "evidence-rejection"],
    "tasks": 4, "jobs": 1, "work": 32, "rows_per_primitive": 32,
    "bits": 6, "domain": 64, "population": 32, "wait_limit": 4,
    "ports": "p plus 24 hex digest characters of endpoint identity; native near-miss guard unchanged",
    "oracle_operands": [1, 3, 7, 15, 31, 63],
    "feature_f0": "observed unmet requests touching this task / all observed unmet requests",
    "feature_f1": "certified unique blocks / all unique blocks of associated plans",
    "feature_f2": "32 / 2048", "age": 0,
    "primitive_coverage": "32 uniformly sampled keys from the 64-key domain",
    "missing_distractor_probability": 0.25,
    "fairness": "one scheduling decision, all ages zero; same prospective row budget",
    "independence": "task subgraphs have disjoint ports; jobs=1, no union-synergy claim",
    "freshness": "fresh root/goal/input requests exclude all observed requests",
    "evidence_rejection": "one task has a deliberately wrong source row; independent oracle check refuses before teach",
    "bounds": {"cpu_seconds": 30, "memory_mib": 512, "wall_boottime_seconds": 30, "pipe_bytes": 65536},
    "production_admission": False,
    "scope": "distinct random finite graph instances within four recurring motifs, not broad semantic competence",
    "gate": {"gain_each_control": 0.05, "paired95_lower": ">0", "family_nonregression": True},
}


def call_checked(argv, destination, env):
    call = bounded(argv, env, 30, experimental_limits=True)
    save(destination, canonical(call))
    if call["returncode"] or call["refused"]:
        raise RuntimeError(f"bounded_native_call_refused: {destination}")
    return call


def artifacts(path):
    files = {item.name: sha(item.read_bytes()) for item in sorted(path.iterdir())}
    if set(files) != {"unit.cnb", "manifest.cknow"}:
        raise RuntimeError("unexpected_generic_capsule_artifacts")
    return files


def ask(runtime, handle, request, expected):
    reply = Reply()
    rc = runtime.lib.cnet_capsule_core_ask(handle, request.encode(), ctypes.byref(reply))
    if reply.verified:
        if rc or reply.value != expected:
            raise RuntimeError("incorrect_verified_composition")
    elif not rc:
        raise RuntimeError("inconsistent_native_refusal")
    return {"request": request, "expected": expected, "rc": rc,
            "verified": reply.verified, "value": reply.value, "hops": reply.hops,
            "units": reply.units.decode(), "reason": reply.reason.decode()}


def open_registry(runtime, registry):
    error = ctypes.create_string_buffer(160)
    handle = runtime.lib.cnet_capsule_core_open(os.fsencode(registry), error, len(error))
    if not handle:
        raise RuntimeError("incumbent_import: " + error.value.decode())
    return handle


def oracle_pool(root, binary, env):
    pool = root / "oracle"
    pool.mkdir(mode=0o700)
    values, receipts = {}, {}
    for operand in PROTOCOL["oracle_operands"]:
        values[operand], receipts[operand] = {}, {}
        for key in range(64):
            path = pool / f"xor-{operand}-{key}.json"
            call = call_checked([str(binary / "cnet_capsule_tool"), "xor", str(operand), str(key)], path, env)
            expected = key ^ operand
            if call["stdout"] != str(expected) + "\n":
                raise RuntimeError("independent_oracle_mismatch")
            values[operand][key] = int(call["stdout"])
            receipts[operand][key] = sha(canonical(call))
    return values, receipts


def make_group(rng, episode, task, family, oracle):
    """Construct plans and prior certification state BEFORE native outcomes."""
    blocks = []
    prefix = f"e{episode}t{task}"

    def block(source, target):
        index = len(blocks)
        operand = rng.choice(PROTOCOL["oracle_operands"])
        keys = sorted(rng.sample(range(64), 32))
        blocks.append({"unit": f"mined_{prefix}b{index}",
                       "input": "p" + sha(source.encode())[:24], "output": "p" + sha(target.encode())[:24],
                       "operand": operand, "keys": keys,
                       "rows": [[key, oracle[operand][key]] for key in keys]})
        return index

    origin = prefix + "src"
    if family == 1:
        middle = prefix + "mid"
        frontier = block(origin, middle)
        plans = []
        for branch in range(rng.randint(2, 3)):
            path, source = [frontier], middle
            for hop in range(rng.randint(1, 3)):
                target = f"{prefix}r{branch}h{hop}"
                path.append(block(source, target))
                source = target
            plans.append(path)
    else:
        length = 1 if family == 0 else rng.randint(2, 5)
        path, source = [], origin
        for hop in range(length):
            target = f"{prefix}h{hop}"
            path.append(block(source, target))
            source = target
        plans, frontier = [path], path[-1]
    installed = [i for i in range(len(blocks)) if i != frontier]
    # Some queued acquisitions advance an unfinished plan; one guaranteed
    # completion opportunity remains in every non-direct family episode.
    if family in (1, 2) and task != 0 and len(installed) and rng.random() < 0.25:
        installed.remove(rng.choice(installed))
    return {"task": task + 1, "blocks": blocks, "plans": plans, "frontier": frontier,
            "installed": installed, "bad_evidence": family == 3 and task == 0}


def validate_tags(groups):
    tags = sorted({block[side] for group in groups for block in group["blocks"] for side in ("input", "output")})
    for index, tag in enumerate(tags):
        if len(tag) != 25 or not tag.startswith("p"):
            raise RuntimeError("experimental_port_identity")
        for prior in tags[:index]:
            if sum(a != b for a, b in zip(tag, prior)) <= 2:
                raise RuntimeError("experimental_ports_too_similar")


def request(group, plan, key, oracle):
    blocks = group["blocks"]
    value = key
    for index in plan:
        item = blocks[index]
        value = oracle[item["operand"]][value]
    return {"request": f"capsule {blocks[plan[0]]['input']} {blocks[plan[-1]]['output']} {key}",
            "expected": value}


def write_source(path, block, corrupt=False):
    rows = [list(row) for row in block["rows"]]
    if corrupt:
        rows[0][1] ^= 1
    raw = "".join(f"{key}\t{value}\n" for key, value in rows).encode()
    save(path, raw)
    return rows, sha(raw)


def teach(binary, env, registry, block, source, receipt):
    call = call_checked([str(binary / "cnet_capsule_core"), "teach", str(registry), block["unit"],
                         block["input"], block["output"], "6", "6", "verified_tool", str(source)], receipt, env)
    if "method=finite_domain_compile" not in call["stdout"] or "CAPSULE_TEACH_PASS" not in call["stdout"]:
        raise RuntimeError("bounded_finite_compiler_not_used")
    return artifacts(registry / block["unit"]), sha(canonical(call))


def trial(runtime, registry, candidate, probes, exhaustive):
    before = open_registry(runtime, registry)
    after = None
    try:
        obligations = ctypes.c_size_t()
        if candidate:
            error = ctypes.create_string_buffer(160)
            after = runtime.lib.cnet_capsule_core_open_candidate(os.fsencode(registry), os.fsencode(candidate), error, len(error))
            if not after:
                raise RuntimeError("canonical_candidate_import: " + error.value.decode())
            if runtime.lib.cnet_capsule_core_validate_growth(before, after, ctypes.byref(obligations)):
                raise RuntimeError("canonical_growth_refusal")
        else:
            after = open_registry(runtime, registry)
        all_keys = [ask(runtime, after, row["request"], row["expected"]) for row in exhaustive]
        comparisons, mask = [], 0
        for index, row in enumerate(probes):
            old = ask(runtime, before, row["request"], row["expected"])
            new = ask(runtime, after, row["request"], row["expected"])
            if old["verified"]:
                raise RuntimeError("probe_was_not_uncovered")
            if new["verified"]:
                mask |= 1 << index
            comparisons.append({"before": old, "after": new})
        return {"growth_obligations": obligations.value, "full_root_domain": all_keys,
                "probes": comparisons, "derived_mask": f"{mask:x}"}
    finally:
        if after:
            runtime.lib.cnet_capsule_core_close(after)
        runtime.lib.cnet_capsule_core_close(before)


def run(args):
    root = Path(tempfile.mkdtemp(prefix="cnet-allocator-composition-"))
    print(json.dumps({"event": "composition_started", "root": str(root)}), flush=True)
    binary = root / "bin"
    binary.mkdir(mode=0o700)
    frozen = {
        "compiler": freeze_binary(args.compiler, binary / "cnet_capsule_core", True),
        "oracle": freeze_binary(args.oracle, binary / "cnet_capsule_tool", True),
        "runtime": freeze_binary(args.library, binary / "libcnet_capsule_core.so", False),
        "producer": freeze_binary(Path(__file__), root / "allocator_composition_campaign.py", False),
        "support": freeze_binary(Path(__file__).with_name("allocator_campaign.py"), root / "allocator_campaign.py", False),
    }
    protocol = {**PROTOCOL, "artifacts": frozen}
    save(root / "protocol.json", canonical(protocol))
    save(root / "freeze.json", canonical({"protocol_sha256": sha(canonical(protocol)), "before_any_outcome": True,
                                         "checkpoint": "not yet fitted; confirmation outputs stay unreported"}))
    runtime = Runtime(binary / "libcnet_capsule_core.so")
    env = {"PATH": "/usr/bin:/bin", "LANG": "C"}
    oracle, oracle_receipts = oracle_pool(root, binary, env)
    totals = {"episodes": 0, "initial_capsules": 0, "successful_acquisitions": 0,
              "evidence_rejections": 0, "full_domain_checks": 0, "wrong_verified": 0}
    digests = {}
    for split in ("development", "confirmation"):
        rng = random.Random(PROTOCOL[split + "_seed"])
        output_rows = []
        for number in range(32):
            eid = (1 if split == "development" else 1001) + number
            family = number % 4
            episode = root / f"episode-{eid}"
            episode.mkdir(mode=0o700)
            registry, evidence = episode / "incumbent", episode / "evidence"
            registry.mkdir(mode=0o700)
            evidence.mkdir(mode=0o700)
            groups = [make_group(rng, eid, task, family, oracle) for task in range(4)]
            validate_tags(groups)
            # Positive random composition of 32; observed and fresh demand
            # counts match, but their actual root values are disjoint.
            weights = [4] * 4
            for _ in range(16):
                weights[rng.randrange(4)] += 1
            cursor = rng.randrange(4)
            probes, observed, exhaustive = [], [], []
            for group, weight in zip(groups, weights):
                possible = [(plan, key) for plan in group["plans"] for key in range(64)]
                sampled = rng.sample(possible, weight * 2)
                observed.extend(request(group, plan, key, oracle) for plan, key in sampled[:weight])
                probes.extend(request(group, plan, key, oracle) for plan, key in sampled[weight:])
                exhaustive.extend(request(group, plan, key, oracle) for plan, key in possible)
            metadata = {"episode": eid, "split": split, "family": family, "cursor": cursor,
                        "weights": weights, "groups": groups, "observed": observed, "fresh": probes}
            save(episode / "inputs.json", canonical(metadata))
            initial = {}
            for group in groups:
                for index in group["installed"]:
                    block = group["blocks"][index]
                    source = evidence / (block["unit"] + ".tsv")
                    write_source(source, block)
                    identity, build_sha = teach(binary, env, registry, block, source, evidence / (block["unit"] + ".build.json"))
                    initial[block["unit"]] = {"artifacts": identity, "build_sha256": build_sha}
                    totals["initial_capsules"] += 1
            handle = open_registry(runtime, registry)
            try:
                prior = [ask(runtime, handle, row["request"], row["expected"]) for row in observed]
                if any(row["verified"] for row in prior):
                    raise RuntimeError("observed_request_was_not_unmet")
                save(episode / "observed.json", canonical(prior))
            finally:
                runtime.lib.cnet_capsule_core_close(handle)
            tasks = "CNET_ALLOCATOR_TASKS_V1\n"
            for group, weight in zip(groups, weights):
                task = group["task"]
                destination = episode / f"task-{task}"
                destination.mkdir(mode=0o700)
                block = group["blocks"][group["frontier"]]
                source = destination / "source.tsv"
                supplied, source_sha = write_source(source, block, group["bad_evidence"])
                valid = all(oracle[block["operand"]][key] == value for key, value in supplied)
                if valid == group["bad_evidence"]:
                    raise RuntimeError("evidence_rejection_fixture_mismatch")
                candidate, identity, build_sha = None, None, None
                if valid:
                    staging = destination / "candidate_registry"
                    shutil.copytree(registry, staging)
                    identity, build_sha = teach(binary, env, staging, block, source, destination / "build.json")
                    candidate = staging / block["unit"]
                    totals["successful_acquisitions"] += 1
                else:
                    totals["evidence_rejections"] += 1
                result = trial(runtime, registry, candidate, probes, exhaustive)
                result.update({"episode_input_sha256": sha(canonical(metadata)), "incumbent": initial,
                               "source_sha256": source_sha, "oracle_sha256": frozen["oracle"],
                               "oracle_call_receipts": [oracle_receipts[block["operand"]][key] for key, _ in supplied],
                               "reference": "integer XOR, independently checked against every cached native call",
                               "evidence_accepted": valid, "candidate_artifacts": identity, "build_sha256": build_sha,
                               "compiler_sha256": frozen["compiler"], "runtime_sha256": frozen["runtime"],
                               "producer_sha256": frozen["producer"], "production_admission": False})
                receipt = canonical(result)
                save(destination / "receipt.json", receipt)
                f1 = len(group["installed"]) / len(group["blocks"])
                row = f"{eid}\t{task}\t{family}\t{cursor}\t32\t1\t32\t4\t{weight / 32:.9g}\t{f1:.9g}\t0.015625\t32\t0"
                tasks += row + "\n"
                output_rows.append(row + f"\t{result['derived_mask']}\t{sha(receipt)}\n")
                totals["full_domain_checks"] += len(exhaustive)
            save(episode / "tasks.tsv", tasks.encode())
            totals["episodes"] += 1
            if totals["episodes"] % 4 == 0:
                print(json.dumps({"event": "composition_progress", "episodes": totals["episodes"]}), flush=True)
        raw = ("CNET_ALLOCATOR_ROWS_V1\n" + "".join(output_rows)).encode()
        save(root / f"{split}.tsv", raw)
        digests[split] = sha(raw)
    summary = {**totals, "root": str(root), "split_sha256": digests,
               "family_episodes_each_split": [8, 8, 8, 8], "gate_invoked": False,
               "checkpoint_trained": False, "confirmation_utilities": "unreported pending checkpoint freeze",
               "gain_claim": "WITHHELD", "production_admission": False,
               "scope": PROTOCOL["scope"]}
    save(root / "summary.json", canonical(summary))
    print(json.dumps(summary, sort_keys=True), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--oracle", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    run(parser.parse_args())
