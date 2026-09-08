#include "cnet_mcp_read_brick.h"
#include "cnet_capsule_core.h"
#include "cnet_mcp_client.h"
#include "cnet_json_escape.h"
#include "cnet_json_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This fixed host ABI, not arbitrary page prose, supplies the capsule labels.
 * The capsule proves finite dispatch only; it cannot grant a new permission. */
enum { MCP_READ_WIKI = 1, MCP_READ_PAGE = 2 };

static const char *command_word(const char *q, const char *word) {
    for (; *word; word++, q++) {
        unsigned char ch = (unsigned char)*q;
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        if (ch != (unsigned char)*word) return NULL;
    }
    /* Non-ASCII separators/controls must not escape into the legacy lookup
     * path. They are claimed here but never accepted as command syntax. */
    return (unsigned char)*q <= ' ' || (unsigned char)*q >= 127 ? q : NULL;
}

static int command_family(const char *q, const char *first, const char *second,
                          const char **argument, int *valid) {
    const char *gap = command_word(q, first), *next, *tail;
    if (!gap || !*gap) return 0;
    *argument = ""; *valid = 0;
    next = gap;
    while (*next && ((unsigned char)*next <= ' ' || (unsigned char)*next >= 127)) {
        /* Excessive separators are terminal; never scan an unbounded prefix. */
        if (next - gap == 16) return 1;
        next++;
    }
    tail = command_word(next, second);
    if (!tail) return 0;
    *valid = next == gap + 1 && *gap == ' ' && *tail == ' ';
    *argument = *tail == ' ' ? tail + 1 : tail;
    return 1;
}

static int refuse(char *out, size_t cap, const char *reason) {
    if (out && cap) {
        int n = snprintf(out, cap,
            "{\"status\":\"refused\",\"trusted\":false,\"certified\":false,\"reason\":\"%s\"}", reason);
        if (n < 0 || (size_t)n >= cap) out[0] = 0;
    }
    return 1;
}

static int argument_json(const char *key, const char *value, size_t max, size_t max_units,
                         char *out, size_t cap) {
    size_t n = 0, units = 0;
    while (n <= max && value[n]) {
        unsigned char ch = (unsigned char)value[n++];
        if (ch < 0x20 || ch == 0x7f) return 0;
        if (ch == 0xc2 && (unsigned char)value[n] >= 0x80 &&
            (unsigned char)value[n] <= 0x9f) return 0;
        if ((ch & 0xc0) != 0x80) units += ch >= 0xf0 ? 2 : 1;
    }
    if (!n || n > max || units > max_units || value[0] == ' ' || value[n - 1] == ' ') return 0;
    char escaped[4100];
    if (cnet_json_escape(value, escaped, sizeof escaped)) return 0;
    int written = snprintf(out, cap, "{\"%s\":\"%s\"}", key, escaped);
    if (written < 0 || (size_t)written >= cap) return 0;
    /* The shared syntax reader also rejects malformed/overlong UTF-8. */
    JsonCursor cursor = {(const unsigned char *)out};
    return !json_value(&cursor, 0) && !*cursor.p;
}

int cnet_mcp_read_brick_ask(const char *q, char *out, size_t cap) {
    unsigned action = 0;
    int valid_command = 0;
    const char *argument = NULL;
    if (out && cap) out[0] = 0;
    if (!q) return 0;
    if (command_family(q, "wiki", "search", &argument, &valid_command)) {
        action = MCP_READ_WIKI;
    } else if (command_family(q, "web", "read", &argument, &valid_command)) {
        action = MCP_READ_PAGE;
    } else return 0;
    if (!out || !cap) return 1;
    const char *enabled = getenv("CNET_MCP_READ_ENABLED");
    if (!enabled || strcmp(enabled, "1")) return refuse(out, cap, "read_disabled");
    char args[4200];
    if (!valid_command || !argument_json(action == MCP_READ_WIKI ? "query" : "url", argument,
                       action == MCP_READ_WIKI ? 800 : 2048,
                       action == MCP_READ_WIKI ? 200 : 2048, args, sizeof args) ||
        (action == MCP_READ_PAGE && (strncmp(argument, "https://", 8) || !argument[8])))
        return refuse(out, cap, "invalid_argument");
    const char *root = getenv("CNET_MCP_READ_CAPSULES");
    if (!root || !root[0]) return refuse(out, cap, "capsule_root_required");
    char error[160], request[96];
    CnetCapsuleCore *core = cnet_capsule_core_open(root, error, sizeof error);
    if (!core) return refuse(out, cap, "capsule_inventory_refused");
    snprintf(request, sizeof request, "capsule mcp_read_action mcp_read_tool %u", action);
    CnetCapsuleCoreReply reply;
    int rc = cnet_capsule_core_ask(core, request, &reply);
    cnet_capsule_core_close(core);
    if (rc || !reply.verified || reply.hops != 1 || reply.value != action ||
        strcmp(reply.units, "mcp_read_dispatch_v1"))
        return refuse(out, cap, "dispatch_capsule_refused");
    const char *tool = action == MCP_READ_WIKI ? "cnet_safe_wiki_search" : "cnet_safe_web_read";
    if (!cnet_mcp_read_call(tool, args, out, cap))
        return refuse(out, cap, "tool_or_evidence_refused");
    return 1;
}
