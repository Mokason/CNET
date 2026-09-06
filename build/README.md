# Legacy build/demo helpers

This directory contains the legacy console demo plus a small agent-memory
compatibility library. It is not the main CNET build entrypoint.

## Standalone helper test

From the repository root, use a new build directory:

```sh
doc_cmake_build=$(mktemp -d /tmp/cnet-agent-memory-build-XXXXXX)
cmake -S build -B "$doc_cmake_build"
cmake --build "$doc_cmake_build" --target agent_memory_tests
ctest --test-dir "$doc_cmake_build" --output-on-failure
```

The standalone CMake file does not link the full CNET dependencies needed
by its console target. Building every CMake target is therefore not the
supported recipe for the console. The root `make build_tool` target
supplies the legacy console dependencies; it is not a core release gate.

## Side effects and limits

[build.c](build.c) can write generated `built_<spec>.txt` artifacts and
training/demo outputs. It also contains network-capable import paths.
Do not run an interactive training/import command against live state during
a build check.

The speech demo's generated waveforms are synthetic label-derived fixtures;
they are not evidence of real speech recognition. Its small language demo
does not establish broad model competence.

The obsolete tracked `built_demo.txt` output is retired; tests may regenerate
it. See [main build guide](../docs/BUILD_AND_TEST.md) and
[cleanup recovery](../docs/MAINTENANCE.md).
