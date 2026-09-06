# Historical specialist review

This path preserves navigation to the review of commit `0e07717`.
Its full original findings, proposed patches and measured checks are in the
[documentation archive](docs/MAINTENANCE.md).

Do not treat that review's “fails today” statements or proposed trust-order
patch as a current diagnosis. Registry state, certification and execution
authority must be checked against the current
[specialist API](include/specialist.h),
[implementation](src/specialist.c) and
[authority decision](plans/integrity_specialist_authority.md).

The review identified trust-axis consistency, missing edge-case coverage and
export-audit gaps. Whether a specific item is now fixed requires its current
focused test; a historical 24-check pass is not today's complete regression
result. Run `make specialist_authority` for that named authority contract.

Current architecture and review scope start at [docs/INDEX.md](docs/INDEX.md).
