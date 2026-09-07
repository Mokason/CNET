#!/usr/bin/env python3
"""Opt-in bounded synthetic scale measurements against the existing native core."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time


REPOSITORY = Path(__file__).resolve().parents[1]
GIB = 1024 ** 3
DISK_LIMIT = 2 * GIB
AS_LIMIT = 16 * GIB
WALL_SECONDS = 300
ENVIRONMENT = {"PATH": "/usr/bin:/bin", "LC_ALL": "C", "OMP_NUM_THREADS": "1"}
TERMINATE_REQUESTED = False


def sha256(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def available_memory():
    for line in Path("/proc/meminfo").read_text().splitlines():
        if line.startswith("MemAvailable:"):
            return int(line.split()[1]) * 1024
    raise RuntimeError("MemAvailable unavailable")


def disk_bytes(root):
    total = 0
    for directory, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            entry = Path(directory) / name
            info = entry.lstat()
            if stat.S_ISLNK(info.st_mode) or not (stat.S_ISREG(info.st_mode) or stat.S_ISDIR(info.st_mode)):
                raise RuntimeError("nonregular artifact: " + str(entry))
            if stat.S_ISREG(info.st_mode):
                total += info.st_size
    return total


def child_limits():
    resource.setrlimit(resource.RLIMIT_AS, (AS_LIMIT, AS_LIMIT))
    resource.setrlimit(resource.RLIMIT_CPU, (WALL_SECONDS, WALL_SECONDS))
    resource.setrlimit(resource.RLIMIT_FSIZE, (64 * 1024 ** 2, 64 * 1024 ** 2))
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def child_written_bytes(process):
    # Linux process accounting bounds writes without repeatedly walking the
    # fixture during a latency sample. Fixed benchmark children do not fork.
    try:
        for line in Path("/proc/" + str(process.pid) + "/io").read_text().splitlines():
            if line.startswith("wchar:"):
                return int(line.split()[1])
    except OSError as error:
        if process.poll() is not None:
            return 0  # Exact child completed; post-exit disk/status checks remain.
        raise RuntimeError("child write accounting unavailable") from error
    raise RuntimeError("child write accounting unavailable")


def run_child(command, artifact, label, units):
    if TERMINATE_REQUESTED:
        return None, "runner_terminated_by_SIGTERM"
    # This fixed five-row fixture is small; reserve 256 KiB per unit for each
    # overlapping generation plus 2 GiB host headroom before starting work.
    required = 2 * GIB + units * 2 * 256 * 1024
    if available_memory() < required:
        return None, "memory_preflight"
    initial_disk = disk_bytes(artifact)
    if initial_disk >= DISK_LIMIT:
        return None, "disk_preflight"
    start = time.monotonic()
    with (artifact / (label + ".stdout")).open("xb") as out, (artifact / (label + ".stderr")).open("xb") as err:
        process = None
        reason = None
        try:
            if TERMINATE_REQUESTED:
                return None, "runner_terminated_by_SIGTERM"
            process = subprocess.Popen(command, cwd=REPOSITORY, env=ENVIRONMENT,
                                       stdin=subprocess.DEVNULL, stdout=out, stderr=err,
                                       start_new_session=True, preexec_fn=child_limits)
            while process.poll() is None:
                if TERMINATE_REQUESTED:
                    reason = "runner_terminated_by_SIGTERM"
                elif time.monotonic() - start >= WALL_SECONDS:
                    reason = "wall_timeout"
                elif available_memory() < 2 * GIB:
                    reason = "memory_pressure"
                elif initial_disk + child_written_bytes(process) >= DISK_LIMIT - 64 * 1024 ** 2:
                    reason = "disk_pressure"
                if reason:
                    break
                time.sleep(.1)
        finally:
            # Cancellation or a failed monitor must not leave our child alive.
            if process is not None and process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            if process is not None:
                code = process.wait()
    if TERMINATE_REQUESTED:
        return None, "runner_terminated_by_SIGTERM"
    if reason:
        return None, reason
    if disk_bytes(artifact) > DISK_LIMIT:
        return None, "disk_budget"
    if code:
        return None, "child_exit_" + str(code)
    return (artifact / (label + ".stdout")).read_text(), None


def main():
    global TERMINATE_REQUESTED
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--counts", nargs="+", type=int, default=[32, 256, 1024, 4096])
    parser.add_argument("--replicates", type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.replicates <= 3 or any(not 2 <= n <= 4096 for n in args.counts) or args.counts != sorted(set(args.counts)):
        parser.error("counts must strictly increase within 2..4096; replicates must be 1..3")
    os.umask(0o077)
    artifact = Path(tempfile.mkdtemp(prefix="cnet-capsule-scale-"))
    print("CAPSULE_SCALE_ARTIFACTS " + str(artifact), file=sys.stderr, flush=True)
    results = (artifact / "results.jsonl").open("x")
    previous_termination = TERMINATE_REQUESTED
    TERMINATE_REQUESTED = False

    def terminate(_signum, _frame):
        # Never throw asynchronously: even the first TERM can arrive during
        # kill/wait cleanup after an unrelated failure. Repeated TERM is safe.
        global TERMINATE_REQUESTED
        TERMINATE_REQUESTED = True

    previous_sigterm = signal.signal(signal.SIGTERM, terminate)

    def emit(record):
        if TERMINATE_REQUESTED and record.get("event") != "failure":
            raise RuntimeError("runner_terminated_by_SIGTERM")
        record.update(schema_version=1, artifact_root=str(artifact))
        line = json.dumps(record, separators=(",", ":"), allow_nan=False)
        results.write(line + "\n"); results.flush()
        print(line, flush=True)

    def failure(phase, reason, count=0, replicate=0):
        emit(dict(event="failure", status="fail", phase=phase, reason=reason,
                  capsules=count, replicate=replicate))
        return 1

    try:
        if available_memory() < 4 * GIB:
            return failure("preflight", "memory_preflight")
        source = REPOSITORY / "tests/capsule_scale_bench.c"
        library = REPOSITORY / "bin/libcnet_capsule_core.so"
        if not library.is_file():
            return failure("preflight", "build_existing_library_with_make_bin/libcnet_capsule_core.so")
        pinned = artifact / library.name
        original_hash = sha256(library)
        shutil.copyfile(library, pinned)
        if sha256(library) != original_hash or sha256(pinned) != original_hash:
            return failure("preflight", "library_changed_during_copy")
        executable = artifact / "capsule_scale_bench"
        command = ["/usr/bin/cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                   "-D_DEFAULT_SOURCE", "-D_GNU_SOURCE", "-include", "include/cnet_platform.h",
                   "-Iinclude", str(source), "-L" + str(artifact), "-lcnet_capsule_core",
                   "-Wl,-rpath," + str(artifact), "-pthread", "-lm", "-o", str(executable)]
        _, error = run_child(command, artifact, "compile", 0)
        if error:
            return failure("compile", error)
        emit(dict(event="configuration", counts=args.counts, replicates=args.replicates,
                  scope="synthetic_distinct_five_row_numeric_functions_not_acquired_knowledge",
                  quantile="nearest_rank_ceil_percent_times_n", fresh_process=True, cold_filesystem_cache=False,
                  wall_seconds=WALL_SECONDS, address_space_bytes=AS_LIMIT, disk_budget_bytes=DISK_LIMIT,
                  rss_method="smaps_rollup_Rss_checkpoints_with_separate_approximate_VmHWM",
                  mem_available_bytes=available_memory(), uname=list(os.uname()),
                  cpu_affinity=sorted(os.sched_getaffinity(0)), compiler_command=command,
                  sha256={str(source.relative_to(REPOSITORY)): sha256(source),
                          "scripts/capsule_scale_bench.py": sha256(Path(__file__)),
                          library.name: original_hash, executable.name: sha256(executable)}))
        _, error = run_child([str(executable), "selftest"], artifact, "quantiles", 0)
        if error:
            return failure("quantiles", error)
        for count in args.counts:
            fixture = artifact / ("fixture-" + str(count)); fixture.mkdir(mode=0o700)
            raw, error = run_child([str(executable), "build", str(fixture), str(count)], artifact, "build-" + str(count), count)
            if error:
                return failure("fixture", error, count)
            fixture_record = json.loads(raw)
            if fixture_record.get("status") != "pass" or fixture_record.get("capsules") != count:
                return failure("fixture", "invalid_fixture_receipt", count)
            serialized = disk_bytes(fixture / "after")
            emit(dict(fixture_record, serialized_bytes=serialized))
            for replicate in range(1, args.replicates + 1):
                label = "measure-" + str(count) + "-" + str(replicate)
                raw, error = run_child([str(executable), "measure", str(fixture), str(count)], artifact, label, count)
                if error:
                    return failure("measurement", error, count, replicate)
                record = json.loads(raw)
                if record.get("status") != "pass" or record.get("capsules") != count:
                    return failure("measurement", "invalid_measurement_receipt", count, replicate)
                record.update(replicate=replicate, serialized_bytes=serialized,
                              serialized_before_bytes=disk_bytes(fixture / "before"),
                              rss_overlap_to_serialized=record["rss_overlap_bytes"] / serialized,
                              rss_overlap_delta_to_serialized=(record["rss_overlap_bytes"] - record["rss_baseline_bytes"]) / serialized)
                emit(record)
        emit(dict(event="complete", status="pass", artifact_bytes=disk_bytes(artifact)))
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        return failure("runner", type(error).__name__ + ":" + str(error))
    finally:
        results.close()
        signal.signal(signal.SIGTERM, previous_sigterm)
        TERMINATE_REQUESTED = previous_termination


if __name__ == "__main__":
    sys.exit(main())
