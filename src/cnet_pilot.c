#include "../include/cnet_pilot.h"

#include <stdlib.h>
#include <string.h>

void cnet_pilot_init(CnetPilot *p) {
    if (!p) return;
    memset(p, 0, sizeof *p);
}

void cnet_pilot_from_env(CnetPilot *p) {
    const char *e;
    if (!p) return;
    e = getenv("CNET_PILOT");
    p->enabled = (e && e[0] == '1') ? 1 : 0;
}

int cnet_pilot_push(CnetPilot *p, int hint_id) {
    int next;
    if (!p || !p->enabled) return 1;
    if (p->count >= CNET_PILOT_RING) {
        /* drop oldest */
        p->tail = (p->tail + 1) % CNET_PILOT_RING;
        p->count--;
    }
    next = p->head;
    p->hints[next] = hint_id;
    p->head = (p->head + 1) % CNET_PILOT_RING;
    p->count++;
    p->recorded++;
    return 0;
}

int cnet_pilot_pop(CnetPilot *p, int *out) {
    if (!p || !out || p->count <= 0) return 0;
    *out = p->hints[p->tail];
    p->tail = (p->tail + 1) % CNET_PILOT_RING;
    p->count--;
    p->drained++;
    return 1;
}

void cnet_pilot_note_hit(CnetPilot *p) {
    if (p) p->hits++;
}
