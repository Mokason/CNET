# Supplied-fact response operator — 2026-09-11

The three-capability experiment found that explicit evidence validation and
fixed-format composition improved the measured response task. It is admitted as
an explicit operation. The language and procedure candidates remain experimental.

`cnet_vsa_evidence_response` in `include/cnet_vsa_evidence.h` consumes signed facts,
a start entity, and a sequence of relation IDs. It checks all reachable branches,
refuses contradictions, missing paths, or multiple final endpoints, and emits the
unique answer with one complete evidence path. An optional proposed answer must
match. Intermediate branches may reconverge. A contradiction on an unreachable
edge does not invalidate the query under this declared contract.

The operator checks consequences of the supplied facts; their external truth and
provenance remain the caller's responsibility. It creates no capsule, receipt, or
new packaging format and does not bypass any existing certification gate.

Bounds: 128 entity IDs, eight relation IDs, eight hops, 512 signed facts. It uses
no heap and no mutable global state. Invalid inputs and short output buffers
cannot publish partial answers. Return codes are 0 for a supported answer, 1 for
refusal, -1 for invalid input, -2 for insufficient output space. `proposed=-1`
requests direct computation; other valid IDs require a matching answer.

Use the integrated CLI:

```sh
printf 'node000 rel00 node001\nnode001 rel01 node002\n' > /tmp/cnet-facts.txt
bin/cnet_vsa_cli explain-facts /tmp/cnet-facts.txt 0 0 1
```

Output:

```text
node002. node000 rel00 node001. node001 rel01 node002.
```

Fact rows are `nodeNNN relRR nodeNNN` or `nodeNNN not relRR nodeNNN`, using bounded
numeric IDs. Whitespace and blank lines are accepted. Trailing content, embedded
NUL bytes, invalid IDs, excessive lines/facts, and oversized rows refuse. CLI exit
codes are 0 for an answer, 1 for abstention, and 2 for input/operational errors.
The standalone command runs before the general CLI initializes its GPU/document
state. The operation is also available from the REPL.

This is an explicit typed operation. `auto`, centroid routing, and the live crawler
retain their existing behavior. Arbitrary-language selection of this operation
needs further measured work; the current grammar extension was not admitted.

Measurement: 96/96 response fixtures pass, including all 64 required refusals;
median native operator time is below 1 µs. Production n-gram composition on the
same supplied facts passes 2/96 and takes approximately 468 µs median. That is a
bounded task comparison, not a general text-quality score.

Resource accounting: standalone GCC object text is 1,644 bytes, data/BSS zero;
operator stack frame is 2,976 bytes, plus caller-owned facts and output. The n-gram
baseline allocates 5,279,768 bytes. The integrated ROCm CLI grows by 4,419 text,
16 data, and 208 BSS bytes (4,643 total section bytes), including its command
adapter and changed linker layout. The addition therefore is not zero-cost in
whole-executable size, despite much lower memory and latency for this operation.
No GPU or energy benefit is claimed.

Validation: RED CLI absence reproduced before wiring; native contract tests,
CLI success/refusal/malformed-input tests, 2,048 disjoint random graphs/4,096
oracle comparisons/341 accepted proof checks, ASan+UBSan, existing CLI/q8/
generation/router gates, and verify-fast all pass. The CLI test caught an embedded
NUL truncation bug during implementation; the final bounded byte reader rejects
it. The original measurement predates publication; clean-master validation is recorded in
`result/cnet_evidence_master_validation_20260911.md`.
