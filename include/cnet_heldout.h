/* Held-out capability fixture consumption.
 *
 * WHY THIS EXISTS. `tests/run_capability_cert.py` used to export
 * `CNET_HELD_OUT_FIXTURE` and no evaluator ever read it: each one ran its own
 * hard-coded cases while the runner scanned stdout for marker strings. A
 * declared held-out case could therefore be edited freely and the certificate
 * stayed green, so the fixture SHA-256 in the report proved which file existed
 * and never which cases were evaluated.
 *
 * This is the consumption side of the fix. An evaluator reads its floors and
 * expectations THROUGH this API, records a per-case verdict, and prints a
 * receipt. The runner refuses to certify without a receipt that binds the exact
 * fixture bytes, every declared case id, and a consumed count equal to the
 * declared case count — so an evaluator that ignores the fixture, or does not
 * run at all, cannot be certified.
 *
 * TWO MODES, on purpose:
 *   standalone (CNET_HELD_OUT_FIXTURE unset) — every read returns the caller's
 *     fallback and no receipt is printed. `make sleep_consolidate` run by hand
 *     or by `verify` behaves exactly as it did before.
 *   bound (CNET_HELD_OUT_FIXTURE set) — the fixture MUST load, MUST declare
 *     this capability, and every read MUST resolve. A missing key, a wrong
 *     type, an unknown case id or an unconsumed case is an error and
 *     cnet_heldout_finish() reports failure.
 *
 * This is a test-support library: it never gates production serving, and its
 * only trust claim is "these exact fixture bytes drove these exact assertions".
 */
#ifndef CNET_HELDOUT_H
#define CNET_HELDOUT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_HELDOUT_MAX_CASES 32
#define CNET_HELDOUT_ID_MAX    64
#define CNET_HELDOUT_MAX_BYTES (1024u * 1024u)

typedef struct {
    int required;            /* 1 when CNET_HELD_OUT_FIXTURE named a fixture */
    int opened;              /* 1 when the fixture parsed cleanly */
    char path[512];
    char capability[64];
    char sha256[65];
    char *text;              /* whole fixture, NUL-terminated */
    size_t len;
    size_t case_count;
    char case_id[CNET_HELDOUT_MAX_CASES][CNET_HELDOUT_ID_MAX];
    size_t case_off[CNET_HELDOUT_MAX_CASES];
    size_t case_len[CNET_HELDOUT_MAX_CASES];
    unsigned reads[CNET_HELDOUT_MAX_CASES];
    signed char verdict[CNET_HELDOUT_MAX_CASES]; /* -1 unset, 0 fail, 1 pass */
    size_t errors;           /* unresolved reads / unknown case ids */
} CnetHeldOut;

/* Load the fixture named by CNET_HELD_OUT_FIXTURE and require it to declare
 * `capability_id`. Returns 0 when a fixture was loaded, 1 when none was
 * requested (standalone mode), -1 when one was requested but is unusable — the
 * caller MUST exit non-zero on -1 rather than fall back to its own cases. */
int cnet_heldout_open(CnetHeldOut *h, const char *capability_id);
void cnet_heldout_close(CnetHeldOut *h);

/* Read a case field. In standalone mode the fallback is returned untouched. In
 * bound mode an unresolved read is recorded as an error and the fallback is
 * still returned so the evaluator can continue and report every problem. */
double cnet_heldout_num(CnetHeldOut *h, const char *case_id, const char *key,
                        double fallback);
const char *cnet_heldout_str(CnetHeldOut *h, const char *case_id,
                             const char *key, char *out, size_t cap,
                             const char *fallback);
size_t cnet_heldout_array_len(CnetHeldOut *h, const char *case_id,
                              const char *key, size_t fallback);

/* Record this case's verdict. Calling it twice for one case keeps the worst. */
void cnet_heldout_verdict(CnetHeldOut *h, const char *case_id, int ok);

/* Print the receipt and report whether the fixture was honoured. Returns 0 in
 * standalone mode (nothing printed) and in bound mode only when every declared
 * case was read, every read resolved, and every verdict passed. */
int cnet_heldout_finish(CnetHeldOut *h);

#ifdef __cplusplus
}
#endif
#endif
