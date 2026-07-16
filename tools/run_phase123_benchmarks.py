#!/usr/bin/env python3
"""Bounded Phase 1-3 benchmark authority with fail-closed claim labeling."""

from __future__ import annotations

import argparse
import ctypes
import json
import math
import os
import signal
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


CREATIVE_PROMPTS = (
    (
        "folklore_oath",
        "Write exactly four compact story sentences. Use these exact lowercase phrases naturally: "
        "ancient pact, shadowed halls, oath, betrayal, long after, cost, consequence, "
        "forgotten legend. Make the moral choice genuinely ambiguous. Output only the story.",
    ),
    (
        "generational_echo",
        "Write exactly four compact mythic sentences. Use these exact lowercase phrases naturally: "
        "oath-bearer, silence, remembrance, echo across generations, centuries, duty, honor, myth. "
        "Show a delayed consequence and no explanation outside the story.",
    ),
)


def truthfulqa_claim(
    contract_pass: bool,
    dataset_present: bool,
    runtime_integrated: bool,
    measured_gain: float | None,
) -> dict[str, Any]:
    if measured_gain is not None and not (dataset_present and runtime_integrated):
        raise ValueError("TruthfulQA/FACTOR gain cannot be measured without dataset and runtime integration")
    result: dict[str, Any] = {
        "contract_pass": bool(contract_pass),
        "dataset_present": bool(dataset_present),
        "runtime_integrated": bool(runtime_integrated),
        "target_gain": 0.025,
        "success_metric_claimed": False,
    }
    if not contract_pass:
        result.update(status="contract_fail", measured_gain=measured_gain)
    elif not (dataset_present and runtime_integrated):
        result.update(
            status="withheld",
            measured_gain=None,
            reason="No real FACTOR/TruthfulQA dataset-to-counterfactual-router execution path is present.",
        )
    else:
        assert measured_gain is not None
        result.update(
            status="measured_pass" if measured_gain >= 0.025 else "measured_fail",
            measured_gain=float(measured_gain),
            success_metric_claimed=True,
        )
    return result


def long_context_claim(
    contract_pass: bool,
    dataset_present: bool,
    runtime_integrated: bool,
    budget_measurements: list[dict[str, Any]],
    measured_quality_delta: float | None = None,
) -> dict[str, Any]:
    if measured_quality_delta is not None and not (dataset_present and runtime_integrated):
        raise ValueError("LongBench quality cannot be measured without dataset and runtime integration")
    result: dict[str, Any] = {
        "contract_pass": bool(contract_pass),
        "dataset_present": bool(dataset_present),
        "runtime_integrated": bool(runtime_integrated),
        "target_budget_range": [0.15, 0.25],
        "local_selector_measurements": budget_measurements,
        "quality_metric_claimed": False,
    }
    if not contract_pass:
        result.update(status="contract_fail", measured_quality_delta=measured_quality_delta)
    elif not (dataset_present and runtime_integrated):
        result.update(
            status="withheld",
            measured_quality_delta=None,
            reason="Sparse selection is not integrated into the admitted llama.cpp KV execution path.",
        )
    else:
        assert measured_quality_delta is not None
        result.update(
            status="measured_pass" if measured_quality_delta >= 0.0 else "measured_fail",
            measured_quality_delta=float(measured_quality_delta),
            quality_metric_claimed=True,
        )
    return result


def creative_preservation(
    reference_scores: list[float],
    candidate_scores: list[float],
    tolerance: float = 0.05,
) -> dict[str, Any]:
    if not reference_scores or len(reference_scores) != len(candidate_scores):
        raise ValueError("creative score lists must be non-empty and aligned")
    reference_mean = sum(reference_scores) / len(reference_scores)
    candidate_mean = sum(candidate_scores) / len(candidate_scores)
    delta = candidate_mean - reference_mean
    passed = delta >= -abs(tolerance)
    return {
        "reference_mean": reference_mean,
        "candidate_mean": candidate_mean,
        "delta": delta,
        "tolerance": abs(tolerance),
        "passed": passed,
        "status": "measured_pass" if passed else "measured_fail",
    }


def overall_verdict(
    phase1: dict[str, Any], phase2: dict[str, Any], phase3: dict[str, Any]
) -> str:
    if not all(bool(item.get("contract_pass")) for item in (phase1, phase2, phase3)):
        return "FAIL_NATIVE_CONTRACT"
    if phase3.get("status") != "measured_pass":
        return "FAIL_CREATIVE_PRESERVATION"
    if phase1.get("status") == "withheld" or phase2.get("status") == "withheld":
        return "PASS_WITH_EXTERNAL_CLAIMS_WITHHELD"
    if phase1.get("status") == phase2.get("status") == "measured_pass":
        return "PASS_ALL_MEASURED"
    return "FAIL_BENCHMARK_CLAIM"


class KVBudget(ctypes.Structure):
    _fields_ = [
        ("specialist_id", ctypes.c_int),
        ("max_tokens", ctypes.c_int),
        ("initial_tokens", ctypes.c_int),
        ("recent_tokens", ctypes.c_int),
        ("long_range_stride", ctypes.c_int),
        ("heavy_hitter_fraction", ctypes.c_float),
    ]


class NarrativeScore(ctypes.Structure):
    _fields_ = [
        ("voice_consistency", ctypes.c_float),
        ("moral_ambiguity", ctypes.c_float),
        ("delayed_consequence", ctypes.c_float),
        ("folklore_texture", ctypes.c_float),
        ("narrative_complexity", ctypes.c_float),
        ("overall_score", ctypes.c_float),
        ("is_valid", ctypes.c_bool),
    ]


class NarrativeConfig(ctypes.Structure):
    _fields_ = [
        ("min_voice_consistency", ctypes.c_float),
        ("min_moral_ambiguity", ctypes.c_float),
        ("min_delayed_consequence", ctypes.c_float),
        ("min_folklore_texture", ctypes.c_float),
        ("min_narrative_complexity", ctypes.c_float),
        ("overall_threshold", ctypes.c_float),
    ]


DEFAULT_NARRATIVE_CONFIG = NarrativeConfig(0.65, 0.60, 0.55, 0.50, 0.60, 0.62)


def load_native_library(path: Path) -> ctypes.CDLL:
    library = ctypes.CDLL(str(path.resolve()))
    library.cce_specialist_select_kv_tokens.argtypes = [
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_int,
        ctypes.POINTER(KVBudget),
        ctypes.POINTER(ctypes.c_int),
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_int),
    ]
    library.cce_specialist_select_kv_tokens.restype = ctypes.c_int
    library.cnet_narrative_evaluate.argtypes = [ctypes.c_char_p, ctypes.POINTER(NarrativeConfig)]
    library.cnet_narrative_evaluate.restype = NarrativeScore
    library.cnet_narrative_passes.argtypes = [
        ctypes.POINTER(NarrativeScore),
        ctypes.POINTER(NarrativeConfig),
    ]
    library.cnet_narrative_passes.restype = ctypes.c_bool
    return library


def sparse_selector_measurements(library: ctypes.CDLL) -> list[dict[str, Any]]:
    measurements: list[dict[str, Any]] = []
    for context_tokens in (512, 2048, 8192):
        needles = (context_tokens // 3 + 1, context_tokens // 2 + 3, 3 * context_tokens // 4 + 5)
        scores = [0.0] * context_tokens
        for rank, index in enumerate(needles):
            scores[index] = 100.0 - rank
        score_array = (ctypes.c_float * context_tokens)(*scores)
        for fraction in (0.15, 0.20, 0.25):
            target = int(math.ceil(context_tokens * fraction))
            budget = KVBudget(-1, target, 4, 32, 64, 1.0)
            output = (ctypes.c_int * target)()
            count = ctypes.c_int(0)
            rc = library.cce_specialist_select_kv_tokens(
                score_array,
                context_tokens,
                ctypes.byref(budget),
                output,
                target,
                ctypes.byref(count),
            )
            if rc != 0:
                raise RuntimeError(f"native sparse selector failed with {rc}")
            selected = set(output[: count.value])
            retained = sum(1 for index in needles if index in selected)
            measurements.append(
                {
                    "context_tokens": context_tokens,
                    "budget_fraction": fraction,
                    "target_tokens": target,
                    "selected_tokens": count.value,
                    "realized_fraction": count.value / context_tokens,
                    "needle_retention": retained / len(needles),
                    "budget_exact": count.value == target,
                }
            )
    return measurements


def score_narrative(library: ctypes.CDLL, text: str) -> dict[str, Any]:
    score = library.cnet_narrative_evaluate(
        text.encode("utf-8"), ctypes.byref(DEFAULT_NARRATIVE_CONFIG)
    )
    passed = bool(library.cnet_narrative_passes(ctypes.byref(score), ctypes.byref(DEFAULT_NARRATIVE_CONFIG)))
    return {
        "voice_consistency": float(score.voice_consistency),
        "moral_ambiguity": float(score.moral_ambiguity),
        "delayed_consequence": float(score.delayed_consequence),
        "folklore_texture": float(score.folklore_texture),
        "narrative_complexity": float(score.narrative_complexity),
        "overall_score": float(score.overall_score),
        "valid": bool(score.is_valid),
        "passes_contract": passed,
    }


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def _request_json(url: str, payload: dict[str, Any] | None, timeout: float) -> dict[str, Any]:
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(url, data=data)
    if data is not None:
        request.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def _stop_process(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=10)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=5)


def generate_creative_outputs(
    server: Path,
    model: Path,
    role: str,
    threads: int,
    ctx_size: int,
    max_tokens: int,
    startup_timeout: float,
    request_timeout: float,
) -> dict[str, Any]:
    port = _free_port()
    with tempfile.NamedTemporaryFile(prefix=f"cnet-phase3-{role}-", suffix=".log", delete=False) as log:
        log_path = Path(log.name)
    command = [
        str(server), "--model", str(model), "--alias", f"cnet-phase3-{role}",
        "--host", "127.0.0.1", "--port", str(port), "--n-gpu-layers", "0",
        "--threads", str(threads), "--ctx-size", str(ctx_size), "--parallel", "1",
        "--n-predict", str(max_tokens), "--jinja", "--chat-template-kwargs",
        '{"enable_thinking":false}', "--reasoning-format", "none", "--no-webui",
    ]
    env = os.environ.copy()
    env.update(CUDA_VISIBLE_DEVICES="", HIP_VISIBLE_DEVICES="", ROCR_VISIBLE_DEVICES="")
    started = time.monotonic()
    with log_path.open("wb") as log_handle:
        process = subprocess.Popen(
            command,
            stdout=log_handle,
            stderr=subprocess.STDOUT,
            env=env,
            start_new_session=True,
        )
    try:
        deadline = time.monotonic() + startup_timeout
        while True:
            if process.poll() is not None:
                raise RuntimeError(
                    f"llama-server exited for {role}: {log_path.read_text(errors='replace')[-4000:]}"
                )
            try:
                if _request_json(f"http://127.0.0.1:{port}/health", None, 2.0).get("status") == "ok":
                    break
            except (OSError, urllib.error.URLError, RuntimeError, json.JSONDecodeError):
                pass
            if time.monotonic() >= deadline:
                raise TimeoutError(f"llama-server startup timed out for {role}")
            time.sleep(0.5)
        ready_seconds = time.monotonic() - started
        outputs = []
        for prompt_id, prompt in CREATIVE_PROMPTS:
            query_started = time.monotonic()
            response = _request_json(
                f"http://127.0.0.1:{port}/v1/chat/completions",
                {
                    "messages": [{"role": "user", "content": prompt}],
                    "max_tokens": max_tokens,
                    "temperature": 0,
                    "seed": 42,
                    "chat_template_kwargs": {"enable_thinking": False},
                },
                request_timeout,
            )
            text = str(response["choices"][0]["message"]["content"]).strip()
            outputs.append(
                {
                    "id": prompt_id,
                    "prompt": prompt,
                    "text": text,
                    "query_seconds": time.monotonic() - query_started,
                }
            )
        return {"role": role, "model": str(model), "ready_seconds": ready_seconds, "outputs": outputs}
    finally:
        _stop_process(process)
        log_path.unlink(missing_ok=True)


def run_binary(path: Path) -> dict[str, Any]:
    started = time.monotonic()
    completed = subprocess.run([str(path)], text=True, capture_output=True, timeout=120, check=False)
    return {
        "path": str(path),
        "returncode": completed.returncode,
        "passed": completed.returncode == 0,
        "seconds": time.monotonic() - started,
        "stdout_tail": completed.stdout[-2000:],
        "stderr_tail": completed.stderr[-2000:],
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--native-library", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--ctx-size", type=int, default=2048)
    parser.add_argument("--max-tokens", type=int, default=96)
    parser.add_argument("--startup-timeout", type=float, default=180.0)
    parser.add_argument("--request-timeout", type=float, default=240.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    for path in (args.reference, args.candidate, args.server, args.native_library):
        if not path.exists():
            raise FileNotFoundError(path)
    contract_runs = {
        "phase1": run_binary(Path("bin/counterfactual_router_test")),
        "phase2": run_binary(Path("bin/sparse_kv_test")),
        "phase3": run_binary(Path("bin/narrative_coherence_test")),
    }
    library = load_native_library(args.native_library)
    selector_measurements = sparse_selector_measurements(library)
    phase1 = truthfulqa_claim(contract_runs["phase1"]["passed"], False, False, None)
    phase2 = long_context_claim(
        contract_runs["phase2"]["passed"], False, False, selector_measurements
    )
    reference = generate_creative_outputs(
        args.server, args.reference, "reference", args.threads, args.ctx_size,
        args.max_tokens, args.startup_timeout, args.request_timeout,
    )
    candidate = generate_creative_outputs(
        args.server, args.candidate, "candidate", args.threads, args.ctx_size,
        args.max_tokens, args.startup_timeout, args.request_timeout,
    )
    for run in (reference, candidate):
        for item in run["outputs"]:
            item["native_score"] = score_narrative(library, item["text"])
    reference_scores = [item["native_score"]["overall_score"] for item in reference["outputs"]]
    candidate_scores = [item["native_score"]["overall_score"] for item in candidate["outputs"]]
    preservation = creative_preservation(reference_scores, candidate_scores)
    phase3 = {
        "contract_pass": contract_runs["phase3"]["passed"],
        **preservation,
        "reference": reference,
        "candidate": candidate,
        "human_judge": "not_run",
        "llm_as_judge": "not_run",
        "claim_scope": "native lexical rubric preservation on real deterministic model outputs",
    }
    verdict = overall_verdict(phase1, phase2, phase3)
    report = {
        "schema_version": 1,
        "cpu_only": True,
        "claim_integrity": {
            "synthetic_is_not_external": True,
            "withheld_claims_are_not_failures": True,
            "phase1_target_gain": 0.025,
            "phase2_target_budget_range": [0.15, 0.25],
            "phase3_preservation_tolerance": 0.05,
        },
        "contract_runs": contract_runs,
        "phase1": phase1,
        "phase2": phase2,
        "phase3": phase3,
        "verdict": verdict,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"verdict": verdict, "report": str(args.report)}))
    return 0 if verdict.startswith("PASS_") else 1


if __name__ == "__main__":
    raise SystemExit(main())
