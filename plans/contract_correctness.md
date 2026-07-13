# Contract Correctness Optimization

## Place
The live CNET contract boundary in `src/contract/contract.c`, where frozen descriptors become runtime authority.

## Dilemma
`contract_init_frozen()` copies only bounded port arrays but records unbounded caller counts and silently truncates invalid names. Later digest/certification paths trust those counts and can read beyond the embedded arrays.

## Consequence
Malformed descriptors can become apparently valid contracts, corrupt certification identity, or trigger out-of-bounds reads. Systematic component: descriptor validation. Irreducible noise: none; malformed shape is deterministic and must be refused.

## Tracer
RED: an oversized frozen descriptor and overlong name must be rejected without mutating the destination.
GREEN: validate name, parent, counts, pointers, dimensions, tags, table arithmetic, and exemplar slices before publishing the contract.
