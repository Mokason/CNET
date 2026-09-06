# Retired Python behavior reference

`roe_evolve_tick.py.spec` preserves the replaced Python gardener behavior.
The active implementation is
[tools/roe_evolve_tick.c](../../tools/roe_evolve_tick.c), built as
`bin/roe_evolve_tick` by `make roe_evolve_tick`.

The native gate rejects restoration of `tools/roe_evolve_tick.py`.
Do not wire the retired spec into systemd, Make targets or product scripts.
This retirement applies to that gardener, not a blanket claim that all CNET
Python tools have been removed.

See [current teacher paths](../../docs/TEACH_PATH.md) and
[historical recovery](../../docs/MAINTENANCE.md).
