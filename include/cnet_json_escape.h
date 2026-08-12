/* cnet_json_escape.h -- the ONE JSON string escaper.
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * Three hand-rolled escapers had diverged, and each was wrong in a different
 * way:
 *
 *   src/cnet_route_log.c  correct (this implementation, promoted here)
 *   src/cnet_fault.c      returned void; `continue`d past control characters,
 *                         SILENTLY DELETING them from the learning loop's
 *                         record; truncated on overflow without telling anyone
 *   tools/cnetd.c         escaped 2 of ~12 string fields and passed every C0
 *                         byte through verbatim, so a raw 0x01 in a model reply
 *                         emitted a JSON document no strict parser accepts
 *
 * A raw control byte inside a JSON string is invalid per RFC 8259 (%x20-21 /
 * %x23-5B / %x5D-10FFFF), so the cnetd bug is a real integrity bug and not a
 * cosmetic one: the daemon can emit a response its own client cannot parse.
 *
 * Header-only and `static inline` on purpose. Adding a .c would mean editing
 * the explicit source lists of hundreds of Makefile targets, and a link error
 * in an unrelated target is a worse outcome than a few duplicated inline
 * bodies.
 *
 * CONTRACT. Writes the BODY of a JSON string -- callers supply the surrounding
 * quotes. Returns 0 on success, -1 if the result would not fit, in which case
 * dst is set to "" rather than left holding a half-escaped fragment (a partial
 * escape is how you emit `"\` and break the whole document).
 */
#ifndef CNET_JSON_ESCAPE_H
#define CNET_JSON_ESCAPE_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static inline int cnet_json_escape(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    const unsigned char *p;
    if (!dst || cap == 0) return -1;
    if (!src) src = "";
    for (p = (const unsigned char *)src; *p; ++p) {
        char esc[7];
        size_t n = 0;
        if (*p == '"' || *p == '\\') {
            esc[0] = '\\'; esc[1] = (char)*p; n = 2;
        } else if (*p == '\n') {
            esc[0] = '\\'; esc[1] = 'n'; n = 2;
        } else if (*p == '\r') {
            esc[0] = '\\'; esc[1] = 'r'; n = 2;
        } else if (*p == '\t') {
            esc[0] = '\\'; esc[1] = 't'; n = 2;
        } else if (*p < 0x20) {
            /* Any other C0 byte. NOT dropped: deleting it would silently alter
               the payload the caller believes it recorded. */
            snprintf(esc, sizeof esc, "\\u%04x", (unsigned)*p);
            n = 6;
        } else {
            esc[0] = (char)*p; n = 1;
        }
        if (o + n + 1 > cap) {
            dst[0] = '\0';
            return -1;
        }
        memcpy(dst + o, esc, n);
        o += n;
    }
    dst[o] = '\0';
    return 0;
}

#endif /* CNET_JSON_ESCAPE_H */
