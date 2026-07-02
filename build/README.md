# CNET Build Tool

This folder contains a tiny C-based build/demo tool and the supporting
agent-memory/MCP compatibility layer.

## What is included
- `build.c`: Console entry point (`build_tool`) that generates `built_<spec>.txt` artifacts (reports use .txt to avoid stray .md; only common-sense README.md kept in tree).
- `include/agent_memory.h`: Public API for memory/sanitization/file-write helpers.
- `src/agent_memory.c`: Implementations for the missing symbols:
  - filename sanitization
  - thought recording
  - file write shim (`port_contract_mcp_file_write`)
- `tests/test_agent_memory.c`: Minimal tests for the helper layer.
- `CMakeLists.txt`: Build/test wiring.

## Build

```bash
cmake -S . -B build
cmake --build build
```

Run the tool:

```bash
cd build
.\build_tool --build "a story about Granny and a woodchipper"
```

From the interactive prompt:

```bash
.\build_tool
build a story about the moon
train speech
train lm
```

Available commands:
- `build <spec>` — create an artifact.
- `--learn-fable5 [source] [--limit N]` — import sentence/response pairs from a JSONL source and cache locally.
- `train speech` or `--train-speech-contract` — train a speech_command contract (BTN + contract file) by fetching labels from https://huggingface.co/datasets and running `btn_train_dynamic` + certify. Outputs `build/speech_command_*`.
- `train lm` (or "train llm") — train *our own* fully internal CNET-native generative / LLM-type model (next-token step BTN + full `CNET_CONTRACT`). Uses the exact same training, port, and certification machinery as everything else. No external models. Artifacts: `cnet_own_lm_weights.txt`, `cnet_own_lm_contract.txt`, reports. This is the foundation for "LLM logic we can train inside".
- `status` — print quick status.
- `quit` / `exit` — terminate.

Learning mode:

```bash
.\build_tool_testexe --learn-fable5 "https://huggingface.co/datasets/Glint-Research/Fable-5-traces/resolve/main/fable5_cot_merged.jsonl?download=true" --limit 200
.\build_tool_testexe --memory-stats
```

If the local environment has no network access, you can pass a local JSONL file with keys `context`, `completion`, and optionally `output_type`.

## Tests

```bash
cd build
ctest
```

This runs `agent_memory_tests`, a lightweight smoke test for filename sanitization and write behavior.
