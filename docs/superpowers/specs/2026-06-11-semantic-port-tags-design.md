# Semantic Port Tags — Design

Date: 2026-06-11
Status: Implemented 2026-06-11 (TDD; `make test` + all demos green).
Repo: G:\AI\CNET (not a git repo — written, not committed)

## Problem

Contracts described representation only: `{family, field_width, field_count}`.
Two unrelated primitives that both emit `BINARY_MSB 4` were interchangeable to
the planner — well-typed but semantically wrong plans become the dominant
failure mode as the registry grows (flagged in the 2026-06-10 analysis; made
more urgent by the now-complete planner search, which finds MORE well-typed
plans). The contract system exists to prevent wrong composition; meaning has
to participate in compatibility.

## Decision: optional nominal tag on Port, AND-ed into compatibility

`Port` gains `char tag[PORT_TAG_MAX]` (32 incl. nul; empty = untagged). A tag
names what the value MEANS ("nibble_value", "word_token"), never how it is
encoded — encoding stays in family/width/count.

Matching rules (`port_compatible`):
- Both tagged and different -> INCOMPATIBLE, regardless of representation.
- Otherwise -> exactly the previous representation rules (incl. RAW fallback).

Untagged is a wildcard. This mirrors the existing RAW philosophy (absence of
information falls back to weaker checking; information present on both sides
must agree) and is what lets every existing frozen file (v1–v3, all untagged)
keep loading and composing. The cost is honest and documented: a tagged
consumer gets no protection from an UNtagged producer. Strict matching was
rejected because it breaks gradual adoption and orphans existing artifacts.

Tags constrain composition in addition to representation — never instead of
it (same meaning in a different encoding still needs a converting primitive).

## API and validation

- `int port_set_tag(Port *port, const char *tag)` — charset `[A-Za-z0-9_]`,
  length <= 31; empty string clears. `-` is excluded by the charset, so the
  file sentinel below can never collide with a real tag.
- Struct-literal `(Port){family, w, c}` still compiles and zero-inits the tag
  (= untagged): existing authoring code is source-compatible. Stack helpers
  that build Ports field-by-field (the tests' `P()`) must zero `tag[0]`.

## Planner integration

- `port_compatible` is the single choke point, so route/dag planners and both
  executors inherit tag awareness with no structural change.
- EXCEPT `route_plan`'s visited-set dedup (`same_port_type`), which must also
  compare tags: two same-representation outputs with different tags are
  distinct port types. Without this, whichever tagged producer is enqueued
  first shadows the other and reachable goals report unreachable (pinned by a
  registry-ordering test).

## File format v4

`PORT_IN <family> <w> <c> <tag>` / `PORT_OUT <family> <w> <c> <tag>`, with
`-` as the untagged sentinel (fscanf %s cannot read an empty token). Saves
always write v4; loader accepts v1–v4. New fixture
`tests/fixtures/v3_hex_value.txt` (snapshot of a real pre-tag file) pins v3
loading forever, alongside the existing v1/v2 fixtures.

## Demo tagging (main.c)

Real primitives get tags end-to-end: hex_value `hex_digit -> nibble_value`,
increment `nibble_value -> incremented_value`, combine `nibble_value x2 ->
byte_value`, conditional_increment `[cond_flag, nibble_value] -> cond_result`,
hex_char `hex_digit_pair -> ascii_char`, word `letter_seq -> word_token`,
raw_word `ascii_bytes -> word_token`. Demos construct untagged goal ports and
sources (wildcard), so they pass unchanged; the chain hex_value->increment
now also type-checks by meaning.

## Tests (TDD)

- test_contract.c: tag mismatch rejects despite identical representation;
  untagged<->tagged wildcard accepts; tag never overrides representation
  mismatch; `port_set_tag` rejects overlong/invalid charset; v4 round-trip
  preserves tags (input + output); v3 fixture loads untagged.
- test_router.c: registry has a WRONG-meaning producer first (same
  representation, untagged input so it is reachable) and the right one
  second; the route must go through the right one (also pins tag-aware
  visited dedup).
- test_dag.c: same shape for the DAG planner; asserts the plan's child is the
  correctly-tagged producer.

## Verification gate

Full rebuild + suite + all demos. Weight regeneration: training math is
untouched, so regenerated files must differ ONLY in the version line and
PORT_IN/PORT_OUT lines (tags added); every numeric line byte-identical.

## Out of scope (YAGNI)

- Tag ontologies/subtyping (a tag is an opaque atom; no hierarchy).
- Strict mode (rejecting untagged producers at tagged consumers).
- Learned scoring (unchanged next frontier; tags shrink the candidate set it
  will rank).
