# CNET-ASI-5 v2 withdrawal record

Status: **WITHDRAWN; NEVER EXECUTED**

CNET-ASI-5 v2 is not benchmark evidence. It was withdrawn during the
pre-execution adversarial review because its fixture was not independent of
the post-v1 development corpus:

- several OOD prompts restated newly added refusal/development cases;
- covered templates reused newly added training surface forms; and
- the 32-row OOD range group contained only eight distinct prompts, each
  repeated four times.

The frozen release was built and a read-only baseline identity preflight was
performed, but no held-out completion request was sent to either backend. The
canonical v2 results directory was empty at withdrawal. No score or capability
claim may be derived from this version.

The files and digests are retained so the invalidated design remains
auditable. Replacement suites must use a fresh suite ID, paths, state root,
release identity, and an explicit native independence gate covering prompt
uniqueness and development-corpus overlap before any held-out execution.
