# Optional Hermes hosting

Hermes is an optional client/teacher integration. A model file path is not
a Hermes catalog model identifier, and a compression manifest is not
deployment acceptance.

The managed control-plane workflow separates:

1. A real-model acceptance report for reference/candidate artifacts.
2. `render-hermes-wrapper`, selecting an admitted candidate or reference fallback.
3. `run-hermes-wrapper`, a bounded local server/probe run from that manifest.

All three require intentional operator inputs. Rendering writes a manifest;
running starts a server and an isolated Hermes session. Do not run these
commands as documentation validation or against an existing profile by default.

The selected wrapper verifies model magic/identity and uses its CPU-only
llama.cpp probe configuration. It does not imply the same model passed a GPU
configuration. A rejected candidate remains quarantined.
The Phase-4 metadata tool alone does not prove that deployment path works.

See [phase-four record](../plans/phase4_uncertainty_hermes.md),
[improvement handoff](improvement_engine_integration.md) and
[operations](CNET_MARBLE_24_7.md). Original wrapper examples and dated probe
results remain in the [archive](MAINTENANCE.md).

MCP RouteOnRole can report a deterministic fallback. Read the returned
mechanism instead of treating the name as evidence of learned routing.
A loopback teacher/provider can itself contact a cloud service; verify its
actual backend before describing the workflow as offline.
