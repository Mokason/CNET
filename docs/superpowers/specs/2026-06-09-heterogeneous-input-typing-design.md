# Heterogeneous Multi-Input Typing — Design

Date: 2026-06-09
Status: Implemented 2026-06-09 (TDD; `make test` + `make hetero` + `make dag` +
`make route` + `make compose` green). Primitives carry an array of typed input
ports (v3 format, v1/v2 still load); planner builds & runs
conditional_increment(flag, hex_value(digit)) sourcing heterogeneous slots
separately. combine remodeled to two BINARY-4 ports.
Repo: G:\AI\CNET (not a git repo — written, not committed)

## Problem

DAG composition supports multi-input primitives, but every input slot is derived
from a single `Port input_port` (homogeneous fields). A primitive cannot declare
genuinely different input types (e.g. one ONEHOT-16 and one BINARY-4). This adds
an explicit array of typed input ports so a primitive can fuse heterogeneous
inputs, each sourced independently.

Decisions (brainstorming): **slots = explicit input ports** (a port's internal
field_count is one cohesive input from one source; remodel combine to two
BINARY-4 ports) and a **conditional_increment (flag + value)** demo.

## Contract model

`BinaryTransformNetwork` replaces `Port input_port` with:
```c
#define BTN_MAX_INPUT_PORTS 8
    Port   input_ports[BTN_MAX_INPUT_PORTS];
    size_t input_port_count;   /* each entry is one independently-sourced slot */
    Port   output_port;        /* unchanged: single output */
```
`port_compatible` / `port_validate` / `port_canonicalize` are unchanged (single
Port operations). API:
```c
int btn_set_input_ports(BinaryTransformNetwork *btn, const Port *inputs,
                        size_t n, Port output);    /* n in [1, BTN_MAX_INPUT_PORTS],
                                                      sum of totals == input_count */
int btn_set_ports(BinaryTransformNetwork *btn, Port input, Port output); /* wrapper n=1 */
```
`btn_init` defaults to one RAW input port spanning input_count. The five existing
single-input primitives keep `btn_set_ports`; combine switches to
`btn_set_input_ports([BINARY_MSB 4, BINARY_MSB 4], 2, BINARY_MSB 8)`.

## File format v3 (v1/v2 still load)

```
CNET_BTN 3
<in> <out> <hidden> <max_hidden> <lr>
INPUTS <n>
PORT_IN <fam> <fw> <fc>      (x n)
PORT_OUT <fam> <fw> <fc>
<biases...>
```
- v1: one RAW input port (input_count, 1).
- v2: the single PORT_IN becomes input_ports[0], count 1.
- v3: read INPUTS n, then n PORT_IN lines, then PORT_OUT.
- Saver always writes v3.
Backward compat tested against real captured fixtures: tests/fixtures/v1_hex_value.txt
(v1) and tests/fixtures/v2_hex_value.txt (v2, captured before the format flip).

## Planner / executor / linear router

- `dag_plan`: slot loop iterates `p->input_ports[0..count]`; each slot carries its
  own type, so heterogeneous matching is automatic (greedy first-fit per slot).
- `dag_execute`: assemble at running offset = sum of totals of prior input ports;
  canonicalize each child to `input_ports[s]`; `port_validate` per slot;
  `btn_forward` the flat assembled vector; canonicalize output.
- `route_plan` / `route_execute` (linear): only primitives with
  `input_port_count == 1` are usable, using `input_ports[0]`. Multi-input
  primitives are reachable only via the DAG planner.

## conditional_increment primitive + demo

New frozen primitive: inputs `[BINARY_MSB 1 (flag), BINARY_MSB 4 (value)]` ->
output `BINARY_MSB 5` = `flag ? value+1 : value`. Trained on 32 samples (flag x
value), lr 0.8 (per the combine lesson), saved to `cond_increment_weights.txt`.

`make hetero` (tests/hetero_demo.c, runs nn_demo first): registry
{hex_value, conditional_increment}; sources a BINARY-1 flag and an ONEHOT-16
digit; goal BINARY-5. Planner builds
`conditional_increment(flag_src, hex_value(digit_src))` -- two differently-typed
inputs sourced from different places -- executes it, asserts `flag ? value+1 :
value` across digits and both flag values.

## Tests (TDD, three cycles, each with teeth)

A. Contract (`test_contract.c`): v3 round-trip of a 2-input-port BTN (assert both
   input_ports + count); `btn_set_input_ports` rejects total mismatch, n=0,
   n>BTN_MAX_INPUT_PORTS; v2 fixture loads as one input port; v1 fixture RAW.
B. Planner (`test_dag.c`): synthetic primitive with two DIFFERENT slot types
   [BINARY_MSB 1, BINARY_MSB 4]; dag_plan matches the flag source to slot 0 and
   routes the digit source through a hexlike to slot 1; a registry/source set
   that cannot type one slot -> -1.
C. Integration (`tests/hetero_demo.c`, `make hetero`): real conditional_increment
   + hex_value; assert `flag ? value+1 : value` for several (digit, flag) pairs.
Regressions: `make test`, `make dag`, `make route`, `make compose` stay green;
combine remodeled to two ports must still pass `make dag`.

## Verification gate

No ASan on this toolchain (MinGW). Gates: `make test` + `make dag` + `make hetero`
+ clean `-Wall -Wextra -pedantic` build.

## Out of scope (YAGNI)

- Single output per primitive (multi-output is future).
- Greedy first-fit source assignment (no bipartite matching).
- Learned scoring / ranking. BTN_MAX_INPUT_PORTS = 8.
