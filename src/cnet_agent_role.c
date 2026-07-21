/*
 * cnet_agent_role.c — closed cognitive-role vocabulary + policy table.
 *
 * Layer-3 policy only. Does not admit specialists, seal units, or alter
 * certification. Harness wiring uses preferred_sampling when the caller
 * leaves sampling AUTO, and routes AICIMO with the canonical role name.
 */
#include "../include/cnet_agent_role.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ---- policy table (static strings; never free) ---- */

static const CnetAgentRolePolicy k_policies[CNET_AGENT_ROLE_COUNT] = {
    {
        CNET_AGENT_ROLE_AUDITOR,
        "auditor",
        CNET_AGENT_SAMPLE_DETERMINISTIC,
        CNET_AGENT_CAP_INSPECT | CNET_AGENT_CAP_CERTIFY | CNET_AGENT_CAP_RECALL,
        "You are the auditor. Check contracts, evidence, and provenance. "
        "Refuse on mismatch. Report findings; do not invent facts or seal units."
    },
    {
        CNET_AGENT_ROLE_RESEARCHER,
        "researcher",
        CNET_AGENT_SAMPLE_BALANCED,
        CNET_AGENT_CAP_RECALL | CNET_AGENT_CAP_SEARCH | CNET_AGENT_CAP_INSPECT,
        "You are the researcher. Gather sources, open questions, and "
        "evidence. Prefer retrieval over speculation; do not commit changes."
    },
    {
        CNET_AGENT_ROLE_CODER,
        "coder",
        CNET_AGENT_SAMPLE_FOCUSED,
        CNET_AGENT_CAP_CODE | CNET_AGENT_CAP_WRITE | CNET_AGENT_CAP_INSPECT,
        "You are the coder. Propose concrete implementations and patches. "
        "Be precise; stay within stated scope; do not seal or certify."
    },
    {
        CNET_AGENT_ROLE_CRITIC,
        "critic",
        CNET_AGENT_SAMPLE_FOCUSED,
        CNET_AGENT_CAP_INSPECT | CNET_AGENT_CAP_RECALL,
        "You are the critic. Attack plans and answers for failure modes, "
        "gaps, and overclaim. Output findings only; never execute or seal."
    },
    {
        CNET_AGENT_ROLE_MEMORY_WITNESS,
        "memory-witness",
        CNET_AGENT_SAMPLE_DETERMINISTIC,
        CNET_AGENT_CAP_RECALL | CNET_AGENT_CAP_INSPECT,
        "You are the memory-witness. Ground every claim in stored evidence. "
        "Attest what was remembered; never invent; refuse when memory is silent."
    },
};

#define CNET_AGENT_CAP_KNOWN ( \
    CNET_AGENT_CAP_INSPECT | CNET_AGENT_CAP_RECALL | CNET_AGENT_CAP_SEARCH | \
    CNET_AGENT_CAP_CODE | CNET_AGENT_CAP_WRITE | CNET_AGENT_CAP_CERTIFY | \
    CNET_AGENT_CAP_SEAL)

/* Normalize separators: treat '_' as '-' for alias matching only. */
static void normalize_role_key(const char *s, char *out, size_t cap) {
    size_t i = 0;
    if (!s || cap == 0) return;
    for (; s[i] && i + 1 < cap; ++i) {
        char c = s[i];
        if (c == '_') c = '-';
        out[i] = (char)tolower((unsigned char)c);
    }
    out[i] = '\0';
}

int cnet_agent_role_parse(const char *s, CnetAgentRole *out) {
    char key[64];
    if (!s || !s[0] || !out) return -1;
    normalize_role_key(s, key, sizeof key);
    if (key[0] == '\0') return -1;

    if (strcmp(key, "auditor") == 0) {
        *out = CNET_AGENT_ROLE_AUDITOR;
        return 0;
    }
    if (strcmp(key, "researcher") == 0) {
        *out = CNET_AGENT_ROLE_RESEARCHER;
        return 0;
    }
    if (strcmp(key, "coder") == 0) {
        *out = CNET_AGENT_ROLE_CODER;
        return 0;
    }
    if (strcmp(key, "critic") == 0) {
        *out = CNET_AGENT_ROLE_CRITIC;
        return 0;
    }
    if (strcmp(key, "memory-witness") == 0 ||
        strcmp(key, "witness") == 0 ||
        strcmp(key, "memorywitness") == 0) {
        *out = CNET_AGENT_ROLE_MEMORY_WITNESS;
        return 0;
    }
    return -1;
}

const char *cnet_agent_role_name(CnetAgentRole role) {
    if ((unsigned)role >= (unsigned)CNET_AGENT_ROLE_COUNT) return "unknown";
    return k_policies[role].name;
}

int cnet_agent_role_policy(CnetAgentRole role, CnetAgentRolePolicy *out) {
    if (!out) return -1;
    if ((unsigned)role >= (unsigned)CNET_AGENT_ROLE_COUNT) return -1;
    *out = k_policies[role];
    return 0;
}

int cnet_agent_role_resolve(const char *s, CnetAgentRolePolicy *out) {
    CnetAgentRole role;
    if (cnet_agent_role_parse(s, &role) != 0) return -1;
    return cnet_agent_role_policy(role, out);
}

int cnet_agent_role_is_known(const char *s) {
    CnetAgentRole role;
    return cnet_agent_role_parse(s, &role) == 0;
}

char *cnet_agent_role_compose_system(const CnetAgentRolePolicy *policy,
                                     const char *caller_system) {
    size_t frag_len, caller_len, n;
    char *buf;
    if (!policy || !policy->system_fragment) return NULL;
    frag_len = strlen(policy->system_fragment);
    caller_len = (caller_system && caller_system[0]) ? strlen(caller_system) : 0;
    /* fragment + "\n\n" + caller + NUL, or fragment alone */
    n = frag_len + 1u;
    if (caller_len > 0) n += 2u + caller_len;
    buf = (char *)malloc(n);
    if (!buf) return NULL;
    memcpy(buf, policy->system_fragment, frag_len);
    if (caller_len > 0) {
        buf[frag_len] = '\n';
        buf[frag_len + 1] = '\n';
        memcpy(buf + frag_len + 2, caller_system, caller_len);
        buf[frag_len + 2 + caller_len] = '\0';
    } else {
        buf[frag_len] = '\0';
    }
    return buf;
}

int cnet_agent_role_caps_valid(uint32_t mask) {
    return (mask & ~CNET_AGENT_CAP_KNOWN) == 0u;
}
