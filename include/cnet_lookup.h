#ifndef CNET_LOOKUP_H
#define CNET_LOOKUP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* web_lookup_v1 — typed fetch hop, not residual speech.
   Input: URL + bind kind. Output: bound scalar + provenance, or abstain.
   Not an ASI-5 compete unit. Not an MTK cartridge. */

#define CNET_LOOKUP_CONTRACT "web_lookup_v1"
#define CNET_LOOKUP_BODY_MAX (256u * 1024u)

typedef enum {
    CNET_LOOKUP_BIND_INTEGER = 0,
    CNET_LOOKUP_BIND_TOKEN = 1,
    CNET_LOOKUP_BIND_LINE = 2,
    CNET_LOOKUP_BIND_YEAR = 3
} CnetLookupBind;

typedef struct {
    char url[512];
    char host[128];
    char sha256[65];
    char value[160];
    char snippet[256];
    char refusal[80];
    long http_status;
    size_t bytes;
    int bound;
} CnetLookupReport;

/* 0 bound, 1 abstain, <0 execution error. */
int cnet_lookup_execute(const char *url, CnetLookupBind bind,
                        CnetLookupReport *report);

/* Native speech from a bound report. Refuses unbound reports. */
int cnet_lookup_speak(const CnetLookupReport *report, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
