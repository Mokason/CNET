#ifndef CONTRACT_UNIT_H
#define CONTRACT_UNIT_H

/* CNET unit file ("CNU1"): ONE sealed binary artifact per primitive that
 * carries the weights AND the contract that specifies them, replacing the
 * <name>.btn + <name>.contract text pair.
 *
 * Compression is format-aware rather than a codec:
 *  - weights are binary f64 (live slots only), ~3x smaller than %.17g text
 *    and bit-exact (certification replays must stay exact);
 *  - contract exemplars are canonical 0/1 values by definition, so they are
 *    BIT-PACKED: 1 bit per value, 64x smaller than raw doubles.
 * The whole payload is sealed with FNV-1a; unit_load verifies the seal
 * BEFORE parsing and refuses any flipped byte or truncation.
 *
 * A unit is coherent by construction: the contract's signature is the
 * btn's signature (unit_save refuses a mismatch), and all exemplar values
 * must be canonical 0/1. Certification stays on-demand and unpersisted —
 * a unit stores the claim (weights + spec), btn_certify grants it (cheap
 * now via the certification cache).
 *
 * .stats and .expansion sidecars stay separate on purpose: evidence and
 * recipes have their own lifecycle (see reliability-persistence design).
 */

#include "contract.h"

/* Write btn + contract as one sealed unit. Refuses (-1): invalid name,
 * signature mismatch between btn and contract, or any non-canonical
 * (not exactly 0.0/1.0) exemplar value. */
int unit_save(const BinaryTransformNetwork *btn, const Contract *c,
              const char *path);

/* Load a unit into caller structs. Verifies the seal over the full payload
 * before parsing; bounds-checks every count; validates every exemplar port
 * slice (like contract_load). On success the btn is initialized (btn_free
 * to release), the contract owns its tables (contract_free) and has
 * seal_verified = 1. Returns 0, or -1 with both outputs untouched/freed. */
int unit_load(BinaryTransformNetwork *btn, Contract *c, const char *path);

#endif /* CONTRACT_UNIT_H */
