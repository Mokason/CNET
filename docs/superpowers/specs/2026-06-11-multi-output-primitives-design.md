# Multi-Output Primitives — Design

Date: 2026-06-11
Status: Implemented 2026-06-11 (TDD; `make test` + all demos green).
Repo: G:\AI\CNET (not a git repo — written, not committed)

## Problem

A primitive's output is a single typed port. A primitive that naturally
yields several independently-consumable results (split a byte into nibbles,
divmod, parse-into-fields) cannot expose them. This is the last deferred
planner frontier from the heterogeneous-input work, which solved exactly
this problem on the INPUT side — the design mirrors it.

## Model: typed output ports + planner-side projection

`Port output_port` becomes `Port output_ports[BTN_MAX_OUTPUT_PORTS]` +
`size_t output_port_count` (max 8, mirroring inputs). The flat output vector
is partitioned at running offsets, one segment per port, each with its own
family/width/count/tag.

The planner targets ANY output port: when goal/slot type T is compatible
with `output_ports[j]`, the DAG node records `output_index = j` and its
consumer receives ONLY segment j (canonicalized against port j). Unused
segments are computed and discarded — **projection, not sharing**. One
execution feeding multiple consumers (true shared sub-results) stays YAGNI,
as it has been since the DAG design; it would need multi-parent nodes and
result caching for no current demonstrated need.

Linear routing stays symmetric with the input-side rule: `route_plan` only
chains primitives with `output_port_count == 1` (multi-output primitives are
reachable through the DAG planner), so `RoutePlan` is unchanged.

## Reliability with multiple outputs

The recorded outcome covers the WHOLE output: success only if every
segment is in-domain under its port. A primitive that saturates the consumed
segment but emits garbage on another is not healthy, and the signal should
say so. Single-output behavior is unchanged (one segment = whole vector).

## API

- `btn_set_io_ports(btn, ins, n_in, outs, n_out)` — full generality; sums of
  port totals must equal input_count/output_count, counts in [1, max].
- `btn_set_input_ports(btn, ins, n, out)` becomes a 1-output wrapper;
  `btn_set_ports` stays the 1-in/1-out wrapper. Every existing call site
  compiles unchanged.

## File format v5

`OUTPUTS m` + m `PORT_OUT family w c tag` lines replace the single PORT_OUT.
Loader accepts v1–v5 (v2–v4 read one PORT_OUT into output_ports[0]). New
fixture tests/fixtures/v4_hex_value.txt pins v4 loading. Regenerated weights
must differ only in version/OUTPUTS/PORT lines; numeric payload
byte-identical (training math untouched).

## Demo: the `split` primitive (inverse of combine)

Input BINARY_MSB 8 "byte_value", outputs [BINARY_MSB 4 "nibble_value",
BINARY_MSB 4 "nibble_value"]. Per-bit identity, so it trains on the SAME
data arrays as `combine` — the same function under a different contract
decomposition, which is the point: ports are the interface, not the math.
Saved to split_weights.txt. New integration demo tests/split_demo.c
(`make split`): byte source + split + frozen increment; the planner builds
increment(split(byte)[0]) — selecting nibble output 0 — and the result must
equal hi_nibble + 1 for several bytes.

## Tests (TDD)

- test_contract.c: btn_set_io_ports validation (accept 4+4=8; reject bad
  totals / n_out 0 / n_out > max); v5 round-trip preserves both output
  ports' types and tags; v4 fixture loads as a single output port.
- test_dag.c: planner selects a NON-FIRST output port (outs [BINARY4,
  BINARY2], goal BINARY2 -> root output_index == 1); execution projects the
  selected segment (crafted biases: segment0 ~ 0.0, segment1 ~ 1.0; goal
  BINARY2 executes to {1,1}); whole-output reliability (consumed segment
  saturated, other segment ambiguous -> failure recorded).
- test_router.c: route_plan refuses to chain a multi-output primitive
  (single-output rule pinned).

## Out of scope (YAGNI)

- Shared sub-results / multi-consumer execution nodes (one forward feeding
  several branches) — projection recomputes per consumer.
- Per-output reliability counters (whole-output health only).
- Multi-output through the LINEAR router.
