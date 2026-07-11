#!/usr/bin/env python3
"""Prove that a staged DS4 API can execute inference, not merely list models."""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import NoReturn


def atomic_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(f"{path.name}.tmp.{os.getpid()}")
    with temp.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, sort_keys=True, separators=(",", ":"))
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temp, path)


def fail(verification: Path, endpoint: str, message: str) -> NoReturn:
    record = {
        "ok": False,
        "endpoint": endpoint,
        "error": message,
        "verified_unix_ns": time.time_ns(),
    }
    atomic_json(verification, record)
    print(f"verify_ds4_endpoint: {message}", file=sys.stderr)
    raise SystemExit(1)


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: verify_ds4_endpoint.py ENDPOINT READY_JSON VERIFY_JSON", file=sys.stderr)
        return 2
    endpoint = sys.argv[1].rstrip("/")
    ready_path = Path(sys.argv[2])
    verification_path = Path(sys.argv[3])
    payload = json.dumps(
        {
            "model": "deepseek-v4-flash",
            "messages": [{"role": "user", "content": "Reply with exactly CNET_READY."}],
            "max_tokens": 1,
            "temperature": 0,
            "stream": False,
        },
        separators=(",", ":"),
    ).encode("utf-8")
    request = urllib.request.Request(
        f"{endpoint}/v1/chat/completions",
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    started = time.perf_counter_ns()
    body: dict = {}
    try:
        with urllib.request.urlopen(request, timeout=600) as response:
            loaded = json.load(response)
            if not isinstance(loaded, dict):
                fail(verification_path, endpoint, "response is not a JSON object")
            body = loaded
    except urllib.error.HTTPError as exc:
        detail = exc.read(4096).decode("utf-8", errors="replace")
        fail(verification_path, endpoint, f"HTTP {exc.code}: {detail}")
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        fail(verification_path, endpoint, str(exc))

    elapsed_ns = time.perf_counter_ns() - started
    choices_value = body.get("choices")
    if not isinstance(choices_value, list) or not choices_value:
        fail(verification_path, endpoint, "response has no choices")
    first = choices_value[0]
    if not isinstance(first, dict) or not isinstance(first.get("message"), dict):
        fail(verification_path, endpoint, "response has no assistant message")

    usage = body.get("usage", {})
    if not isinstance(usage, dict):
        usage = {}
    record = {
        "ok": True,
        "endpoint": endpoint,
        "elapsed_ns": elapsed_ns,
        "response_id": body.get("id", ""),
        "finish_reason": first.get("finish_reason", ""),
        "completion_tokens": usage.get("completion_tokens"),
        "verified_unix_ns": time.time_ns(),
    }
    atomic_json(verification_path, record)

    readiness: dict = {}
    if ready_path.exists():
        try:
            readiness = json.loads(ready_path.read_text(encoding="utf-8"))
        except (OSError, ValueError, json.JSONDecodeError):
            readiness = {}
    readiness.update(
        {
            "ready": True,
            "api_ready": True,
            "inference_verified": True,
            "inference_elapsed_ns": elapsed_ns,
            "verified_unix_ns": record["verified_unix_ns"],
        }
    )
    atomic_json(ready_path, readiness)
    print(json.dumps(record, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
