#!/usr/bin/env python3
"""Launch a CNET-admitted GGUF behind llama-server and route Hermes to it."""

from __future__ import annotations

import argparse
import hashlib
import json
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


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(8 * 1024 * 1024)
            if not chunk:
                return digest.hexdigest()
            digest.update(chunk)


def validate_manifest(manifest: dict[str, Any]) -> Path:
    if manifest.get("schema_version") != 1 or manifest.get("kind") != "cnet-hermes-wrapper":
        raise ValueError("unsupported Hermes wrapper manifest")
    runtime = manifest.get("runtime", {})
    if runtime.get("backend") != "llama-server":
        raise ValueError("only llama-server wrappers are supported")
    if runtime.get("cpu_only") is not True or int(runtime.get("gpu_layers", -1)) != 0:
        raise ValueError("wrapper must be CPU-only with gpu_layers=0")
    if runtime.get("host") not in ("127.0.0.1", "localhost"):
        raise ValueError("wrapper must bind to loopback")
    if int(runtime.get("ctx_size", 0)) < 65_536:
        raise ValueError("Hermes wrappers require ctx_size >= 65536")
    selected = manifest.get("selected_model", {})
    if selected.get("role") not in ("reference", "candidate"):
        raise ValueError("wrapper lacks a valid selected role")
    path = Path(str(selected.get("path", ""))).resolve()
    if not path.is_file():
        raise ValueError(f"selected model not found: {path}")
    with path.open("rb") as handle:
        if handle.read(4) != b"GGUF":
            raise ValueError(f"selected model is not GGUF: {path}")
    expected = str(selected.get("sha256", ""))
    if len(expected) != 64 or _sha256(path) != expected:
        raise ValueError("selected model SHA-256 mismatch")
    hermes = manifest.get("hermes", {})
    if hermes.get("provider") != "custom" or not hermes.get("model"):
        raise ValueError("Hermes provider/model is not configured")
    if not hermes.get("default_probe") or not hermes.get("probe_expected"):
        raise ValueError("Hermes probe contract is incomplete")
    max_output = int(hermes.get("max_output_tokens", 0))
    if max_output < 1 or max_output > 256:
        raise ValueError("Hermes max_output_tokens must be between 1 and 256")
    query_timeout = float(hermes.get("query_timeout_seconds", 0))
    if query_timeout < 180 or query_timeout > 600:
        raise ValueError("Hermes query_timeout_seconds must be between 180 and 600")
    max_turns = int(hermes.get("max_turns", 0))
    if max_turns < 1 or max_turns > 3:
        raise ValueError("Hermes max_turns must be between 1 and 3")
    return path


def build_server_command(manifest: dict[str, Any], model: Path, port: int) -> list[str]:
    runtime = manifest["runtime"]
    return [
        str(runtime["server"]),
        "--model",
        str(model),
        "--alias",
        str(manifest["hermes"]["model"]),
        "--host",
        "127.0.0.1",
        "--port",
        str(port),
        "--n-gpu-layers",
        "0",
        "--threads",
        str(int(runtime.get("threads", 4))),
        "--ctx-size",
        str(int(runtime.get("ctx_size", 8192))),
        "--parallel",
        str(int(runtime.get("parallel", 1))),
        "--jinja",
        "--chat-template-kwargs",
        '{"enable_thinking":false}',
        "--reasoning",
        "off",
        "--no-webui",
    ]


def build_hermes_command(
    manifest: dict[str, Any], query: str, hermes_executable: str = "hermes"
) -> list[str]:
    settings = manifest["hermes"]
    return [
        hermes_executable,
        "chat",
        "--query",
        query,
        "--model",
        str(settings["model"]),
        "--provider",
        str(settings["provider"]),
        "--toolsets",
        "",
        "--ignore-rules",
        "--max-turns",
        str(int(settings["max_turns"])),
        "--quiet",
    ]


def build_hermes_config(manifest: dict[str, Any], base_url: str) -> dict[str, Any]:
    settings = manifest["hermes"]
    return {
        "model": {
            "default": str(settings["model"]),
            "provider": str(settings["provider"]),
            "base_url": base_url,
            "api_mode": str(settings.get("api_mode", "chat_completions")),
            "context_length": int(manifest["runtime"]["ctx_size"]),
            "max_tokens": int(settings["max_output_tokens"]),
        },
    }


def probe_matched(output: str, expected: str) -> bool:
    lines = [line.strip() for line in output.splitlines() if line.strip()]
    if not lines or any("Reached maximum iterations" in line for line in lines):
        return False
    return lines[-1] == expected


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def _healthy(port: int) -> bool:
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=2) as response:
            return json.loads(response.read().decode("utf-8")).get("status") == "ok"
    except (OSError, urllib.error.URLError, urllib.error.HTTPError, json.JSONDecodeError):
        return False


def _stop(process: subprocess.Popen[bytes]) -> None:
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


def run_wrapper(
    manifest: dict[str, Any],
    model: Path,
    query: str,
    hermes_executable: str,
    startup_timeout: float,
    query_timeout: float,
) -> dict[str, Any]:
    server_path = Path(str(manifest["runtime"]["server"])).resolve()
    if not server_path.is_file() or not os.access(server_path, os.X_OK):
        raise ValueError(f"llama-server is not executable: {server_path}")
    port = int(manifest["runtime"].get("port", 0)) or _free_port()
    server_command = build_server_command(manifest, model, port)
    with tempfile.NamedTemporaryFile(prefix="cnet-hermes-server-", suffix=".log", delete=False) as handle:
        log_path = Path(handle.name)
    environment = os.environ.copy()
    environment.update(
        {
            "CUDA_VISIBLE_DEVICES": "",
            "HIP_VISIBLE_DEVICES": "",
            "ROCR_VISIBLE_DEVICES": "",
            "CUSTOM_BASE_URL": f"http://127.0.0.1:{port}/v1",
            "HERMES_DASHBOARD_AUTOSTART": "0",
        }
    )
    with log_path.open("wb") as log_handle:
        process = subprocess.Popen(
            server_command,
            stdout=log_handle,
            stderr=subprocess.STDOUT,
            env=environment,
            start_new_session=True,
        )
    started = time.monotonic()
    try:
        deadline = started + startup_timeout
        while not _healthy(port):
            if process.poll() is not None:
                tail = log_path.read_text(errors="replace")[-4000:]
                raise RuntimeError(f"llama-server exited during startup\n{tail}")
            if time.monotonic() >= deadline:
                tail = log_path.read_text(errors="replace")[-4000:]
                raise TimeoutError(f"llama-server startup timed out\n{tail}")
            time.sleep(0.5)

        ready_seconds = time.monotonic() - started
        command = build_hermes_command(manifest, query, hermes_executable)
        query_started = time.monotonic()
        with tempfile.TemporaryDirectory(prefix="cnet-hermes-home-") as hermes_home:
            home = Path(hermes_home)
            (home / "config.yaml").write_text(
                json.dumps(build_hermes_config(manifest, environment["CUSTOM_BASE_URL"]), indent=2)
                + "\n"
            )
            environment["HERMES_HOME"] = str(home)
            completed = subprocess.run(
                command,
                env=environment,
                text=True,
                capture_output=True,
                timeout=query_timeout,
            )
        output = completed.stdout.strip()
        error = completed.stderr.strip()
        expected = str(manifest["hermes"]["probe_expected"])
        matched = probe_matched(output, expected)
        final_line = next((line.strip() for line in reversed(output.splitlines()) if line.strip()), "")
        return {
            "status": "pass" if completed.returncode == 0 and matched else "fail",
            "returncode": completed.returncode,
            "expected": expected,
            "matched": matched,
            "final_line": final_line,
            "output": output,
            "stderr": error,
            "base_url": environment["CUSTOM_BASE_URL"],
            "server_ready_seconds": ready_seconds,
            "query_seconds": time.monotonic() - query_started,
        }
    finally:
        _stop(process)
        log_path.unlink(missing_ok=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--query")
    parser.add_argument("--hermes", default="hermes")
    parser.add_argument("--startup-timeout", type=float, default=120.0)
    parser.add_argument("--query-timeout", type=float)
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        manifest = json.loads(args.manifest.read_text())
        model = validate_manifest(manifest)
        query = args.query or str(manifest["hermes"]["default_probe"])
        port = int(manifest["runtime"].get("port", 0)) or 18080
        if args.dry_run:
            result = {
                "status": "dry-run",
                "server_command": build_server_command(manifest, model, port),
                "hermes_command": build_hermes_command(manifest, query, args.hermes),
                "cpu_only": True,
                "loopback": True,
            }
        else:
            result = run_wrapper(
                manifest,
                model,
                query,
                args.hermes,
                args.startup_timeout,
                float(args.query_timeout if args.query_timeout is not None else manifest["hermes"]["query_timeout_seconds"]),
            )
    except (OSError, ValueError, RuntimeError, TimeoutError, json.JSONDecodeError, subprocess.TimeoutExpired) as exc:
        print(json.dumps({"status": "error", "reason": str(exc)}, indent=2))
        return 1
    print(json.dumps(result, indent=2, ensure_ascii=False))
    return 0 if result["status"] in ("pass", "dry-run") else 1


if __name__ == "__main__":
    raise SystemExit(main())
