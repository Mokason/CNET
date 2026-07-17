#!/usr/bin/env python3
"""Real-model bounded GPU-offload benchmark and telemetry gate.

Runs identical deterministic CNET harness work on CPU and selected ROCm tiers,
samples discrete-GPU sysfs power/VRAM, verifies output equivalence and cleanup,
and rejects dual-GPU layer split unless it materially beats one GPU.
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

MIB = 1024 * 1024
GIB = 1024 * MIB


@dataclass(frozen=True)
class Gpu:
    logical_index: int
    card: str
    pci_slot: str
    device_dir: Path
    hwmon_dir: Path | None

    def read_int(self, name: str) -> int | None:
        path = self.device_dir / name
        try:
            return int(path.read_text().strip())
        except (OSError, ValueError):
            return None

    def power_uw(self) -> int | None:
        if self.hwmon_dir is None:
            return None
        for name in ("power1_average", "power1_input"):
            try:
                return int((self.hwmon_dir / name).read_text().strip())
            except (OSError, ValueError):
                continue
        return None


def discover_discrete_gpus() -> list[Gpu]:
    found: list[tuple[str, Path, Path | None]] = []
    for card in sorted(Path("/sys/class/drm").glob("card[0-9]*")):
        device = card / "device"
        try:
            total = int((device / "mem_info_vram_total").read_text().strip())
            uevent = (device / "uevent").read_text()
        except (FileNotFoundError, PermissionError, ValueError):
            continue
        if total < 16 * GIB:
            continue
        slot = next(
            (line.split("=", 1)[1] for line in uevent.splitlines()
             if line.startswith("PCI_SLOT_NAME=")),
            card.name,
        )
        hwmons = sorted((device / "hwmon").glob("hwmon*"))
        found.append((slot, device, hwmons[0] if hwmons else None))
    found.sort(key=lambda item: item[0])
    return [Gpu(i, d.parent.name, slot, d, h)
            for i, (slot, d, h) in enumerate(found)]


def snapshot(gpus: list[Gpu]) -> dict[str, dict[str, int | None]]:
    return {
        str(g.logical_index): {
            "vramBytes": g.read_int("mem_info_vram_used"),
            "powerUw": g.power_uw(),
            "busyPercent": g.read_int("gpu_busy_percent"),
        }
        for g in gpus
    }


def median_numeric(runs: list[dict[str, Any]], key: str) -> float:
    return float(statistics.median(float(run["harness"][key]) for run in runs))


def parse_harness_json(stdout: str, stderr: str) -> dict[str, Any]:
    for text in (stdout, stderr):
        for line in reversed(text.splitlines()):
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict) and "status" in value:
                return value
    raise RuntimeError("harness did not emit JSON evidence")


def run_case(
    *,
    label: str,
    model: Path,
    dotnet: Path,
    smoke_dll: Path,
    worktree: Path,
    llama_bin: Path,
    gpus: list[Gpu],
    layers: int,
    devices: list[int],
    cap_mib: int,
    timeout_s: float,
) -> dict[str, Any]:
    env = os.environ.copy()
    ld_parts = [str(worktree / "bin"), str(llama_bin)]
    if env.get("LD_LIBRARY_PATH"):
        ld_parts.append(env["LD_LIBRARY_PATH"])
    env["LD_LIBRARY_PATH"] = ":".join(ld_parts)
    env["GGML_BACKEND_DIR"] = str(llama_bin)
    if layers == 0:
        env["ROCR_VISIBLE_DEVICES"] = ""
        env["HIP_VISIBLE_DEVICES"] = ""
        env["CUDA_VISIBLE_DEVICES"] = ""
        command = [str(dotnet), str(smoke_dll), str(model), "--gpu-baseline"]
    else:
        visible = ",".join(str(i) for i in devices)
        env["ROCR_VISIBLE_DEVICES"] = visible
        env["HIP_VISIBLE_DEVICES"] = visible
        env["CUDA_VISIBLE_DEVICES"] = ""
        # Device indices are logical after the visibility filter.
        policy_devices = ",".join(str(i) for i in range(len(devices)))
        command = [
            str(dotnet), str(smoke_dll), str(model), "--gpu-offload",
            str(layers), policy_devices, str(cap_mib),
        ]

    baseline_samples = []
    for _ in range(10):
        baseline_samples.append(snapshot(gpus))
        time.sleep(0.1)
    baseline = baseline_samples[-1]

    started = time.monotonic()
    proc = subprocess.Popen(
        command,
        cwd=worktree,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    samples: list[dict[str, dict[str, int | None]]] = []
    while proc.poll() is None:
        if time.monotonic() - started > timeout_s:
            proc.kill()
            stdout, stderr = proc.communicate()
            raise RuntimeError(
                f"{label} timed out after {timeout_s}s\n{stdout}\n{stderr}")
        samples.append(snapshot(gpus))
        time.sleep(0.1)
    stdout, stderr = proc.communicate()
    wall_ms = (time.monotonic() - started) * 1000.0
    if proc.returncode != 0:
        raise RuntimeError(
            f"{label} exited {proc.returncode}\nstdout:\n{stdout}\nstderr:\n{stderr}")
    harness = parse_harness_json(stdout, stderr)

    post_samples = []
    for _ in range(50):
        post_samples.append(snapshot(gpus))
        time.sleep(0.1)
    post = post_samples[-1]

    peak: dict[str, dict[str, int | None]] = {}
    for gpu in gpus:
        idx = str(gpu.logical_index)
        values = [sample[idx] for sample in samples]
        peak[idx] = {
            "vramBytes": max(
                (v["vramBytes"] for v in values if v["vramBytes"] is not None),
                default=None,
            ),
            "powerUw": max(
                (v["powerUw"] for v in values if v["powerUw"] is not None),
                default=None,
            ),
            "busyPercent": max(
                (v["busyPercent"] for v in values if v["busyPercent"] is not None),
                default=None,
            ),
        }

    cleanup_ok = True
    for gpu in gpus:
        idx = str(gpu.logical_index)
        before = baseline[idx]["vramBytes"]
        after = post[idx]["vramBytes"]
        if before is not None and after is not None and after > before + 64 * MIB:
            cleanup_ok = False

    selected_power_ok = True
    for idx in devices:
        power = peak.get(str(idx), {}).get("powerUw")
        if power is not None and power > 150_000_000:
            selected_power_ok = False

    return {
        "label": label,
        "layers": layers,
        "physicalDevices": devices,
        "command": command,
        "wallMs": wall_ms,
        "harness": harness,
        "baseline": baseline,
        "peak": peak,
        "post": post,
        "sampleCount": len(samples),
        "cleanupOk": cleanup_ok,
        "selectedPowerOk": selected_power_ok,
        "stdout": stdout,
        "stderr": stderr,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--worktree", type=Path, default=Path.cwd())
    parser.add_argument("--dotnet", type=Path, default=Path("/home/marble/dotnet/dotnet"))
    parser.add_argument("--llama-bin", type=Path,
                        default=Path("/home/marble/llama.cpp/build-rocm/bin"))
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--cap-mib", type=int, default=2048)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--output", type=Path,
                        default=Path("logs/cnet_harness_gpu_benchmark.json"))
    args = parser.parse_args()

    worktree = args.worktree.resolve()
    model = args.model.resolve()
    smoke = (worktree / "dotnet/CnetHarnessSmoke/bin/Release/net10.0/"
             "CnetHarnessSmoke.dll")
    for required in (model, args.dotnet, smoke, args.llama_bin):
        if not required.exists():
            raise FileNotFoundError(required)
    if args.repeats < 1:
        raise ValueError("repeats must be positive")

    gpus = discover_discrete_gpus()
    if len(gpus) < 2:
        raise RuntimeError(f"expected two discrete GPUs, found {len(gpus)}")

    case_specs = [
        ("cpu", 0, []),
        ("gpu4", 4, [0]),
        ("gpu8", 8, [0]),
        ("gpu12", 12, [0]),
        ("gpu16", 16, [0]),
        ("dual12", 12, [0, 1]),
    ]
    all_runs: dict[str, list[dict[str, Any]]] = {}
    try:
        for label, layers, devices in case_specs:
            runs = []
            for repeat in range(args.repeats):
                run = run_case(
                    label=f"{label}-{repeat + 1}",
                    model=model,
                    dotnet=args.dotnet,
                    smoke_dll=smoke,
                    worktree=worktree,
                    llama_bin=args.llama_bin,
                    gpus=gpus,
                    layers=layers,
                    devices=devices,
                    cap_mib=args.cap_mib,
                    timeout_s=args.timeout,
                )
                runs.append(run)
            all_runs[label] = runs
    except Exception as exc:
        partial = {
            "status": "CNET_HARNESS_GPU_BENCHMARK_FAIL",
            "error": str(exc),
            "runs": all_runs,
        }
        output = (worktree / args.output).resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(partial, indent=2) + "\n")
        print(json.dumps({"status": partial["status"], "error": str(exc)}),
              file=sys.stderr)
        return 1

    cpu_gen = median_numeric(all_runs["cpu"], "GenerationMs")
    cpu_prompt = median_numeric(all_runs["cpu"], "PromptMs")
    cpu_text = all_runs["cpu"][0]["harness"]["Text"]
    single_labels = ["gpu4", "gpu8", "gpu12", "gpu16"]
    medians: dict[str, dict[str, float]] = {}
    for label in case_specs:
        name = label[0]
        medians[name] = {
            "promptMs": median_numeric(all_runs[name], "PromptMs"),
            "generationMs": median_numeric(all_runs[name], "GenerationMs"),
        }
        medians[name]["promptSpeedup"] = (
            cpu_prompt / medians[name]["promptMs"]
            if medians[name]["promptMs"] > 0 else 0.0)
        medians[name]["generationSpeedup"] = (
            cpu_gen / medians[name]["generationMs"]
            if medians[name]["generationMs"] > 0 else 0.0)

    best_single = min(single_labels, key=lambda name: medians[name]["generationMs"])
    best_speedup = medians[best_single]["generationSpeedup"]
    dual_accepted = (
        medians["dual12"]["generationMs"]
        <= medians[best_single]["generationMs"] * 0.90)

    output_equivalent = all(
        run["harness"]["Text"] == cpu_text
        for name, runs in all_runs.items() if name != "cpu"
        for run in runs
    )
    telemetry_ok = all(
        run["cleanupOk"] and run["selectedPowerOk"]
        for runs in all_runs.values() for run in runs
    )
    offload_contract_ok = all(
        run["harness"].get("partial") is True
        and run["harness"].get("residencyBounded") is True
        for name, runs in all_runs.items() if name != "cpu"
        for run in runs
    )
    speedup_ok = best_speedup >= 1.10
    passed = output_equivalent and telemetry_ok and offload_contract_ok and speedup_ok

    evidence = {
        "status": ("CNET_HARNESS_GPU_BENCHMARK_PASS" if passed
                   else "CNET_HARNESS_GPU_BENCHMARK_FAIL"),
        "model": str(model),
        "repeats": args.repeats,
        "gpus": [
            {"logicalIndex": g.logical_index, "card": g.card,
             "pciSlot": g.pci_slot, "hwmon": str(g.hwmon_dir)}
            for g in gpus
        ],
        "medians": medians,
        "bestSingleGpuTier": best_single,
        "bestSingleGpuGenerationSpeedup": best_speedup,
        "dualGpuAccepted": dual_accepted,
        "dualGpuDecision": ("accept" if dual_accepted else
                            "reject: PCIe split did not beat best single GPU by 10%"),
        "outputEquivalent": output_equivalent,
        "telemetryOk": telemetry_ok,
        "offloadContractOk": offload_contract_ok,
        "speedupOk": speedup_ok,
        "runs": all_runs,
    }
    output = (worktree / args.output).resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(evidence, indent=2) + "\n")
    compact = {key: evidence[key] for key in (
        "status", "bestSingleGpuTier", "bestSingleGpuGenerationSpeedup",
        "dualGpuAccepted", "dualGpuDecision", "outputEquivalent",
        "telemetryOk", "offloadContractOk", "speedupOk")}
    compact["output"] = str(output)
    print(json.dumps(compact))
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
