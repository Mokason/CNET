#!/usr/bin/env python3
"""Bounded CPU-only acceptance campaign for real CNET GGUF/QGKP artifacts."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import math
import os
import signal
import socket
import struct
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

PROBES = (
    ("instruction_exact", "/no_think\nReply with exactly READY.", "READY"),
    ("arithmetic_exact", "/no_think\nWhat is 17+25? Reply with digits only.", "42"),
    ("constraint_exact", "/no_think\nReply with the word BLUE and nothing else.", "BLUE"),
)


def require_magic(path: Path, expected: bytes) -> None:
    if not path.is_file():
        raise ValueError(f"file not found: {path}")
    with path.open("rb") as handle:
        actual = handle.read(len(expected))
    if actual != expected:
        raise ValueError(f"magic mismatch for {path}: expected {expected!r}, got {actual!r}")


def sha256_file(path: Path, chunk_size: int = 8 * 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(chunk_size)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


_GGUF_SCALAR_FORMATS = {
    0: "B",  # uint8
    1: "b",  # int8
    2: "H",  # uint16
    3: "h",  # int16
    4: "I",  # uint32
    5: "i",  # int32
    6: "f",  # float32
    7: "?",  # bool
    10: "Q",  # uint64
    11: "q",  # int64
    12: "d",  # float64
}
_GGML_TYPE_NAMES = {
    0: "F32",
    1: "F16",
    2: "Q4_0",
    3: "Q4_1",
    6: "Q5_0",
    7: "Q5_1",
    8: "Q8_0",
    9: "Q8_1",
    10: "Q2_K",
    11: "Q3_K",
    12: "Q4_K",
    13: "Q5_K",
    14: "Q6_K",
    15: "Q8_K",
    16: "IQ2_XXS",
    17: "IQ2_XS",
    18: "IQ3_XXS",
    19: "IQ1_S",
    20: "IQ4_NL",
    21: "IQ3_S",
    22: "IQ2_S",
    23: "IQ4_XS",
    24: "I8",
    25: "I16",
    26: "I32",
    27: "I64",
    28: "F64",
    29: "IQ1_M",
    30: "BF16",
    34: "TQ1_0",
    35: "TQ2_0",
}


def _read_exact(handle: Any, count: int) -> bytes:
    data = handle.read(count)
    if len(data) != count:
        raise ValueError("truncated GGUF metadata")
    return data


def _read_number(handle: Any, format_code: str) -> Any:
    size = struct.calcsize("<" + format_code)
    return struct.unpack("<" + format_code, _read_exact(handle, size))[0]


def _read_gguf_string(handle: Any) -> str:
    count = int(_read_number(handle, "Q"))
    if count > 100_000_000:
        raise ValueError(f"unreasonable GGUF string length: {count}")
    return _read_exact(handle, count).decode("utf-8", errors="replace")


def _read_or_skip_gguf_value(handle: Any, value_type: int, keep: bool) -> Any:
    if value_type == 8:  # string
        value = _read_gguf_string(handle)
        return value if keep else None
    if value_type == 9:  # array
        element_type = int(_read_number(handle, "I"))
        count = int(_read_number(handle, "Q"))
        if count > 10_000_000:
            raise ValueError(f"unreasonable GGUF array length: {count}")
        if element_type == 8:
            values = [] if keep and count < 100 else None
            for _ in range(count):
                value = _read_gguf_string(handle)
                if values is not None:
                    values.append(value)
            return values
        format_code = _GGUF_SCALAR_FORMATS.get(element_type)
        if format_code is None:
            raise ValueError(f"unsupported GGUF array element type: {element_type}")
        size = struct.calcsize("<" + format_code) * count
        raw = _read_exact(handle, size)
        if keep and count < 100:
            return list(struct.unpack("<" + format_code * count, raw))
        return None
    format_code = _GGUF_SCALAR_FORMATS.get(value_type)
    if format_code is None:
        raise ValueError(f"unsupported GGUF value type: {value_type}")
    value = _read_number(handle, format_code)
    return value if keep else None


def inspect_gguf(path: Path) -> dict[str, Any]:
    """Read GGUF metadata/tensor headers without touching multi-gigabyte payloads."""
    with path.open("rb") as handle:
        if _read_exact(handle, 4) != b"GGUF":
            raise ValueError(f"magic mismatch for {path}: expected b'GGUF'")
        version = int(_read_number(handle, "I"))
        tensor_count = int(_read_number(handle, "Q"))
        metadata_count = int(_read_number(handle, "Q"))
        if version not in (2, 3):
            raise ValueError(f"unsupported GGUF version: {version}")
        if tensor_count > 10_000_000 or metadata_count > 1_000_000:
            raise ValueError("unreasonable GGUF header counts")

        selected: dict[str, Any] = {}
        selected_keys = {
            "general.architecture",
            "general.file_type",
            "general.name",
            "general.quantization_version",
        }
        for _ in range(metadata_count):
            key = _read_gguf_string(handle)
            value_type = int(_read_number(handle, "I"))
            value = _read_or_skip_gguf_value(handle, value_type, key in selected_keys)
            if key in selected_keys:
                selected[key] = value

        tensor_types: dict[str, int] = {}
        for _ in range(tensor_count):
            _read_gguf_string(handle)
            dimensions = int(_read_number(handle, "I"))
            if dimensions > 8:
                raise ValueError(f"unreasonable GGUF tensor rank: {dimensions}")
            _read_exact(handle, dimensions * 8)
            tensor_type = int(_read_number(handle, "I"))
            _read_exact(handle, 8)  # payload offset
            name = _GGML_TYPE_NAMES.get(tensor_type, f"TYPE_{tensor_type}")
            tensor_types[name] = tensor_types.get(name, 0) + 1

    return {
        "version": version,
        "architecture": selected.get("general.architecture"),
        "name": selected.get("general.name"),
        "file_type": selected.get("general.file_type"),
        "quantization_version": selected.get("general.quantization_version"),
        "tensor_count": tensor_count,
        "metadata_count": metadata_count,
        "tensor_types": dict(sorted(tensor_types.items())),
    }


def structural_mismatch_reasons(
    reference: dict[str, Any], candidate: dict[str, Any]
) -> list[str]:
    reasons: list[str] = []
    if reference.get("architecture") != candidate.get("architecture"):
        reasons.append("architecture_mismatch")
    if int(reference.get("tensor_count", -1)) != int(candidate.get("tensor_count", -2)):
        reasons.append("tensor_count_mismatch")
    return reasons


def quantization_diagnosis(
    reference: dict[str, Any], candidate: dict[str, Any]
) -> dict[str, Any]:
    def ternary_fraction(inspection: dict[str, Any]) -> float:
        types = inspection.get("tensor_types", {})
        ternary = int(types.get("TQ1_0", 0)) + int(types.get("TQ2_0", 0))
        count = int(inspection.get("tensor_count", 0))
        return ternary / count if count else 0.0

    reference_fraction = ternary_fraction(reference)
    candidate_fraction = ternary_fraction(candidate)
    return {
        "reference_ternary_fraction": reference_fraction,
        "candidate_ternary_fraction": candidate_fraction,
        "aggressive_ternarization": (
            candidate_fraction > 0.5 and candidate_fraction > reference_fraction + 0.25
        ),
        "source": "embedded_gguf_tensor_types",
    }


def _probability_map(top_logprobs: list[dict[str, Any]]) -> dict[int, float]:
    values = {int(item["id"]): math.exp(float(item["logprob"])) for item in top_logprobs}
    total = sum(values.values())
    if total <= 0.0:
        return {token_id: 0.0 for token_id in values}
    return {token_id: value / total for token_id, value in values.items()}


def normalized_entropy(top_logprobs: list[dict[str, Any]]) -> float:
    probabilities = list(_probability_map(top_logprobs).values())
    if len(probabilities) <= 1:
        return 0.0
    entropy = -sum(value * math.log(value) for value in probabilities if value > 0.0)
    return entropy / math.log(len(probabilities))


def _aligned_distributions(
    reference: list[dict[str, Any]], candidate: list[dict[str, Any]]
) -> tuple[list[float], list[float]]:
    ref_map = _probability_map(reference)
    cand_map = _probability_map(candidate)
    token_ids = sorted(set(ref_map) | set(cand_map))
    return (
        [ref_map.get(token_id, 0.0) for token_id in token_ids],
        [cand_map.get(token_id, 0.0) for token_id in token_ids],
    )


def _l2(values: list[float]) -> float:
    return math.sqrt(sum(value * value for value in values))


def distribution_metrics(
    reference: list[dict[str, Any]], candidate: list[dict[str, Any]]
) -> dict[str, Any]:
    ref_values, cand_values = _aligned_distributions(reference, candidate)
    residual = [ref - cand for ref, cand in zip(ref_values, cand_values)]
    curve = []
    for alpha in (0.0, 0.25, 0.5, 0.75, 1.0):
        recovered = [cand + alpha * delta for cand, delta in zip(cand_values, residual)]
        curve.append(
            {
                "alpha": alpha,
                "residual_l2_norm": _l2([ref - value for ref, value in zip(ref_values, recovered)]),
            }
        )
    return {
        "identity_l2_norm": _l2(ref_values),
        "residual_l2_norm": _l2(residual),
        "recovery_curve": curve,
    }


def admit_candidate(
    reference: dict[str, Any],
    candidate: dict[str, Any],
    max_quality_delta: float,
    structural_reasons: list[str] | None = None,
) -> dict[str, Any]:
    delta = float(candidate["quality_pass_fraction"]) - float(reference["quality_pass_fraction"])
    reasons = list(structural_reasons or [])
    if delta < -abs(max_quality_delta):
        reasons.append("quality_regression")
    reasons = list(dict.fromkeys(reasons))
    admitted = not reasons
    return {
        "admitted": admitted,
        "selected_role": "candidate" if admitted else "reference",
        "quality_delta": delta,
        "reasons": reasons,
    }


class _CompressionGrads(ctypes.Structure):
    _fields_ = [
        ("values", ctypes.POINTER(ctypes.c_float)),
        ("count", ctypes.c_size_t),
        ("preserve_identity_path", ctypes.c_int),
        ("preserve_residual_path", ctypes.c_int),
    ]


def cnet_grad_norms(
    library_path: Path,
    reference: list[dict[str, Any]],
    candidate: list[dict[str, Any]],
) -> dict[str, float]:
    ref_values, cand_values = _aligned_distributions(reference, candidate)
    count = len(ref_values)
    if count == 0:
        return {"identity_l2_norm": 0.0, "residual_l2_norm": 0.0}
    library = ctypes.CDLL(str(library_path.resolve()))
    library.cce_compression_grads_init.argtypes = [ctypes.POINTER(_CompressionGrads), ctypes.c_size_t]
    library.cce_compression_grads_init.restype = ctypes.c_int
    library.cce_compression_grads_accumulate.argtypes = [
        ctypes.POINTER(_CompressionGrads),
        ctypes.POINTER(ctypes.c_float),
        ctypes.POINTER(ctypes.c_float),
        ctypes.c_size_t,
        ctypes.c_float,
        ctypes.c_float,
    ]
    library.cce_compression_grads_accumulate.restype = ctypes.c_int
    library.cce_compression_grads_l2_norm.argtypes = [ctypes.POINTER(_CompressionGrads)]
    library.cce_compression_grads_l2_norm.restype = ctypes.c_float
    library.cce_compression_grads_free.argtypes = [ctypes.POINTER(_CompressionGrads)]

    zeros = (ctypes.c_float * count)(*([0.0] * count))
    identity = (ctypes.c_float * count)(*ref_values)
    residual = (ctypes.c_float * count)(*[ref - cand for ref, cand in zip(ref_values, cand_values)])

    def norm(identity_values: Any, residual_values: Any) -> float:
        grads = _CompressionGrads()
        if library.cce_compression_grads_init(ctypes.byref(grads), count) != 0:
            raise RuntimeError("cce_compression_grads_init failed")
        try:
            if library.cce_compression_grads_accumulate(
                ctypes.byref(grads), identity_values, residual_values, count, 1.0, 1.0
            ) != 0:
                raise RuntimeError("cce_compression_grads_accumulate failed")
            return float(library.cce_compression_grads_l2_norm(ctypes.byref(grads)))
        finally:
            library.cce_compression_grads_free(ctypes.byref(grads))

    return {
        "identity_l2_norm": norm(identity, zeros),
        "residual_l2_norm": norm(zeros, residual),
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
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {exc.code} from {url}: {body}") from exc


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


def probe_model(
    server: Path,
    model: Path,
    role: str,
    startup_timeout: float,
    request_timeout: float,
    threads: int,
    ctx_size: int,
) -> dict[str, Any]:
    port = _free_port()
    with tempfile.NamedTemporaryFile(prefix=f"cnet-{role}-", suffix=".log", delete=False) as log:
        log_path = Path(log.name)
    command = [
        str(server),
        "--model",
        str(model),
        "--alias",
        f"cnet-{role}",
        "--host",
        "127.0.0.1",
        "--port",
        str(port),
        "--n-gpu-layers",
        "0",
        "--threads",
        str(threads),
        "--ctx-size",
        str(ctx_size),
        "--parallel",
        "1",
        "--no-webui",
    ]
    environment = os.environ.copy()
    environment.update(
        {
            "CUDA_VISIBLE_DEVICES": "",
            "HIP_VISIBLE_DEVICES": "",
            "ROCR_VISIBLE_DEVICES": "",
        }
    )
    started = time.monotonic()
    with log_path.open("wb") as log_handle:
        process = subprocess.Popen(
            command,
            stdout=log_handle,
            stderr=subprocess.STDOUT,
            env=environment,
            start_new_session=True,
        )
    try:
        deadline = time.monotonic() + startup_timeout
        while True:
            if process.poll() is not None:
                tail = log_path.read_text(errors="replace")[-4000:]
                raise RuntimeError(f"llama-server exited during {role} startup\n{tail}")
            try:
                health = _request_json(f"http://127.0.0.1:{port}/health", None, 2.0)
                if health.get("status") == "ok":
                    break
            except (OSError, urllib.error.URLError, RuntimeError, json.JSONDecodeError):
                pass
            if time.monotonic() >= deadline:
                tail = log_path.read_text(errors="replace")[-4000:]
                raise TimeoutError(f"llama-server startup timed out for {role}\n{tail}")
            time.sleep(0.5)

        results = []
        for probe_id, prompt, expected in PROBES:
            payload = {
                "messages": [{"role": "user", "content": prompt}],
                "max_tokens": 16,
                "temperature": 0,
                "seed": 42,
                "chat_template_kwargs": {"enable_thinking": False},
                "logprobs": True,
                "top_logprobs": 10,
            }
            try:
                response = _request_json(
                    f"http://127.0.0.1:{port}/v1/chat/completions", payload, request_timeout
                )
            except Exception as exc:
                results.append(
                    {
                        "id": probe_id,
                        "expected": expected,
                        "response": "",
                        "passed": False,
                        "error": str(exc),
                        "top_logprobs": [],
                        "normalized_entropy": 0.0,
                        "tokens_per_second": 0.0,
                    }
                )
                continue
            choice = response["choices"][0]
            content = str(choice["message"].get("content", "")).strip()
            logprob_content = choice.get("logprobs", {}).get("content", [])
            top = logprob_content[0].get("top_logprobs", []) if logprob_content else []
            results.append(
                {
                    "id": probe_id,
                    "expected": expected,
                    "response": content,
                    "passed": content == expected,
                    "top_logprobs": top,
                    "normalized_entropy": normalized_entropy(top),
                    "tokens_per_second": float(
                        response.get("timings", {}).get("predicted_per_second", 0.0)
                    ),
                }
            )
        passed = sum(1 for result in results if result["passed"])
        return {
            "role": role,
            "model": str(model.resolve()),
            "startup_seconds": time.monotonic() - started,
            "quality_passed": passed,
            "quality_total": len(results),
            "quality_pass_fraction": passed / len(results),
            "probes": results,
        }
    finally:
        _stop_process(process)
        log_path.unlink(missing_ok=True)


def _calibrate_uncertainty(probes: list[dict[str, Any]]) -> dict[str, float]:
    values = sorted(float(probe["normalized_entropy"]) for probe in probes)
    if not values:
        return {"low": 0.0, "high": 1.0, "median": 0.0}
    return {
        "low": values[max(0, len(values) // 4)],
        "high": values[min(len(values) - 1, (3 * len(values)) // 4)],
        "median": values[len(values) // 2],
    }


def run_campaign(args: argparse.Namespace) -> dict[str, Any]:
    reference = args.reference.resolve()
    candidate = args.candidate.resolve()
    qgkp = args.qgkp.resolve()
    server = args.server.resolve()
    library = args.library.resolve()
    qgkp_cli = args.qgkp_cli.resolve()

    require_magic(reference, b"GGUF")
    require_magic(candidate, b"GGUF")
    require_magic(qgkp, b"QGKP")
    reference_quantization = inspect_gguf(reference)
    candidate_quantization = inspect_gguf(candidate)
    structure_reasons = structural_mismatch_reasons(reference_quantization, candidate_quantization)
    quantization_evidence = quantization_diagnosis(reference_quantization, candidate_quantization)
    if not server.is_file() or not os.access(server, os.X_OK):
        raise ValueError(f"llama-server is not executable: {server}")
    if not library.is_file():
        raise ValueError(f"CNET library not found: {library}")
    if not qgkp_cli.is_file() or not os.access(qgkp_cli, os.X_OK):
        raise ValueError(f"QGKP CLI is not executable: {qgkp_cli}")

    inspection = subprocess.run(
        [str(qgkp_cli), "inspect", str(qgkp)],
        check=True,
        text=True,
        capture_output=True,
        timeout=30,
    ).stdout.strip()
    reference_sha = sha256_file(reference)
    qgkp_round_trip = None
    if args.materialized_cache:
        cache = args.materialized_cache.resolve()
        require_magic(cache, b"GGUF")
        cache_sha = sha256_file(cache)
        qgkp_round_trip = {
            "cache": str(cache),
            "cache_sha256": cache_sha,
            "byte_identical": cache_sha == reference_sha and cache.stat().st_size == reference.stat().st_size,
        }

    reference_run = probe_model(
        server, reference, "reference", args.startup_timeout, args.request_timeout, args.threads, args.ctx_size
    )
    candidate_run = probe_model(
        server, candidate, "candidate", args.startup_timeout, args.request_timeout, args.threads, args.ctx_size
    )
    admission = admit_candidate(
        reference_run,
        candidate_run,
        args.max_quality_delta,
        structural_reasons=structure_reasons,
    )
    selected_model = candidate if admission["admitted"] else reference
    selected_initial = candidate_run if admission["admitted"] else reference_run
    restart_run = probe_model(
        server, selected_model, "selected-restart", args.startup_timeout, args.request_timeout, args.threads, args.ctx_size
    )

    distributions = []
    for ref_probe, cand_probe in zip(reference_run["probes"], candidate_run["probes"]):
        metrics = distribution_metrics(ref_probe["top_logprobs"], cand_probe["top_logprobs"])
        metrics["cnet_grads"] = cnet_grad_norms(
            library, ref_probe["top_logprobs"], cand_probe["top_logprobs"]
        )
        metrics["id"] = ref_probe["id"]
        distributions.append(metrics)

    restart_match = all(
        initial["response"] == restarted["response"]
        for initial, restarted in zip(selected_initial["probes"], restart_run["probes"])
    )
    reference_operational = reference_run["quality_pass_fraction"] > 0.0
    selected_quality_preserved = (
        restart_run["quality_pass_fraction"] >= selected_initial["quality_pass_fraction"]
    )
    round_trip_ok = qgkp_round_trip is None or qgkp_round_trip["byte_identical"]
    overall_pass = reference_operational and selected_quality_preserved and restart_match and round_trip_ok

    return {
        "schema_version": 2,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "execution": {
            "cpu_only": True,
            "server": str(server),
            "threads": args.threads,
            "ctx_size": args.ctx_size,
            "max_quality_delta": args.max_quality_delta,
        },
        "artifacts": {
            "reference": {
                "path": str(reference),
                "bytes": reference.stat().st_size,
                "sha256": reference_sha,
                "gguf": reference_quantization,
            },
            "candidate": {
                "path": str(candidate),
                "bytes": candidate.stat().st_size,
                "sha256": sha256_file(candidate),
                "storage_ratio_vs_reference": candidate.stat().st_size / reference.stat().st_size,
                "gguf": candidate_quantization,
            },
            "qgkp": {
                "path": str(qgkp),
                "bytes": qgkp.stat().st_size,
                "inspection": inspection,
                "round_trip": qgkp_round_trip,
            },
        },
        "reference": reference_run,
        "candidate": candidate_run,
        "quantization_diagnosis": quantization_evidence,
        "admission": admission,
        "selected_model": {
            "role": admission["selected_role"],
            "path": str(selected_model),
        },
        "selected_restart": restart_run,
        "restart_integrity": {
            "responses_identical": restart_match,
            "quality_preserved": selected_quality_preserved,
        },
        "uncertainty_calibration": {
            "reference": _calibrate_uncertainty(reference_run["probes"]),
            "candidate": _calibrate_uncertainty(candidate_run["probes"]),
        },
        "distribution_evidence": distributions,
        "overall_pass": overall_pass,
        "verdict": (
            "PASS_CANDIDATE_ADMITTED"
            if overall_pass and admission["admitted"]
            else "PASS_CANDIDATE_QUARANTINED"
            if overall_pass
            else "FAIL"
        ),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--qgkp", type=Path, required=True)
    parser.add_argument("--materialized-cache", type=Path)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--library", type=Path, default=Path("cnet.so"))
    parser.add_argument("--qgkp-cli", type=Path, default=Path("bin/cnet_qgkp"))
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--ctx-size", type=int, default=512)
    parser.add_argument("--startup-timeout", type=float, default=120.0)
    parser.add_argument("--request-timeout", type=float, default=90.0)
    parser.add_argument("--max-quality-delta", type=float, default=0.0)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        report = run_campaign(args)
    except Exception as exc:
        print(json.dumps({"overall_pass": False, "verdict": "ERROR", "error": str(exc)}, indent=2))
        return 1
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n")
    print(json.dumps({
        "verdict": report["verdict"],
        "selected_role": report["selected_model"]["role"],
        "reference_quality": report["reference"]["quality_pass_fraction"],
        "candidate_quality": report["candidate"]["quality_pass_fraction"],
        "restart_integrity": report["restart_integrity"],
        "report": str(args.report),
    }, indent=2))
    return 0 if report["overall_pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
