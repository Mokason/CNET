# Primitive Interface Contracts — Design

Date: 2026-06-08
Status: Implemented 2026-06-08 (TDD; `make test` + `make compose` green). All 5
BTN primitives persist v2 contracts; v1 files load as RAW (verified by fixture).
Repo: G:\AI\CNET (note: not a git repo — this doc is written but cannot be committed)

## Problem

CNET's north star is **composition of frozen primitives**. The toy-scale thesis
passed (`hex_value -> increment`, 16/16, see `make compose`), but composition
correctness currently rests on a human eyeballing two encoders to confirm their
bit layouts agree. Nothing in the system enforces that a producer's output
matches a consumer's input. A bit-order mismatch (`BINARY_MSB` vs `BINARY_LSB`)
passes a width check and silently produces garbage, and a frozen primitive fed
out-of-distribution input has no guard (the deeper analog of the past-the-
terminator OOB bug already fixed this session).

This makes composition **verifiable** instead of **eyeballed**: each primitive
carries a machine-checkable interface contract, stored with its weights.

## Design intent: domain-agnostic

The contract types the **representation on the wire**, not the **meaning**. A
`ONEHOT 16` port is a 16-way one-hot regardless of whether it encodes a hex
digit, a nucleotide, or a chess piece. `port_compatible` / `port_validate` never
inspect semantics. Therefore the same machinery scales to *any* domain: "general
knowledge, more base data" = more frozen primitives speaking these same
encodings. Do not bake text/hex/ASCII assumptions into the contract layer.

## Scope

- BTN (`BinaryTransformNetwork`) primitives only this build. The scalar
  `NeuralNetwork` nets (nibble/char) are out of scope for now (YAGNI).
- Storage: embedded in the weights file (single source of truth, cannot desync).
- Enforcement: static interface typing (wire-time) **and** runtime domain
  validation (per-inference).

## Port model

```c
typedef enum { PORT_RAW = 0, PORT_ONEHOT, PORT_BINARY_MSB, PORT_BINARY_LSB } PortFamily;

typedef struct {
    PortFamily family;     /* RAW = untyped/legacy; bit-order is encoded in the family */
    size_t     field_width;/* size of one field */
    size_t     field_count;/* repeated fields; total = field_width * field_count */
} Port;
```

`BinaryTransformNetwork` gains `Port input_port, output_port;`.

Primitive contracts (authored in main.c next to each `build_*_data`):

| primitive | input port        | output port      |
|-----------|-------------------|------------------|
| hex_value | `ONEHOT 16x1`     | `BINARY_MSB 4x1` |
| increment | `BINARY_MSB 4x1`  | `BINARY_MSB 5x1` |
| hex_char  | `ONEHOT 16x2`     | `BINARY_MSB 7x1` |
| word      | `ONEHOT 27x5`     | `ONEHOT 8x1`     |
| raw_word  | `BINARY_MSB 8x5`  | `ONEHOT 8x1`     |

## API

```c
/* Authoring: validates field_width*field_count == input_count / output_count. */
int  btn_set_ports(BinaryTransformNetwork *btn, Port input_port, Port output_port);

/* Static: producer's output port vs consumer's input port.
   Match requires family AND field_width AND field_count equal.
   RAW falls back to total-width equality (legacy v1 files). Returns 1/0. */
int  port_compatible(Port producer_output, Port consumer_input);

/* Runtime domain check of an actual vector against a port. Returns 1/0.
   ONEHOT      -> each field has exactly one value > 0.5
   BINARY_*    -> every value is unambiguous (< 0.25 or > 0.75)
   RAW         -> always valid (escape hatch) */
int  port_validate(Port port, const double *values);
```

`btn_init` initializes both ports to `PORT_RAW` with `field_width=input/output
count, field_count=1` so untyped networks are well-formed.

## File format v2

```
CNET_BTN 2
<in> <out> <hidden> <max_hidden> <lr>
PORT_IN  <family-token> <field_width> <field_count>
PORT_OUT <family-token> <field_width> <field_count>
<output biases...>
<weights...>
```

- Family tokens: `raw | onehot | binary_msb | binary_lsb`.
- `btn_load`: `version == 1` -> load exactly as today; ports default to RAW
  (width-only compatibility). `version == 2` -> read the two PORT lines after the
  dimension line, before the biases.
- `btn_save`: always writes version 2.
- Backward compatibility: existing v1 weight files still load. After re-running
  the demo, files are rewritten as v2 with real ports.

## Testing (each gate must have teeth)

1. `tests/test_contract.c` — hermetic, no weight files:
   - `port_compatible(BINARY_MSB 4x1, BINARY_LSB 4x1) == 0` (bit-order mismatch a
     width check would pass — the key teeth case).
   - `port_compatible(BINARY_MSB 4x1, BINARY_MSB 5x1) == 0` (width).
   - `port_compatible(ONEHOT 16x2, ONEHOT 16x2) == 1`; vs `ONEHOT 32x1 == 0`
     (field structure matters, not just total width).
   - hex_value.out vs increment.in -> 1.
   - `port_validate(ONEHOT 16x1, <clean one-hot>) == 1`; two-hot -> 0; all-0.3 -> 0.
   - `port_validate(BINARY_MSB 4x1, {1,0,1,0}) == 1`; `{0.5,0.5,0.5,0.5} == 0`.
2. `tests/test_composition.c` — upgrade: replace the ad-hoc width check with
   `port_compatible(hexval.output_port, incr.input_port)`, and call
   `port_validate(incr.input_port, inc_input)` on the actual handoff vector
   before feeding increment. Still prints the 16-row table and PASS.
3. Regression: `test_nn` and `test_encode_oob` stay green; `make compose` 16/16.

## Verification gate

This toolchain has no ASan runtime (MinGW; see project memory). Gates are the
behavioral tests above, run via `make test` and `make compose`, plus a clean
`-Wall -Wextra -pedantic` build.

## Out of scope (YAGNI)

- Partial/coercive compatibility (e.g. uint4 -> uint5 zero-extension).
- Per-port human-readable semantic labels (possible later; would be documentation
  only, not part of compatibility).
- Contracts for the scalar `NeuralNetwork` type.
- A generic composition/router API — composition is still wired by hand; the
  contract just makes that wiring checkable.
```
