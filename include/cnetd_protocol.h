#ifndef CNETD_PROTOCOL_H
#define CNETD_PROTOCOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNETD_REQUEST_MAX 8192
#define CNETD_PEER_MAX 64

typedef enum {
    CNETD_REQUEST_ASK = 1,
    CNETD_REQUEST_PING = 2,
    CNETD_REQUEST_STATUS = 3,
    CNETD_REQUEST_QUIT = 4
} CnetdRequestKind;

typedef struct {
    CnetdRequestKind kind;
    int json;
    char query[CNETD_REQUEST_MAX];
    char peer[CNETD_PEER_MAX];
} CnetdRequest;

typedef enum {
    CNETD_READ_OK = 0,
    CNETD_READ_EOF = 1,
    CNETD_READ_TIMEOUT = 2,
    CNETD_READ_TOO_LONG = 3,
    CNETD_READ_INCOMPLETE = 4,
    CNETD_READ_ERROR = 5
} CnetdReadResult;

/* Reads exactly one newline-terminated request under one monotonic deadline. */
CnetdReadResult cnetd_read_request(int fd, char *out, size_t cap,
                                   int timeout_ms);

/* Parses ASK/PEER/control/JSON strictly. JSON asks accept an optional "peer"
 * string (< CNETD_PEER_MAX bytes, no ASCII whitespace/control characters).
 * Peer is request-local display context, never authorization or CERT evidence. */
int cnetd_parse_request(const char *line, CnetdRequest *out,
                        char *error, size_t error_cap);

#ifdef __cplusplus
}
#endif

#endif
