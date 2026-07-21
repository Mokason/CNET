# Explicit agent roles (layer 3)

**Status**: vocabulary + policy table + harness/AICIMO wire (first slice)  
**Gate**: `make agent_role` → `AGENT_ROLE_PASS`

## Distinction

| Vocabulary | Answers | Lives in |
|---|---|---|
| `SpecialistRole` | Is this plannable / shadow / recipe / advisory? | `include/specialist.h` lifecycle view |
| **`CnetAgentRole`** | Who is speaking / stance / tool rights? | `include/cnet_agent_role.h` policy |

Agent roles are **dispatch layer 3** (orchestrator policy). They do not admit
specialists, compose plans, or seal units.

## Closed set

| Role | Preferred sampling | Caps (default) |
|---|---|---|
| `auditor` | DETERMINISTIC | inspect, certify, recall |
| `researcher` | BALANCED | recall, search, inspect |
| `coder` | FOCUSED | code, write, inspect |
| `critic` | FOCUSED | inspect, recall |
| `memory-witness` | DETERMINISTIC | recall, inspect |

None of the five carry `SEAL` by default.

Aliases: `memory_witness`, `witness` → `memory-witness`. Case-insensitive.

## Harness behavior

When `options.role` resolves as a known agent role:

1. AICIMO routes on the **canonical** name (`auditor`, …).
2. Under `SAMPLING_AUTO`, sampling starts from the role preference; high
   route uncertainty may still **downgrade** one step.
3. Explicit sampling override still wins (`aicimo_override`).
4. Backend system prompt is **role fragment**, then optional caller system.

Unknown free-form roles keep the previous adapter→profile map (back-compat).

## API

```c
cnet_agent_role_parse / resolve / policy / name / is_known
cnet_agent_role_compose_system(policy, caller_system)  /* free result */
```

## Route decision log

Set `CNET_ROUTE_LOG=/path/to/file.jsonl` before `cnet_harness_open` **or**
`soul_open`. Both surfaces share `cnet_route_log`.

### Harness (`probe_route` / `generate`)

| Field | Meaning |
|---|---|
| `mechanism` | `aicimo_agent_role` or `aicimo_role_hash` |
| `selected_expert` | AICIMO adapter index |
| `expert_profile` | effective sampling profile name |
| `entropy` | route uncertainty in [0,1] |
| `outcome` | `ok` / `err_backend` / … |
| `route_latency_ms` / `total_latency_ms` | decision / end-to-end |
| `cost` | `1 + prompt_tokens + generated_tokens` |

### Core CNET (`soul_route` / `soul_request`)

| Field | Meaning |
|---|---|
| `mechanism` | `planner` / `residual` / `no_plan` / `probe` |
| `selected_unit` | final plan step unit name |
| `plan_length` | certified plan steps |
| `selected_expert` | same as plan_length (step count) |
| `expert_profile` | `certified` / `residual` / `probe` / `none` |
| `entropy` | `1 - mean(step reliability)` |
| `outcome` | `ok` / `ok_residual` / `ok_probe` / `no_plan` / … |
| `route_latency_ms` | `route_plan` wall |
| `total_latency_ms` | plan + execute wall |
| `cost` | `1 + sum(adapter_cost or 1 per step)` |

Gate: `make route_log` → `ROUTE_LOG_PASS`; `make soul_host_test` covers live
SoulHost logging.

## Not in this slice

- Multi-step pipeline `researcher → coder → critic → auditor → memory-witness`
- SoulHost / MCP role fields
- Contrastive activation directions for real RouteOnRole (DS4 note)
