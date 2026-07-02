#ifndef PROPERTY_H
#define PROPERTY_H

#include <stddef.h>

#include "nn.h"
#include "router.h"

/* An equational property: a NAMED LAW defined by data -- a typed source
   signature plus two chains of primitive names. It holds iff, for every
   canonical member of the enumerated source domain, strictly executing
   the LHS chain equals strictly executing the RHS chain (RHS empty =
   identity on the sources). Laws state what exemplar contracts cannot:
   relations BETWEEN primitives -- and they are the regression net for
   retraining (a broken combine violates split-after-combine = identity
   even if nobody wrote its exemplar table). */

#define PROPERTY_NAME_MAX 64   /* atoms over [A-Za-z0-9_], like tags */
#define PROPERTY_MAX_SOURCES BTN_MAX_INPUT_PORTS
#define PROPERTY_MAX_STEPS 8

typedef struct {
    char name[PROPERTY_NAME_MAX];
    Port sources[PROPERTY_MAX_SOURCES];
    size_t source_count;
    char lhs[PROPERTY_MAX_STEPS][PROPERTY_NAME_MAX];
    size_t lhs_len;                      /* >= 1 */
    char rhs[PROPERTY_MAX_STEPS][PROPERTY_NAME_MAX];
    size_t rhs_len;                      /* 0 = identity on the sources */
} Property;

/* Persist / restore ("CNET_PROPERTY 1"). Fixed-size struct: nothing to
   free. property_load validates magic+version, atom names, source count
   in [1, PROPERTY_MAX_SOURCES], chain lengths in bounds (LHS >= 1),
   known families, nonzero widths/counts, tags via port_set_tag. Returns
   0, or -1 on malformed input (*p untouched). */
int property_save(const Property *p, const char *path);
int property_load(Property *p, const char *path);

typedef struct {
    size_t inputs;     /* domain members checked */
    size_t held;       /* both sides clean and equal */
    size_t violated;   /* unclean handoff on either side, or mismatch */
} PropertyReport;

/* Check a law against a registry: (1) RESOLVE every chain name (first
   strcmp match wins); (2) STATIC TYPE GATE -- sources -> first step,
   step -> step (port counts equal, position-wise port_compatible), and
   LHS final vs RHS final equal in REPRESENTATION (family/width/count;
   tags may differ -- the law equates values, not labels); (3) REPLAY --
   enumerate the canonical source domain (RAW or over-max_samples
   refused) and run BOTH chains per input with strict validate-then-
   canonicalize at every handoff; an unclean value on either side, or a
   final canonical mismatch, counts that input as a violation. Never
   aborts early: the report shows how broken a broken law is. The chain
   runner sits on btn_forward, which records nothing -- checking is
   stateless. Returns 0 iff the replay ran with zero violations; -1
   otherwise (resolve/gate refusals leave report.inputs at 0). */
int property_check(const Property *p, const PrimitiveRegistry *reg,
                   size_t max_samples, PropertyReport *report);

#endif
