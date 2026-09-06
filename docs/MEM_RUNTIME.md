# Capsule memory runtime

The [memory API](../include/cnet_mem_runtime.h) combines three bounded stores:

| Store | Role |
| --- | --- |
| STM | Hot pins for already admitted units |
| LTM | Named capsule-directory index, imported on demand |
| FORM | Candidate records awaiting verified import and promotion |

This adapter's public capacities are 64 STM slots, 128 LTM slots and 64 FORM
items. They are not the [capsule core's](CAPSULE_CORE.md) 4096-directory limit.

Resolution applies the ASI catalog gate, tries STM, then LTM import, otherwise
abstains. `cnet_mem_resolve_ex` accepts host world-mask and residual evidence;
the compatibility wrapper supplies an all-bits world mask and an unmeasured
residual. Callers needing those restrictions must use the explicit interface.

Serve does not train. FORM needs a sealed capsule, a score beating the
incumbent and successful import; continual regression can reject it.
A submitted score or episode is not independent certification.

`make mem_runtime` checks the integration. The implementation and statistics
are in [cnet_mem_runtime.c](../src/memory/cnet_mem_runtime.c).
Do not infer daemon-wide hot swapping, concurrency guarantees or real-world
cache performance from this standalone adapter. See [remaining scope](../tasks/todo.md).
