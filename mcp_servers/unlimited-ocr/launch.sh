#!/bin/bash
# Wrapper for Unlimited-OCR MCP
# Adjust PYTHON path if you have a torch/ROCm environment

export PYTHONPATH="/home/marble/AI/CNET:${PYTHONPATH}"
# If you have a specific torch venv, activate it here:
# source /path/to/torch-venv/bin/activate

exec /home/marble/.hermes/hermes-agent/venv/bin/python /home/marble/AI/CNET/mcp_servers/unlimited-ocr/unlimited_ocr_mcp.py "$@"
