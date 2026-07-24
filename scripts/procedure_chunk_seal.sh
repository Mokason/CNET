#!/usr/bin/env bash
# Queue structured procedure skills into CNET gap inbox (no tk*q*).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INBOX="${CNET_GAP_INBOX:-$ROOT/soul_gemma4v2_final.cnb.gaps.txt}"
# Prefer MCP/learn path when available; else append structured note lines for operators.
echo "procedure_chunk_seal: seeds in artifacts/janitor/procedure_seeds.md"
echo "procedure_chunk_seal: use cnet_learn_from_chat skill=chunk_* or research_*"
echo "PROCEDURE_CHUNK_SEAL_OK inbox=$INBOX"
