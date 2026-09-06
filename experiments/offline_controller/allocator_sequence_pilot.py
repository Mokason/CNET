#!/usr/bin/env python3
"""Development-only actual native acquisition trajectories; never activation."""
from collections import Counter
import argparse
import ctypes
import json
import math
import os
from pathlib import Path
import random
import resource
import shutil
import sys
import tempfile
import time

from allocator_campaign import Runtime, Reply, bounded, canonical, freeze_binary, save, sha


POLICIES = ("cursor", "demand_cost", "completion", "marginal", "bundle", "exact")
PROTOCOL = {
    "kind": "allocator_sequence_DEVELOPMENT_pilot_v1", "episodes": 16,
    "families": ["direct", "shared-prefix", "chain-completion", "evidence-rejection"],
    "seed": 690607411, "tasks": 32, "jobs": 8, "work": 512,
    "bits": 6, "keys": 64, "primitive_rows": [48, 64], "requests": 64,
    "policies": list(POLICIES), "exact_cpu_seconds": 1,
    "reference_cpu_seconds": 30, "campaign_boottime_seconds": 900,
    "child_seconds": 30, "child_memory_mib": 512, "child_pipe_bytes": 65536,
    "fairness": "task zero is overdue and mandatory for all policies; evidence rejection is observed there",
    "planned_information": "public graph, planned exact tool domains/transforms, demand, installed dependencies, costs, ages, cursor",
    "score": "native verified final demand answers / 64 after whole acquisition trajectory",
    "labels": "native xor tool independently checked by integer xor, never CNET answers",
    "production_admission": False, "generator": "trusted self-generated finite XOR experiments, generic teach is unsealed",
    "scope": "16 distinct DEVELOPMENT graphs within four recurring motifs; denser NEW workload, not repaired old holdout",
    "gate": "not run; unchanged gain>=.05, positive paired95, all families and old certificate floors remain required",
    "stop": "no fitting/confirmation/GPU unless >=.05 headroom is demonstrated against strongest control with proven bound",
    "compile_reuse": "each candidate really compiled once; each policy independently admits its sequence with equal row-work charges, not a wall-speed comparison",
}

def coverage(requirements, selected):
    return sum(any(selected & path == path for path in paths) for paths in requirements)


def reachable_requirements(tasks, requests, installed=()):
    """Prospective plans, NOT outcome labels. Inputs/costs/domains are public.

    Values are transformed at every hop. Superset paths cannot improve a
    monotone acquisition set, so remove them. Refuse enumeration exhaustion.
    """
    if len(tasks) > 32 or len(installed) > 32 or len(requests) > 2048:
        raise ValueError("sequence_graph_cap")
    adjacency = {}
    for index, task in enumerate(tasks + list(installed)):
        adjacency.setdefault(task["input"], []).append((task, 1 << index if index < len(tasks) else 0))
    result = []
    for source, target, key in requests:
        pending, paths, seen = [(source, key, 0, 0)], set(), 0
        while pending:
            port, value, mask, depth = pending.pop()
            seen += 1
            if seen > 4096:
                raise ValueError("sequence_path_cap")
            if depth == 8:
                continue
            for task, bit in adjacency.get(port, ()):
                if value not in task["keys"]:
                    continue
                next_value, next_mask = value ^ task["operand"], mask | bit
                if task["output"] == target:
                    paths.add(next_mask)
                else:
                    pending.append((task["output"], next_value, next_mask, depth + 1))
        minimal = tuple(sorted(path for path in paths if not any(other != path and other & path == other for other in paths)))
        result.append(minimal)
    return result


def plan(tasks, requirements, policy, jobs=8, work=512, cursor=0, cpu_seconds=1,
         clock=None):
    """Fair finite planner. Rejected evidence is only a shared overdue outcome.

    DFS retains at most N+1 frontier nodes. Its upper bound distributes each
    still-possible request's weight over every relevant action at weight/r,
    where r is its smallest feasible missing-path cardinality. Any completion
    must consume at least r such actions, making top-K weights an upper bound.
    """
    if policy not in POLICIES or not 1 <= jobs <= 8 or not 1 <= work <= 512 or not 0 < cpu_seconds <= 30:
        raise ValueError("sequence_budget")
    if not 1 <= len(tasks) <= 32 or not 1 <= len(requirements) <= 2048:
        raise ValueError("sequence_size")
    if any(not 1 <= task["cost"] <= 64 or not 0 <= task["age"] <= 4 or
           (task.get("rejected") and task["age"] != 4) for task in tasks):
        raise ValueError("sequence_task")
    clock = clock or time.process_time
    start, wall = clock(), time.monotonic()
    n = len(tasks)
    rank = {i: (i - cursor) % n for i in range(n)}
    forced = sorted((i for i, task in enumerate(tasks) if task["age"] >= 4), key=lambda i: (-tasks[i]["age"], rank[i]))
    actions, spent, selected = [], 0, 0
    for index in forced:
        if len(actions) == jobs or spent + tasks[index]["cost"] > work:
            raise ValueError("sequence_fairness_infeasible")
        actions.append(index)
        spent += tasks[index]["cost"]
        if not tasks[index].get("rejected"):
            selected |= 1 << index
    available = [i for i in range(n) if i not in actions]
    grouped = list(Counter(tuple(paths) for paths in requirements).items())

    def value(mask):
        return sum(weight for paths, weight in grouped if any(mask & path == path for path in paths))

    def greedy(kind):
        chosen, used, mask = list(actions), spent, selected
        while len(chosen) < jobs:
            possible = [i for i in available if i not in chosen and used + tasks[i]["cost"] <= work]
            if not possible:
                break
            before = value(mask)
            if kind == "bundle":
                bundles = {(i,) for i in possible}
                possible_mask = sum(1 << i for i in possible)
                for paths, _ in grouped:
                    for path in paths:
                        missing = path & ~mask
                        if missing and missing & possible_mask == missing:
                            bundle = tuple(i for i in possible if missing >> i & 1)
                            if len(bundle) <= jobs - len(chosen) and sum(tasks[i]["cost"] for i in bundle) <= work - used:
                                bundles.add(bundle)
                def score(bundle):
                    cost = sum(tasks[i]["cost"] for i in bundle)
                    gain = value(mask | sum(1 << i for i in bundle)) - before
                    return (gain / cost, gain, -len(bundle), tuple(-rank[i] for i in bundle))
                additions = list(max(bundles, key=score))
            else:
                def score(index):
                    bit, cost = 1 << index, tasks[index]["cost"]
                    touching = sum(weight for paths, weight in grouped if any(path & bit for path in paths))
                    completion = max((1 - (path & ~mask).bit_count() / max(1, path.bit_count())
                                      for paths, _ in grouped for path in paths if path & bit), default=0)
                    if "features" in tasks[index]:
                        touching, completion = tasks[index]["features"][:2]
                    primary = {"cursor": -rank[index], "demand_cost": touching / cost,
                               "completion": completion, "marginal": (value(mask | bit) - before) / cost}[kind]
                    return (primary, touching / cost if kind == "completion" else 0, -rank[index])
                additions = [max(possible, key=score)]
            for index in additions:
                chosen.append(index)
                used += tasks[index]["cost"]
                mask |= 1 << index
        return chosen, used, mask

    nodes, complete, initialization_cpu = 0, True, 0.0
    if policy != "exact":
        best_actions, best_work, best_mask = greedy(policy)
        best_value = value(best_mask)
        upper = len(requirements)  # No proof is claimed for a heuristic.
        complete = False
    else:
        starters = [greedy(kind) for kind in ("bundle", "marginal", "demand_cost", "completion")]
        initialization_cpu = clock() - start
        best_actions, best_work, best_mask = max(starters, key=lambda row: (value(row[2]), -row[1]))
        best_value = value(best_mask)
        ordering = sorted(available, key=lambda i: (i not in best_actions, rank[i]))
        suffix = [0] * (len(ordering) + 1)
        for pos in range(len(ordering) - 1, -1, -1):
            suffix[pos] = suffix[pos + 1] | (1 << ordering[pos])

        def bound(mask, remaining, slots, money):
            current, possible = 0, 0
            impact = [0.0] * n
            for paths, weight in grouped:
                if any(mask & path == path for path in paths):
                    current += weight
                    continue
                feasible = []
                for path in paths:
                    missing = path & ~mask
                    if missing and missing & remaining == missing and missing.bit_count() <= slots and sum(tasks[i]["cost"] for i in ordering if missing >> i & 1) <= money:
                        feasible.append(missing)
                if feasible:
                    possible += weight
                    union, minimum = 0, min(item.bit_count() for item in feasible)
                    for item in feasible:
                        union |= item
                    for index in ordering:
                        if union >> index & 1:
                            impact[index] += weight / minimum
            fractional = math.floor(sum(sorted(impact, reverse=True)[:slots]) + 1e-7)
            return current + min(possible, fractional)

        initial_bound = bound(selected, suffix[0], jobs - len(actions), work - spent)
        stack = [(0, selected, tuple(actions), spent, initial_bound)]
        while stack:
            if clock() - start >= cpu_seconds:
                complete = False
                break
            pos, mask, chosen, used, upper = stack.pop()
            nodes += 1
            if upper <= best_value:
                continue
            current = value(mask)
            if current > best_value:
                best_actions, best_work, best_mask, best_value = list(chosen), used, mask, current
            if pos == len(ordering) or len(chosen) == jobs:
                continue
            index, next_pos = ordering[pos], pos + 1
            skipped = bound(mask, suffix[next_pos], jobs - len(chosen), work - used)
            if skipped > best_value:
                stack.append((next_pos, mask, chosen, used, skipped))
            if used + tasks[index]["cost"] <= work:
                included = bound(mask | (1 << index), suffix[next_pos], jobs - len(chosen) - 1, work - used - tasks[index]["cost"])
                if included > best_value:
                    stack.append((next_pos, mask | (1 << index), chosen + (index,), used + tasks[index]["cost"], included))
                elif value(mask | (1 << index)) > best_value:
                    best_actions, best_work, best_mask = list(chosen) + [index], used + tasks[index]["cost"], mask | (1 << index)
                    best_value = value(best_mask)
        upper = max([best_value] + [node[4] for node in stack])
    elapsed_cpu = clock() - start
    return {"policy": policy, "actions": best_actions, "work": best_work,
            "value": best_value, "upper_bound": upper, "complete": complete,
            "nodes": nodes, "cpu_seconds": elapsed_cpu,
            "initialization_cpu_seconds": initialization_cpu,
            "initialization_overrun": initialization_cpu > cpu_seconds,
            "cap_overrun_seconds": max(0, elapsed_cpu - cpu_seconds),
            "wall_seconds": time.monotonic() - wall, "cpu_cap_seconds": cpu_seconds}


def make_episode(number):
    rng = random.Random(PROTOCOL["seed"] + number)
    family = number % 4
    pairs, goals = [], []
    if family == 0:
        goals = [f"g{i}" for i in range(32)]
        pairs = [("root", goal) for goal in goals]
    elif family == 1:
        middle = [f"m{i}" for i in range(8)]
        goals = [f"g{i}" for i in range(8)]
        pairs = [("root", item) for item in middle]
        for i, goal in enumerate(goals):
            parents = {(i + offset) % 8 for offset in (0, 1, 3)}
            pairs.extend((middle[parent], goal) for parent in sorted(parents))
    else:
        left, right = [f"l{i}" for i in range(4)], [f"r{i}" for i in range(8)]
        goals = [f"g{i}" for i in range(8)]
        pairs = [("root", item) for item in left]
        pairs.extend((left[i % 4], right[i]) for i in range(8))
        for i, goal in enumerate(goals):
            pairs.extend((right[(i + offset) % 8], goal) for offset in ((0, 2, 5) if i < 4 else (0, 3)))
    assert len(pairs) == 32
    # Queue order and vertex potentials are fresh per episode; every pending
    # edge is on a requested plan. Alternatives agree by endpoint potentials.
    rng.shuffle(pairs)
    endpoints = sorted({endpoint for pair in pairs for endpoint in pair})
    potential = {endpoint: rng.randrange(4) for endpoint in endpoints}
    tags = {endpoint: "p" + sha(f"sequence-{number}-{endpoint}".encode())[:24] for endpoint in endpoints}
    if any(sum(a != b for a, b in zip(left, right)) <= 2 for i, left in enumerate(tags.values()) for right in list(tags.values())[i + 1:]):
        raise RuntimeError("sequence_tag_near_miss")
    tasks = []
    for i, (source, target) in enumerate(pairs):
        keys = sorted(rng.sample(range(64), rng.randint(48, 64)))
        tasks.append({"unit": f"mined_sequence{number}t{i}", "input": tags[source], "output": tags[target],
                      "keys": keys, "operand": potential[source] ^ potential[target], "cost": len(keys),
                      "age": 4 if i == 0 else rng.randrange(4), "rejected": family == 3 and i == 0})
    requests = []
    # Balanced distinct current requests; request keys and domains are public.
    for goal in goals:
        for key in rng.sample(range(64), 64 // len(goals)):
            requests.append([tags["root"], tags[goal], key])
    rng.shuffle(requests)
    requirements = reachable_requirements(tasks, requests)
    structural = reachable_requirements([{**task, "keys": list(range(64))} for task in tasks], requests)
    for i, task in enumerate(tasks):
        # Exact original chooser contracts, evaluated on the richer public
        # topology: structural demand fraction, already certified fraction=0,
        # missing rows/2048. All capsules in this pilot start missing.
        f0 = sum(any(path >> i & 1 for path in paths) for paths in structural) / len(requests)
        task["features"] = [f0, 0.0, task["cost"] / 2048]
    return {"number": number, "family": PROTOCOL["families"][family], "tasks": tasks,
            "requests": requests, "requirements": requirements, "cursor": rng.randrange(32),
            "goals": [[tags["root"], tags[goal], potential["root"] ^ potential[goal]] for goal in goals]}


def native_choice(library, episode, policy):
    class Task(ctypes.Structure):
        _fields_ = [("id", ctypes.c_uint64), ("features", ctypes.c_float * 3),
                    ("cost", ctypes.c_uint), ("age", ctypes.c_uint)]
    class Choice(ctypes.Structure):
        _fields_ = [("count", ctypes.c_uint), ("work", ctypes.c_uint), ("index", ctypes.c_uint * 8)]
    method = library.cnet_core_allocator_choose
    method.argtypes = [ctypes.c_void_p, ctypes.POINTER(Task), ctypes.c_size_t,
                       ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(Choice)]
    method.restype = ctypes.c_int
    tasks = (Task * len(episode["tasks"]))()
    for i, source in enumerate(episode["tasks"]):
        tasks[i].id, tasks[i].cost, tasks[i].age = i + 1, source["cost"], source["age"]
        tasks[i].features[:] = source["features"]
    output = Choice()
    if method(None, tasks, len(tasks), episode["cursor"], 8, 512, 4, POLICIES.index(policy) + 1, ctypes.byref(output)):
        raise RuntimeError("native_original_chooser_refused")
    return list(output.index[:output.count]), output.work


def open_core(runtime, registry, candidate=None):
    error = ctypes.create_string_buffer(160)
    if candidate is None:
        handle = runtime.lib.cnet_capsule_core_open(os.fsencode(registry), error, len(error))
    else:
        handle = runtime.lib.cnet_capsule_core_open_candidate(os.fsencode(registry), os.fsencode(candidate), error, len(error))
    if not handle:
        raise RuntimeError("canonical_import_refused: " + error.value.decode())
    return handle


def native_probe(runtime, handle, request, expected):
    reply = Reply()
    query = f"capsule {request[0]} {request[1]} {request[2]}"
    rc = runtime.lib.cnet_capsule_core_ask(handle, query.encode(), ctypes.byref(reply))
    if reply.verified and (rc or reply.value != expected) or not reply.verified and not rc:
        raise RuntimeError("independent_value_or_verified_status_mismatch")
    return {"request": query, "expected": expected, "rc": rc, "verified": reply.verified,
            "value": reply.value, "hops": reply.hops, "reason": reply.reason.decode()}


def capsule_identity(path):
    names = sorted(item.name for item in path.iterdir())
    if names != ["manifest.cknow", "unit.cnb"]:
        raise RuntimeError("sequence_capsule_inventory")
    return {name: sha((path / name).read_bytes()) for name in names}


def trajectory(specification):
    """Disposable, bounded process: no subprocesses and no training authority."""
    spec = json.loads(Path(specification).read_bytes())
    episode, decision = spec["episode"], spec["decision"]
    root, binary = Path(spec["root"]), Path(spec["binary"])
    registry = root / "registry"
    registry.mkdir(mode=0o700)
    runtime = Runtime(binary / "libcnet_capsule_core.so")
    if decision["policy"] in POLICIES[:3]:
        actual_actions, actual_work = native_choice(ctypes.CDLL(str(binary / "libcnet_allocator_control.so")), episode, decision["policy"])
        if (actual_actions, actual_work) != (decision["actions"], decision["work"]):
            raise RuntimeError("original_native_chooser_parity")
    values = spec["oracle"]
    goal_operand = {(a, b): operand for a, b, operand in episode["goals"]}
    selected, previous_verified, checks, steps = 0, set(), 0, []
    for step, index in enumerate(decision["actions"]):
        task = episode["tasks"][index]
        source_path = Path(spec["bank"]) / f"rows-{index}.tsv"
        raw = source_path.read_bytes()
        if sha(raw) != spec["sources"][str(index)]:
            raise RuntimeError("source_identity_changed")
        rows = [tuple(map(int, line.split())) for line in raw.decode("ascii").splitlines()]
        agrees = rows == [(key, values[str(task["operand"])][str(key)]) for key in task["keys"]]
        before = open_core(runtime, registry)
        after = None
        try:
            obligations = ctypes.c_size_t()
            if not agrees:
                if not task["rejected"] or step != 0:
                    raise RuntimeError("unexpected_tool_evidence_rejection")
                after = open_core(runtime, registry)
            else:
                candidate = Path(spec["bank"]) / task["unit"] / task["unit"]
                if capsule_identity(candidate) != spec["capsules"][str(index)]:
                    raise RuntimeError("candidate_identity_changed")
                after = open_core(runtime, registry, candidate)
                if runtime.lib.cnet_capsule_core_validate_growth(before, after, ctypes.byref(obligations)):
                    raise RuntimeError("canonical_growth_refused")
                selected |= 1 << index
            probes, now_verified = [], set()
            for j, request in enumerate(episode["requests"]):
                expected = values[str(goal_operand[tuple(request[:2])])][str(request[2])]
                result = native_probe(runtime, after, request, expected)
                predicted = any(selected & path == path for path in episode["requirements"][j])
                if bool(result["verified"]) != predicted:
                    raise RuntimeError("native_planned_reachability_disagreement")
                if result["verified"]:
                    now_verified.add(j)
                probes.append(result)
            if not previous_verified <= now_verified:
                raise RuntimeError("old_demand_interference")
            previous_verified = now_verified
            checks += len(probes)
            receipt = {"step": step, "action": index, "source_sha256": sha(raw), "accepted": agrees,
                       "charged_rows": task["cost"], "growth_obligations": obligations.value,
                       "candidate": spec["capsules"].get(str(index)), "probes": probes}
            save(root / f"step-{step}.json", canonical(receipt))
            steps.append({k: v for k, v in receipt.items() if k != "probes"})
            if agrees:
                shutil.copytree(candidate, registry / task["unit"])
        finally:
            if after:
                runtime.lib.cnet_capsule_core_close(after)
            runtime.lib.cnet_capsule_core_close(before)
    final = open_core(runtime, registry)
    try:
        requests = [[a, b, key] for a, b, _ in episode["goals"] for key in range(64)]
        requirements = reachable_requirements(episode["tasks"], requests)
        full = []
        for request, paths in zip(requests, requirements):
            expected = values[str(goal_operand[tuple(request[:2])])][str(request[2])]
            result = native_probe(runtime, final, request, expected)
            if bool(result["verified"]) != any(selected & path == path for path in paths):
                raise RuntimeError("final_full_domain_reachability_disagreement")
            full.append(result)
        checks += len(full)
        save(root / "final-full-domain.json", canonical(full))
    finally:
        runtime.lib.cnet_capsule_core_close(final)
    if len(previous_verified) != decision["value"]:
        raise RuntimeError("native_trajectory_score_disagreement")
    result = {"native_value": len(previous_verified), "full_domain_checks": checks, "wrong_verified": 0,
              "steps": steps, "policy": decision["policy"], "specification_sha256": sha(Path(specification).read_bytes())}
    save(root / "result.json", canonical(result))
    print(json.dumps({k: v for k, v in result.items() if k != "steps"}), flush=True)


def run(args):
    start_wall, start_cpu = time.clock_gettime(time.CLOCK_BOOTTIME), time.process_time()
    root = Path(tempfile.mkdtemp(prefix="cnet-allocator-sequence-"))
    print(json.dumps({"event": "DEVELOPMENT_started", "root": str(root)}), flush=True)
    binary = root / "bin"
    binary.mkdir(mode=0o700)
    frozen = {}
    for label, source, name, executable in (
        ("compiler", args.compiler, "cnet_capsule_core", True),
        ("oracle", args.oracle, "cnet_capsule_tool", True),
        ("runtime", args.library, "libcnet_capsule_core.so", False),
        ("native_controls", args.chooser_library, "libcnet_allocator_control.so", False),
    ):
        frozen[label] = freeze_binary(Path(source), binary / name, executable)
    for name in ("allocator_sequence_pilot.py", "allocator_campaign.py", "allocator_sequence_pilot_test.py", "allocator_sequence_pilot_contract.md"):
        frozen[name] = freeze_binary(Path(__file__).with_name(name), root / name, False)
    source_root = Path(__file__).resolve().parents[2]
    for relative in ("src/serve/cnet_core_allocator.c", "src/serve/cnet_core_cell.c", "include/cnet_core_allocator.h", "include/cnet_core_cell.h"):
        frozen[relative] = freeze_binary(source_root / relative, root / Path(relative).name, False)
    protocol = {**PROTOCOL, "artifacts": frozen, "interpreter": sys.executable,
                "runtime_loader_scope": "owner-trusted installed Python/system shared libraries, not a deployment manifest"}
    save(root / "protocol.json", canonical(protocol))
    save(root / "freeze.json", canonical({"before_native_outcomes": True, "protocol_sha256": sha(canonical(protocol)), "training": False, "confirmation": False}))
    env = {"PATH": "/usr/bin:/bin", "LANG": "C", "PYTHONDONTWRITEBYTECODE": "1"}
    calls = 0

    def call(argv, receipt):
        nonlocal calls
        remaining = 900 - (time.clock_gettime(time.CLOCK_BOOTTIME) - start_wall)
        if remaining <= 0:
            raise RuntimeError("campaign_boottime_deadline")
        cpu_before = resource.getrusage(resource.RUSAGE_CHILDREN)
        child = bounded(argv, env, min(30, remaining), experimental_limits=True)
        cpu_after = resource.getrusage(resource.RUSAGE_CHILDREN)
        child["cpu_seconds"] = cpu_after.ru_utime + cpu_after.ru_stime - cpu_before.ru_utime - cpu_before.ru_stime
        save(receipt, canonical(child))
        calls += 1
        if child["returncode"] or child["refused"]:
            raise RuntimeError(f"bounded_native_child_refused: {receipt}")
        return child

    oracle, oracle_receipts = {}, {}
    oracle_root = root / "oracle"
    oracle_root.mkdir(mode=0o700)
    for operand in range(4):
        oracle[operand], oracle_receipts[operand] = {}, {}
        for key in range(64):
            result = call([str(binary / "cnet_capsule_tool"), "xor", str(operand), str(key)], oracle_root / f"{operand}-{key}.json")
            if result["stdout"] != str(key ^ operand) + "\n":
                raise RuntimeError("native_oracle_disagreement")
            oracle[operand][key], oracle_receipts[operand][key] = int(result["stdout"]), sha(canonical(result))
    save(root / "oracle-values.json", canonical({"values": oracle, "receipts": oracle_receipts}))
    episodes = []
    for number in range(16):
        if time.clock_gettime(time.CLOCK_BOOTTIME) - start_wall >= 900:
            raise RuntimeError("campaign_boottime_deadline")
        episode = make_episode(number)
        directory = root / f"episode-{number}"
        directory.mkdir(mode=0o700)
        save(directory / "prospective.json", canonical(episode))
        decisions = [plan(episode["tasks"], episode["requirements"], policy, cursor=episode["cursor"]) for policy in POLICIES]
        reference = plan(episode["tasks"], episode["requirements"], "exact", cursor=episode["cursor"], cpu_seconds=30)
        reference["policy"] = "reference"
        decisions.append(reference)
        save(directory / "decisions-before-native.json", canonical(decisions))
        print(json.dumps({"event": "plans_frozen", "episode": number, "family": episode["family"],
                          "values": {d["policy"]: d["value"] for d in decisions},
                          "reference_upper": reference["upper_bound"], "reference_complete": reference["complete"],
                          "reference_cpu_seconds": reference["cpu_seconds"]}), flush=True)
        bank = directory / "bank"
        bank.mkdir(mode=0o700)
        sources, capsules = {}, {}
        for index, task in enumerate(episode["tasks"]):
            rows = [[key, oracle[task["operand"]][key]] for key in task["keys"]]
            if task["rejected"]:
                rows[0][1] ^= 1
            raw = "".join(f"{key}\t{value}\n" for key, value in rows).encode()
            source = bank / f"rows-{index}.tsv"
            save(source, raw)
            sources[index] = sha(raw)
            if task["rejected"]:
                continue
            registry = bank / task["unit"]
            result = call([str(binary / "cnet_capsule_core"), "teach", str(registry), task["unit"],
                           task["input"], task["output"], "6", "6", "verified_tool", str(source)], bank / f"build-{index}.json")
            if "method=finite_domain_compile" not in result["stdout"]:
                raise RuntimeError("unexpected_native_training_method")
            for artifact in (registry / task["unit"]).iterdir():
                artifact.chmod(0o400)
            capsules[index] = capsule_identity(registry / task["unit"])
        native = []
        for decision in decisions:
            child_root = directory / decision["policy"]
            child_root.mkdir(mode=0o700)
            spec = {"episode": episode, "decision": decision, "root": str(child_root), "binary": str(binary),
                    "bank": str(bank), "sources": sources, "capsules": capsules, "oracle": oracle,
                    "oracle_receipts_sha256": sha(canonical(oracle_receipts))}
            specification = child_root / "specification.json"
            save(specification, canonical(spec))
            result = call([sys.executable, str(root / "allocator_sequence_pilot.py"), "--trajectory", str(specification)], child_root / "call.json")
            native.append(json.loads(result["stdout"]))
        record = {"episode": number, "family": episode["family"], "decisions": decisions, "native": native,
                  "prospective_sha256": sha(canonical(episode))}
        save(directory / "summary.json", canonical(record))
        episodes.append(record)
        print(json.dumps({"event": "native_episode_complete", "episode": number,
                          "native_values": {result["policy"]: result["native_value"] for result in native},
                          "checks": sum(result["full_domain_checks"] for result in native)}), flush=True)
    policy_values = {policy: sum(next(d["native_value"] for d in e["native"] if d["policy"] == policy) for e in episodes) / (16 * 64) for policy in (*POLICIES, "reference")}
    upper = sum(e["decisions"][-1]["upper_bound"] for e in episodes) / (16 * 64)
    summary = {"kind": PROTOCOL["kind"], "episodes": 16, "coverage": policy_values,
               "reference_proven_episodes": sum(e["decisions"][-1]["complete"] for e in episodes),
               "reference_upper_mean": upper, "strongest_control_upper_headroom": upper - max(policy_values[p] for p in POLICIES),
               "native_calls": calls, "checks": sum(r["full_domain_checks"] for e in episodes for r in e["native"]),
               "wrong_verified": 0, "parent_cpu_seconds": time.process_time() - start_cpu,
               "boottime_seconds": time.clock_gettime(time.CLOCK_BOOTTIME) - start_wall,
               "gain_claim": "WITHHELD", "trained": False, "confirmation": False, "activated": False}
    save(root / "summary.json", canonical(summary))
    print(json.dumps({"event": "DEVELOPMENT_complete", "root": str(root), **summary}), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler")
    parser.add_argument("--oracle")
    parser.add_argument("--library")
    parser.add_argument("--chooser-library")
    parser.add_argument("--trajectory")
    arguments = parser.parse_args()
    if arguments.trajectory:
        trajectory(arguments.trajectory)
    elif all((arguments.compiler, arguments.oracle, arguments.library, arguments.chooser_library)):
        run(arguments)
    else:
        parser.error("all four fixed native artifacts are required")
