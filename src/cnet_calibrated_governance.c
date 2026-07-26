#include "cnet_calibrated_governance.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CNET_GOVERNANCE_CONFIG_MAX (64u * 1024u)

static void governance_copy(char *dst, size_t dst_size, const char *src) {
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int route_id_valid(const char *route_id) {
    const unsigned char *p = (const unsigned char *)route_id;
    if (!p || !*p) return 0;
    while (*p) {
        if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.') return 0;
        p++;
    }
    return 1;
}

static const char *object_key(const char *begin, const char *end,
                              const char *key) {
    char pattern[80];
    const char *found;
    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) < 0) return NULL;
    found = strstr(begin, pattern);
    return found && found < end ? found + strlen(pattern) : NULL;
}

static const char *skip_colon_space(const char *cursor, const char *end) {
    while (cursor < end && isspace((unsigned char)*cursor)) cursor++;
    if (cursor >= end || *cursor != ':') return NULL;
    cursor++;
    while (cursor < end && isspace((unsigned char)*cursor)) cursor++;
    return cursor < end ? cursor : NULL;
}

static int object_string(const char *begin, const char *end, const char *key,
                         char *output, size_t output_capacity) {
    const char *cursor = object_key(begin, end, key);
    size_t n = 0;
    if (!cursor || !(cursor = skip_colon_space(cursor, end)) ||
        *cursor++ != '"') return -1;
    while (cursor < end && *cursor != '"') {
        if (*cursor == '\\' || (unsigned char)*cursor < 0x20 ||
            n + 1 >= output_capacity) return -1;
        output[n++] = *cursor++;
    }
    if (cursor >= end || *cursor != '"') return -1;
    output[n] = '\0';
    return 0;
}

static int object_double(const char *begin, const char *end, const char *key,
                         double *output) {
    const char *cursor = object_key(begin, end, key);
    char *number_end;
    double value;
    if (!cursor || !(cursor = skip_colon_space(cursor, end))) return -1;
    errno = 0;
    value = strtod(cursor, &number_end);
    while (number_end < end && isspace((unsigned char)*number_end)) number_end++;
    if (errno || number_end == cursor || number_end > end ||
        (number_end < end && *number_end != ',' && *number_end != '}') ||
        !isfinite(value))
        return -1;
    *output = value;
    return 0;
}

static int object_size(const char *begin, const char *end, const char *key,
                       size_t *output) {
    const char *cursor = object_key(begin, end, key);
    char *number_end;
    unsigned long long value;
    if (!cursor || !(cursor = skip_colon_space(cursor, end)) || *cursor == '-')
        return -1;
    errno = 0;
    value = strtoull(cursor, &number_end, 10);
    while (number_end < end && isspace((unsigned char)*number_end)) number_end++;
    if (errno || number_end == cursor || number_end > end ||
        (number_end < end && *number_end != ',' && *number_end != '}') ||
        value > (unsigned long long)SIZE_MAX) return -1;
    *output = (size_t)value;
    return 0;
}

static int policy_valid(const CnetRoutePolicy *policy) {
    return policy && route_id_valid(policy->route_id) &&
           isfinite(policy->minimum_margin) &&
           policy->minimum_margin >= 0.0 &&
           isfinite(policy->minimum_reliability) &&
           policy->minimum_reliability >= 0.0 &&
           policy->minimum_reliability <= 1.0 &&
           policy->minimum_samples > 0;
}

static uint64_t binding_hash(uint64_t hash, const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        hash ^= (uint64_t)*p++;
        hash *= UINT64_C(1099511628211);
    }
    hash ^= 0xffu;
    return hash * UINT64_C(1099511628211);
}

static int binding_text_valid(const char *text, size_t maximum) {
    const unsigned char *p = (const unsigned char *)text;
    size_t n = 0;
    if (!p || !*p) return 0;
    while (*p) {
        if (*p < 0x20 || ++n >= maximum) return 0;
        p++;
    }
    return 1;
}

static int json_append(char *output, size_t capacity, size_t *used,
                       const char *text) {
    const size_t n = strlen(text);
    if (*used + n >= capacity) return -1;
    memcpy(output + *used, text, n);
    *used += n;
    output[*used] = '\0';
    return 0;
}

static int json_append_escaped(char *output, size_t capacity, size_t *used,
                               const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    if (json_append(output, capacity, used, "\"") != 0) return -1;
    while (*p) {
        char escaped[3] = {(char)*p, '\0', '\0'};
        if (*p == '"' || *p == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char)*p;
        } else if (*p < 0x20) {
            return -1;
        }
        if (json_append(output, capacity, used, escaped) != 0) return -1;
        p++;
    }
    return json_append(output, capacity, used, "\"");
}

CnetGovernanceDecision cnet_governance_decide(
    const CnetRoutePolicy *policy,
    double margin,
    double reliability,
    size_t sample_count) {
    if (!policy_valid(policy) || !isfinite(margin) ||
        !isfinite(reliability) || margin < 0.0 ||
        reliability < 0.0 || reliability > 1.0) {
        return CNET_GOVERNANCE_ABSTAIN;
    }
    if (margin < policy->minimum_margin ||
        reliability < policy->minimum_reliability ||
        sample_count < policy->minimum_samples) {
        return CNET_GOVERNANCE_ABSTAIN;
    }
    return CNET_GOVERNANCE_ANSWER;
}

int cnet_governance_load_json(const char *path,
                              CnetRoutePolicy *policies,
                              size_t policy_capacity,
                              size_t *out_policy_count) {
    FILE *file;
    char *document;
    long length;
    const char *cursor;
    size_t count = 0, i;
    int closed = 0;
    if (out_policy_count) *out_policy_count = 0;
    if (!path || !policies || policy_capacity == 0 || !out_policy_count)
        return -1;
    file = fopen(path, "rb");
    if (!file) return -2;
    if (fseek(file, 0, SEEK_END) != 0 ||
        (length = ftell(file)) < 0 ||
        (unsigned long)length > CNET_GOVERNANCE_CONFIG_MAX ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return -3;
    }
    document = (char *)malloc((size_t)length + 1);
    if (!document) {
        fclose(file);
        return -4;
    }
    if (fread(document, 1, (size_t)length, file) != (size_t)length) {
        free(document);
        fclose(file);
        return -3;
    }
    document[length] = '\0';
    fclose(file);
    cursor = strstr(document, "\"routes\"");
    if (!cursor || !(cursor = strchr(cursor, '['))) {
        free(document);
        return -5;
    }
    cursor++;
    while (*cursor) {
        const char *begin, *end;
        while (*cursor && (isspace((unsigned char)*cursor) ||
               *cursor == ',')) cursor++;
        if (*cursor == ']') {
            closed = 1;
            break;
        }
        if (*cursor != '{' || count >= policy_capacity) {
            free(document);
            return -5;
        }
        begin = cursor;
        end = strchr(begin, '}');
        if (!end || object_string(begin, end, "route_id",
                                  policies[count].route_id,
                                  sizeof(policies[count].route_id)) != 0 ||
            object_double(begin, end, "minimum_margin",
                          &policies[count].minimum_margin) != 0 ||
            object_double(begin, end, "minimum_reliability",
                          &policies[count].minimum_reliability) != 0 ||
            object_size(begin, end, "minimum_samples",
                        &policies[count].minimum_samples) != 0 ||
            !policy_valid(&policies[count])) {
            free(document);
            return -5;
        }
        for (i = 0; i < count; i++) {
            if (strcmp(policies[i].route_id,
                       policies[count].route_id) == 0) {
                free(document);
                return -5;
            }
        }
        count++;
        cursor = end + 1;
    }
    if (!closed) {
        free(document);
        return -5;
    }
    free(document);
    if (count == 0) return -5;
    *out_policy_count = count;
    return 0;
}

const CnetRoutePolicy *cnet_governance_find_policy(
    const CnetRoutePolicy *policies,
    size_t policy_count,
    const char *route_id) {
    size_t i;
    if (!policies || !route_id) return NULL;
    for (i = 0; i < policy_count; i++) {
        if (strcmp(policies[i].route_id, route_id) == 0)
            return &policies[i];
    }
    return NULL;
}

int cnet_claim_bind(const char *claim,
                    const char *const *evidence_refs,
                    size_t evidence_count,
                    CnetClaimBinding *binding) {
    uint64_t digest = UINT64_C(1469598103934665603);
    size_t i, j;
    if (!binding_text_valid(claim, CNET_CLAIM_MAX) ||
        !evidence_refs || evidence_count == 0 ||
        evidence_count > CNET_CLAIM_EVIDENCE_MAX || !binding) {
        return -1;
    }
    memset(binding, 0, sizeof(*binding));
    governance_copy(binding->claim, sizeof(binding->claim), claim);
    digest = binding_hash(digest, claim);
    for (i = 0; i < evidence_count; i++) {
        if (!binding_text_valid(evidence_refs[i],
                                CNET_EVIDENCE_REF_MAX)) return -1;
        for (j = 0; j < i; j++) {
            if (strcmp(evidence_refs[j], evidence_refs[i]) == 0) return -1;
        }
        governance_copy(binding->evidence_refs[i],
                        sizeof(binding->evidence_refs[i]),
                        evidence_refs[i]);
        digest = binding_hash(digest, evidence_refs[i]);
    }
    binding->evidence_count = evidence_count;
    binding->binding_digest = digest;
    return 0;
}

int cnet_claim_binding_json(const CnetClaimBinding *binding,
                            char *output,
                            size_t output_capacity) {
    char digest[32];
    size_t used = 0, i;
    if (!binding || !output || output_capacity == 0 ||
        binding->evidence_count == 0 ||
        binding->evidence_count > CNET_CLAIM_EVIDENCE_MAX) return -1;
    output[0] = '\0';
    if (json_append(output, output_capacity, &used, "{\"claim\":") != 0 ||
        json_append_escaped(output, output_capacity, &used,
                            binding->claim) != 0 ||
        json_append(output, output_capacity, &used,
                    ",\"evidence_refs\":[") != 0) return -2;
    for (i = 0; i < binding->evidence_count; i++) {
        if (i && json_append(output, output_capacity, &used, ",") != 0)
            return -2;
        if (json_append_escaped(output, output_capacity, &used,
                                binding->evidence_refs[i]) != 0) return -2;
    }
    (void)snprintf(digest, sizeof(digest), "%016llx",
                   (unsigned long long)binding->binding_digest);
    if (json_append(output, output_capacity, &used,
                    "],\"binding_digest\":\"") != 0 ||
        json_append(output, output_capacity, &used, digest) != 0 ||
        json_append(output, output_capacity, &used, "\"}") != 0) return -2;
    return (int)used;
}
