/*
 * cce_specialist.h — Specialist uncertainty interface (legacy stub).
 *
 * The previous version of this header declared a function taking a
 * CceSpecialist* — a type that was never defined anywhere in the codebase,
 * making it a compile-time fiction. That declaration and its accompanying
 * stub implementation (src/cce/cce_specialist_uncertainty.c) have been
 * removed.
 *
 * Real specialist uncertainty estimation lives in cce_uncertainty.h, which
 * operates on activation arrays and counterfactual route scores — concrete
 * data, not an undefined opaque type.
 *
 * This file is retained as an empty placeholder so existing #include paths
 * do not break. It intentionally declares nothing.
 */
#ifndef CCE_SPECIALIST_H
#define CCE_SPECIALIST_H

/* Intentionally empty. See cce_uncertainty.h for the real API. */

#endif /* CCE_SPECIALIST_H */