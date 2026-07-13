#ifndef CNET_SPECIALIST_KIND_H
#define CNET_SPECIALIST_KIND_H

/* WHICH backend a planner node is — the one shared enum.
 *
 * It lives in its own dependency-free header so the authoritative registry
 * entry (router.h) can store a node's kind AND the Specialist wrap/admit
 * surface (specialist.h) can name it, without a header cycle (specialist.h
 * includes router.h). This is durable LIVE identity: specialist_admit stamps
 * it on the entry at admission and it survives a CNB reopen (native units
 * default to BTN because the enum's zero value is BTN); trust still replays
 * and is never persisted.
 *
 * BTN == 0 so a zero-initialised RegistryEntry defaults to the native matrix
 * kind — the correct default for any entry admitted outside the specialist
 * door (e.g. tests exercising registry_add_certified directly). */
typedef enum {
    SPECIALIST_KIND_BTN    = 0,  /* native matrix primitive */
    SPECIALIST_KIND_CCE    = 1,  /* CCE cascade/model behind the adapter ABI */
    SPECIALIST_KIND_ORACLE = 2   /* oracle/mined unit behind the adapter ABI */
} SpecialistKind;

#endif /* CNET_SPECIALIST_KIND_H */
