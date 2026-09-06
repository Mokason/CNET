#!/usr/bin/env python3
"""Actual native table outcomes; a frozen-feature limitation, NOT gain evidence."""
import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import random
import re
import resource
import selectors
import stat
import subprocess
import tempfile
import time

PROTOCOL = {
    "kind": "allocator_table_feature_aliasing_v1",
    "episodes": 32, "development": 16, "confirmation": 16,
    "development_seed": 690602101, "confirmation_seed": 690602103,
    "tasks_per_episode": [2, 3, 4], "rows_per_task": [8, 12, 16, 24],
    "observed_probes_per_task": 2, "fresh_probes_per_task": 16,
    "full_domain_keys_per_task": 256, "jobs": 1, "wait_limit": 4,
    "native_worker_timeout_seconds": 30, "native_worker_cpu_seconds": 30,
    "native_worker_memory_mib": 512, "pipe_cap_bytes": 65536,
    "family": "direct", "missing_gate_families": [1, 2, 3],
    "candidate": "untrained deterministic initialization, diagnostic only",
    "claim": "identical online vectors force tied choices on this workload",
    "not_claimed": "impossibility of allocator improvement on every workload",
}


def canonical(value):
    return (json.dumps(value, sort_keys=True, separators=(",", ":")) + "\n").encode()


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def save(path, raw):
    with path.open("xb") as stream:
        stream.write(raw)
        stream.flush()
        os.fsync(stream.fileno())
    path.chmod(0o600)


def bounded(argv, env, deadline=30, experimental_limits=False):
    """Deadline includes pipe drain and uses suspend-inclusive BOOTTIME."""
    def limits():
        resource.setrlimit(resource.RLIMIT_CPU, (30, 30))
        resource.setrlimit(resource.RLIMIT_AS, (512 * 1024 * 1024,) * 2)
        resource.setrlimit(resource.RLIMIT_FSIZE, (64 * 1024 * 1024,) * 2)
        resource.setrlimit(resource.RLIMIT_NOFILE, (64, 64))
    start = time.clock_gettime(time.CLOCK_BOOTTIME)
    proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=env, close_fds=True,
                            preexec_fn=limits if experimental_limits else None)
    output = [bytearray(), bytearray()]
    refused = None
    try:
        with selectors.DefaultSelector() as poll:
            for index, stream in enumerate((proc.stdout, proc.stderr)):
                os.set_blocking(stream.fileno(), False)
                poll.register(stream, selectors.EVENT_READ, index)
            while poll.get_map() or proc.poll() is None:
                if time.clock_gettime(time.CLOCK_BOOTTIME) - start >= deadline:
                    refused = "boottime_deadline"
                    break
                for key, _ in poll.select(0.05):
                    chunk = os.read(key.fileobj.fileno(), 4096)
                    if not chunk:
                        poll.unregister(key.fileobj)
                        continue
                    target = output[key.data]
                    if len(target) + len(chunk) > PROTOCOL["pipe_cap_bytes"]:
                        refused = "pipe_limit"
                        break
                    target.extend(chunk)
                if refused:
                    break
        if refused and proc.poll() is None:
            proc.kill()
        code = proc.wait(timeout=2)
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=2)
        proc.stdout.close()
        proc.stderr.close()
    return {"argv": argv, "returncode": code, "refused": refused,
            "seconds": time.clock_gettime(time.CLOCK_BOOTTIME) - start,
            "stdout": output[0].decode("ascii"), "stderr": output[1].decode("ascii")}


def reference(raw, name):
    """Independent strict reference; does not call the C table decoder."""
    lines = raw.decode("ascii").splitlines(keepends=True)
    prefix = ["CNET_LOCAL_TABLE_V1\n", f"dataset {name}\n", "authority verified_tool\n",
              "input_bits 8\n", "output_bits 16\n"]
    if lines[:5] != prefix or len(lines) < 7 or not re.fullmatch(r"rows [1-9][0-9]*\n", lines[5]):
        raise ValueError("reference_header")
    count = int(lines[5][5:])
    if not 1 <= count <= 256 or len(lines) != count + 6:
        raise ValueError("reference_count")
    values = {}
    previous = -1
    for line in lines[6:]:
        if not re.fullmatch(r"(0|[1-9][0-9]*)\t(0|[1-9][0-9]*)\n", line):
            raise ValueError("reference_row")
        key, value = map(int, line.split())
        if not previous < key <= 255 or value > 65535:
            raise ValueError("reference_range_order")
        values[key] = value
        previous = key
    return values


class Reply(ctypes.Structure):
    _fields_ = [("verified", ctypes.c_int), ("value", ctypes.c_uint),
                ("hops", ctypes.c_size_t), ("units", ctypes.c_char * 512),
                ("reason", ctypes.c_char * 160)]


class Cell(ctypes.Structure):
    _fields_ = [("weight", ctypes.c_float * 41)]


class Candidate(ctypes.Structure):
    _fields_ = [("cell", Cell), ("training", ctypes.c_char * 65),
                ("evaluation", ctypes.c_char * 65)]


class Runtime:
    def __init__(self, path):
        self.lib = ctypes.CDLL(str(path))
        for name, arguments, result in (
            ("cnet_capsule_core_open", [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t], ctypes.c_void_p),
            ("cnet_capsule_core_open_candidate", [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t], ctypes.c_void_p),
            ("cnet_capsule_core_validate_growth", [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t)], ctypes.c_int),
            ("cnet_capsule_core_ask", [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(Reply)], ctypes.c_int),
            ("cnet_capsule_core_close", [ctypes.c_void_p], None),
            ("cnet_core_candidate_save_objective_at", [ctypes.c_int, ctypes.c_char_p, ctypes.POINTER(Candidate), ctypes.c_uint], ctypes.c_int),
        ):
            function = getattr(self.lib, name)
            function.argtypes, function.restype = arguments, result

    def ask(self, handle, request, expected):
        reply = Reply()
        result = self.lib.cnet_capsule_core_ask(handle, request.encode(), ctypes.byref(reply))
        correct = (result == 0 and reply.verified == 1 and reply.value == expected) if expected is not None else (result != 0 and reply.verified == 0)
        return {"request": request, "expected": expected, "returncode": result,
                "verified": reply.verified, "value": reply.value, "hops": reply.hops,
                "reason": reply.reason.decode(), "correct": correct}

    def verify(self, registry, candidate, dataset, values, probes):
        error = ctypes.create_string_buffer(160)
        before = self.lib.cnet_capsule_core_open(os.fsencode(registry), error, len(error))
        if not before:
            raise RuntimeError("baseline_import: " + error.value.decode())
        after = None
        try:
            after = self.lib.cnet_capsule_core_open_candidate(os.fsencode(registry), os.fsencode(candidate), error, len(error))
            if not after:
                raise RuntimeError("candidate_import: " + error.value.decode())
            obligations = ctypes.c_size_t()
            if self.lib.cnet_capsule_core_validate_growth(before, after, ctypes.byref(obligations)):
                raise RuntimeError("growth_refused")
            full = [self.ask(after, f"data {dataset} {key}", values.get(key)) for key in range(256)]
            observed = []
            mask = 0
            for index, (name, key, expected) in enumerate(probes):
                request = f"data {name} {key}"
                prior = self.ask(before, request, None)
                current = self.ask(after, request, expected if name == dataset else None)
                if current["verified"] and current["correct"] and not prior["verified"]:
                    mask |= 1 << index
                observed.append({"before": prior, "after": current})
            if not all(row["correct"] for row in full) or not all(pair[which]["correct"] for pair in observed for which in ("before", "after")):
                raise RuntimeError("independent_value_or_abstention_mismatch")
            return {"growth_obligations": obligations.value, "all_keys": full,
                    "fresh_probes": observed, "derived_mask": f"{mask:x}"}
        finally:
            if after:
                self.lib.cnet_capsule_core_close(after)
            self.lib.cnet_capsule_core_close(before)


def freeze_binary(source, destination, executable):
    fd = os.open(source, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC)
    with os.fdopen(fd, "rb") as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode) or before.st_uid != os.getuid() or before.st_nlink != 1 or before.st_size > 128 * 1024 * 1024:
            raise ValueError("trusted_artifact_identity")
        raw = stream.read(128 * 1024 * 1024 + 1)
        after = os.fstat(stream.fileno())
        if len(raw) != before.st_size or before.st_mtime_ns != after.st_mtime_ns or before.st_ctime_ns != after.st_ctime_ns:
            raise ValueError("trusted_artifact_changed_during_freeze")
    save(destination, raw)
    destination.chmod(0o500 if executable else 0o400)
    return sha(raw)


def run(args):
    root = Path(tempfile.mkdtemp(prefix="cnet-allocator-limitation-"))
    print(json.dumps({"event": "campaign_started", "root": str(root)}), flush=True)
    binary = root / "bin"
    binary.mkdir(mode=0o700)
    frozen = {
        "tool": freeze_binary(args.tool, binary / "cnet_table_capsule", True),
        "runtime": freeze_binary(args.library, binary / "libcnet_capsule_core.so", False),
        "chooser": freeze_binary(args.chooser, binary / "allocator_campaign", True),
        "producer": freeze_binary(Path(__file__), root / "allocator_campaign.py", False),
    }
    protocol = {**PROTOCOL, "artifacts": frozen}
    save(root / "protocol.json", canonical(protocol))
    runtime = Runtime(binary / "libcnet_capsule_core.so")
    checkpoint = Candidate()
    for i in range(41):
        checkpoint.cell.weight[i] = ((i * 37) % 101 - 50) / 100
    checkpoint.training = sha(b"untrained deterministic diagnostic initialization; not acquisition labels").encode()
    checkpoint.evaluation = sha(canonical(protocol)).encode()
    fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
    try:
        if runtime.lib.cnet_core_candidate_save_objective_at(fd, b"diagnostic.cell", ctypes.byref(checkpoint), 2):
            raise RuntimeError("diagnostic_checkpoint_refused")
    finally:
        os.close(fd)
    checkpoint_sha = sha((root / "diagnostic.cell").read_bytes())
    save(root / "freeze.json", canonical({"protocol_sha256": sha(canonical(protocol)), "checkpoint_sha256": checkpoint_sha,
                                         "trained": False, "before_any_episode_generation": True}))
    rows = {"development": [], "confirmation": []}
    summary = {"episodes": 0, "tasks": 0, "all_key_checks": 0, "different_utility_episodes": 0,
               "coverage_sum": [0.0] * 4, "wrong_verified": 0, "outcome_gain_claim": "WITHHELD"}
    measurements = []
    env = {"PATH": "/usr/bin:/bin", "LANG": "C"}
    for split in rows:
        rng = random.Random(PROTOCOL[split + "_seed"])
        for number in range(16):
            episode_id = (1 if split == "development" else 1001) + number
            episode = root / f"episode-{episode_id}"
            episode.mkdir(mode=0o700)
            data, registry = episode / "data", episode / "incumbent"
            data.mkdir(mode=0o700)
            registry.mkdir(mode=0o700)
            os.environ["CNET_CAPSULE_DATA_ROOT"] = str(data)
            count = rng.choice(PROTOCOL["tasks_per_episode"])
            width = rng.choice(PROTOCOL["rows_per_task"])
            cursor = rng.randrange(count)
            sources, probes = [], []
            for task in range(count):
                name = f"diag_{episode_id}_{task}"
                keys = sorted(rng.sample(range(256), width))
                values = {key: rng.randrange(65536) for key in keys}
                raw = (f"CNET_LOCAL_TABLE_V1\ndataset {name}\nauthority verified_tool\ninput_bits 8\noutput_bits 16\nrows {width}\n"
                       + "".join(f"{key}\t{values[key]}\n" for key in keys)).encode()
                save(data / f"{name}.tsv", raw)
                expected = reference(raw, name)
                observed = rng.sample(keys, 2)
                fresh = rng.sample([key for key in range(256) if key not in observed], 16)
                probes.extend((name, key, expected.get(key)) for key in fresh)
                sources.append({"name": name, "sha256": sha(raw), "observed_keys": observed,
                                "fresh_keys": fresh, "values": expected})
            metadata = {"episode": episode_id, "split": split, "family": 0, "task_count": count,
                        "rows_per_task": width, "cursor": cursor, "sources": sources, "probes": probes}
            save(episode / "inputs.json", canonical(metadata))
            tasks_text = "CNET_ALLOCATOR_TASKS_V1\n"
            masks = []
            for task, source in enumerate(sources):
                name = source["name"]
                output = episode / f"task-{task}"
                output.mkdir(mode=0o700)
                call = bounded([str(binary / "cnet_table_capsule"), "build-worker", str(data), name, str(output),
                                str(os.getpid()), str(PROTOCOL["native_worker_cpu_seconds"]),
                                str(PROTOCOL["native_worker_memory_mib"])], env,
                               PROTOCOL["native_worker_timeout_seconds"])
                save(output / "build.json", canonical(call))
                if call["returncode"] or call["refused"]:
                    raise RuntimeError(f"native_acquisition_failed: {output}")
                candidate = output / "capsule"
                artifacts = {path.name: sha(path.read_bytes()) for path in sorted(candidate.iterdir())}
                if set(artifacts) != {"unit.cnb", "manifest.cknow", "frontend.cvfa"}:
                    raise RuntimeError("candidate_extra_artifacts")
                evidence = runtime.verify(registry, candidate, name, source["values"], probes)
                evidence.update({"input_sha256": sha(canonical(metadata)), "source_sha256": source["sha256"],
                                 "tool_sha256": frozen["tool"], "runtime_sha256": frozen["runtime"],
                                 "evaluator_sha256": frozen["producer"], "build_sha256": sha(canonical(call)),
                                 "candidate_artifacts": artifacts, "incumbent": "empty verified registry",
                                 "verified_tool": "generated numeric fixture; independently parsed source fidelity"})
                receipt = canonical(evidence)
                save(output / "receipt.json", receipt)
                mask = int(evidence["derived_mask"], 16)
                masks.append(mask)
                base = f"{episode_id}\t{task + 1}\t0\t{cursor}\t{len(probes)}\t1\t{width}\t4\t{1 / count:.9g}\t0\t{width / 2048:.9g}\t{width}\t0"
                tasks_text += base + "\n"
                rows[split].append(base + f"\t{mask:x}\t{sha(receipt)}\n")
                summary["tasks"] += 1
                summary["all_key_checks"] += 256
            tasks_file = episode / "tasks.tsv"
            save(tasks_file, tasks_text.encode())
            decision = bounded([str(binary / "allocator_campaign"), str(root), "diagnostic.cell", str(tasks_file)], env, 5)
            save(episode / "decision.json", canonical(decision))
            if decision["returncode"] or decision["refused"]:
                raise RuntimeError("native_choice_failed")
            choices = json.loads(decision["stdout"])["choices"]
            if len(choices) != 4 or any(choice["tasks"] != choices[0]["tasks"] or choice["work"] != width for choice in choices):
                raise RuntimeError("equal_feature_choices_differ")
            coverage = []
            for index, choice in enumerate(choices):
                actual = 0
                for task in choice["tasks"]:
                    actual |= masks[task - 1]
                coverage.append(actual.bit_count() / len(probes))
                summary["coverage_sum"][index] += coverage[-1]
            measurements.append({"episode": episode_id, "split": split, "coverage": coverage,
                                 "oracle_best_single_job": max(mask.bit_count() for mask in masks) / len(probes)})
            summary["different_utility_episodes"] += len({mask.bit_count() for mask in masks}) > 1
            summary["episodes"] += 1
            if summary["episodes"] % 4 == 0:
                print(json.dumps({"event": "campaign_progress", "episodes": summary["episodes"], "tasks": summary["tasks"]}), flush=True)
    for split, lines in rows.items():
        save(root / f"{split}.tsv", ("CNET_ALLOCATOR_ROWS_V1\n" + "".join(lines)).encode())
    if sha((root / "diagnostic.cell").read_bytes()) != checkpoint_sha:
        raise RuntimeError("diagnostic_checkpoint_changed")
    gains, lowers = [], []
    for comparator in range(1, 4):
        paired = [row["coverage"][0] - row["coverage"][comparator] for row in measurements]
        mean = sum(paired) / len(paired)
        variance = sum((value - mean) ** 2 for value in paired) / (len(paired) - 1)
        gains.append(mean)
        lowers.append(mean - 2.040 * math.sqrt(variance / len(paired)))
    save(root / "measurements.json", canonical(measurements))
    summary.update({"kind": "allocator_native_limitation", "root": str(root), "trained": False,
                    "family_counts": [32, 0, 0, 0], "coverage_mean": [value / 32 for value in summary["coverage_sum"]],
                    "paired_gain": gains, "paired95_lower": lowers,
                    "oracle_best_single_job_mean": sum(row["oracle_best_single_job"] for row in measurements) / len(measurements),
                    "minimum_gain": 0.05, "gate_passed": False, "gate_invoked": False,
                    "withheld_reasons": ["identical_feature_choices_zero_gain", "required_families_absent", "confirmation_only_16_episodes", "untrained_diagnostic_model"],
                    "distinct_episodes": 32, "independent_family_evidence": False,
                    "structure_repeated": "one direct-task feature-aliasing design with distinct random tables and probes",
                    "limitation": "counterexample to expressiveness; not impossibility of gain on every workload"})
    save(root / "summary.json", canonical(summary))
    print(json.dumps(summary, sort_keys=True), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", type=Path, required=True)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--chooser", type=Path, required=True)
    run(parser.parse_args())
