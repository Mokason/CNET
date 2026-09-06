# Retired Python — reference spec only, NOT the product path

CNET product path is C. These files are kept as behavioural specs for the C
reimplementations and must never be referenced from a systemd ExecStart, a
Makefile target, or a gate.

| retired | replaced by | gate |
|---|---|---|
| `roe_evolve_tick.py.spec` | `tools/roe_evolve_tick.c` -> `bin/roe_evolve_tick` | `make roe_evolve_tick` |

`tools/roe_evolve_tick_gate.c` asserts `tools/roe_evolve_tick.py` is absent, so
restoring it into `tools/` fails the build.
