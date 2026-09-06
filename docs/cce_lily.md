# Shared-basis deep adapters

[Lily's API](../include/cce/cce_lily.h) adds low-rank corrections across a
same-width frozen layer stack. A shared input projection uses
`rank × width × (layers + 1)` parameters; independent per-layer projections
use `2 × layers × rank × width`.

Sharing is a prior, not a guaranteed improvement. The historical fixture
underfit independent per-layer targets. Both positive and negative tables are
preserved in the [archive](MAINTENANCE.md).

## Implemented surfaces

- Linear-stack apply/merge and exact-gradient training.
- DS residual capture, including served and multi-token/context variants.
- Residual fitting and serve-in-the-loop refitting.
- [Registry policy](../include/router/registry_lily.h) for held-out fixes,
  regressions and net gain.

The old header's introductory “not wired” description predates later API
declarations and tests. Use those declarations and focused gates as the
implementation boundary. DS hooks do not imply a general GGUF q/k/v/o adapter.

```sh
make cce_lily_test cce_lily_serve cce_lily_collect
make cce_lily_teacher registry_lily_test registry_lily_compute
```

## Known limits

Independent per-layer offline fitting can over-correct when all deltas run
together. Serve-in-the-loop fitting addresses that effect on the recorded
smooth compression fixture; it does not solve every intervention.

The sparse-attention compute-reduction fixture loses information and rejected
the low-rank candidate under its regression policy. The recorded full-rank
ceiling was only about 32% gap recovery there. Keep this negative result
visible when considering further work.

Collected student states may be training inputs, but target labels must retain
independent provenance. Neither a synthetic teacher trajectory nor a tiny
runtime fixture proves real-model quality. No contract floor is relaxed.
