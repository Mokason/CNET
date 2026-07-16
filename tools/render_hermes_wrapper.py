#!/usr/bin/env python3
"""Render a fail-closed Hermes launcher manifest from real-model evidence."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def build_manifest(report: dict[str, Any], report_path: Path, server: Path) -> dict[str, Any]:
    if not report.get("overall_pass"):
        raise ValueError("acceptance report is not green")
    restart = report.get("restart_integrity", {})
    if not restart.get("responses_identical") or not restart.get("quality_preserved"):
        raise ValueError("selected model did not survive restart")

    selected = report.get("selected_model", {})
    role = selected.get("role")
    if role not in ("reference", "candidate"):
        raise ValueError(f"invalid selected model role: {role!r}")
    artifact = report.get("artifacts", {}).get(role, {})
    if selected.get("path") != artifact.get("path"):
        raise ValueError("selected model path does not match campaign artifact")
    if not artifact.get("sha256"):
        raise ValueError("selected artifact lacks SHA-256 evidence")

    return {
        "schema_version": 1,
        "kind": "cnet-hermes-wrapper",
        "source_report": str(report_path.resolve()),
        "campaign_verdict": report.get("verdict"),
        "selected_model": {
            "role": role,
            "path": selected["path"],
            "sha256": artifact["sha256"],
            "bytes": artifact.get("bytes"),
        },
        "candidate_admission": report.get("admission", {}),
        "runtime": {
            "backend": "llama-server",
            "server": str(server),
            "cpu_only": True,
            "host": "127.0.0.1",
            "port": 0,
            "threads": 4,
            "ctx_size": 65536,
            "parallel": 1,
            "gpu_layers": 0,
        },
        "hermes": {
            "provider": "custom",
            "model": "qwen/qwen3.5-9b-cnet-selected",
            "api_mode": "chat_completions",
            "default_probe": "Reply with exactly Ready",
            "probe_expected": "Ready",
            "max_output_tokens": 64,
            "max_turns": 2,
            "query_timeout_seconds": 540,
            "start_command": "python3 tools/run_hermes_wrapper.py --manifest <manifest.json>",
        },
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        report = json.loads(args.report.read_text())
        manifest = build_manifest(report, args.report, args.server.resolve())
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(json.dumps({"status": "refused", "reason": str(exc)}))
        return 1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps({"status": "written", "manifest": str(args.output), "selected_role": manifest["selected_model"]["role"]}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
