#include "cnet_chat_fluency.h"
#include "cnet_compete_eval.h"
#include "cnet_compete_runtime.h"
#include "cnet_utterance.h"

#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Development probe: same independently authored turns, both backends,
   cnet_chat_fluency_v1 + exact number/abstain. Not an official compete.
   Prints aggregates. Truncates baseline text. Does not write a fixture. */

typedef struct {
    const char *prompt;
    const char *when;
    const char *contract;
    const char *input;
    unsigned expected;
    size_t guards;
    int covered;
} ProbeTurn;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    int failed;
} ResponseBuffer;

static unsigned reference_crc8(unsigned input) {
    unsigned remainder = 0, bit;
    for (bit = 0; bit < 8; ++bit) {
        unsigned message_bit = (input >> (7u - bit)) & 1u;
        unsigned feedback = ((remainder >> 7) & 1u) ^ message_bit;
        remainder = (remainder << 1) & 255u;
        if (feedback != 0) remainder ^= 0x07u;
    }
    return remainder;
}

static size_t receive_response(char *ptr, size_t size, size_t nmemb,
                               void *userdata) {
    ResponseBuffer *buffer = (ResponseBuffer *)userdata;
    size_t add = size * nmemb;
    char *grown;
    if (buffer->failed) return 0;
    if (buffer->length + add + 1u > buffer->capacity) {
        size_t cap = buffer->capacity ? buffer->capacity * 2u : 4096u;
        while (cap < buffer->length + add + 1u) cap *= 2u;
        grown = (char *)realloc(buffer->data, cap);
        if (grown == NULL) {
            buffer->failed = 1;
            return 0;
        }
        buffer->data = grown;
        buffer->capacity = cap;
    }
    memcpy(buffer->data + buffer->length, ptr, add);
    buffer->length += add;
    buffer->data[buffer->length] = '\0';
    return add;
}

static int number_token(const char *text, unsigned value) {
    char needle[32], *hit;
    size_t n;
    snprintf(needle, sizeof needle, "%u", value);
    n = strlen(needle);
    for (hit = strstr(text, needle); hit != NULL; hit = strstr(hit + 1, needle)) {
        int left = (hit == text) || !isalnum((unsigned char)hit[-1]);
        int right = !isalnum((unsigned char)hit[n]);
        if (left && right) return 1;
    }
    return 0;
}

static int ask_baseline(CURL *curl, const char *prompt, char *spoken,
                        size_t cap) {
    ResponseBuffer response;
    struct curl_slist *headers = NULL;
    char *body = NULL, *esc_prompt = NULL, *esc_model = NULL;
    char extracted[2048];
    size_t pcap, mcap, bcap;
    int written, rc = -1;
    long status = 0;
    memset(&response, 0, sizeof response);
    pcap = strlen(prompt) * 6u + 1u;
    mcap = strlen(CNET_COMPETE_BASELINE_MODEL) * 2u + 1u;
    esc_prompt = (char *)malloc(pcap);
    esc_model = (char *)malloc(mcap);
    bcap = pcap + mcap + 512u;
    body = (char *)malloc(bcap);
    if (esc_prompt == NULL || esc_model == NULL || body == NULL ||
        cnet_compete_eval_json_escape(prompt, esc_prompt, pcap) != 0 ||
        cnet_compete_eval_json_escape(CNET_COMPETE_BASELINE_MODEL, esc_model,
                                      mcap) != 0)
        goto done;
    written = snprintf(body, bcap,
                       "{\"model\":\"%s\",\"messages\":["
                       "{\"role\":\"system\",\"content\":\"Answer the user "
                       "request.\"},"
                       "{\"role\":\"user\",\"content\":\"%s\"}],"
                       "\"temperature\":0,\"max_tokens\":128,\"seed\":20260814,"
                       "\"stream\":false}",
                       esc_model, esc_prompt);
    if (written < 0 || (size_t)written >= bcap) goto done;
    headers = curl_slist_append(NULL, "Content-Type: application/json");
    if (headers == NULL) goto done;
    curl_easy_reset(curl);
    if (curl_easy_setopt(curl, CURLOPT_URL, CNET_COMPETE_BASELINE_ENDPOINT) !=
            CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POST, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_response) !=
            CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 180000L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_PROXY, "") != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "*") != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK)
        goto done;
    if (curl_easy_perform(curl) != CURLE_OK ||
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK ||
        status != 200 || response.failed || response.data == NULL)
        goto done;
    if (cnet_compete_eval_extract_chat_content(
            response.data, CNET_COMPETE_BASELINE_MODEL, extracted,
            sizeof extracted) != 0) {
        /* llama.cpp sometimes echoes a short model name; take first content. */
        const char *key = strstr(response.data, "\"content\":\"");
        size_t o = 0;
        if (key == NULL) goto done;
        key += 11;
        while (*key != '\0' && *key != '"' && o + 1u < cap) {
            if (*key == '\\' && key[1] != '\0') ++key;
            spoken[o++] = *key++;
        }
        spoken[o] = '\0';
        if (o == 0) goto done;
        rc = 0;
        goto done;
    }
    snprintf(spoken, cap, "%s", extracted);
    rc = 0;
done:
    if (headers != NULL) curl_slist_free_all(headers);
    free(response.data);
    free(body);
    free(esc_prompt);
    free(esc_model);
    return rc;
}

int main(int argc, char **argv) {
    ProbeTurn turns[8];
    CnetCompeteRuntime *runtime = NULL;
    CnetCompeteRuntimeReport report;
    CnetUtterState state;
    CURL *curl = NULL;
    size_t n = 0, i;
    size_t cnet_exact = 0, base_exact = 0, cnet_rubric = 0, base_rubric = 0;
    double cnet_sum = 0.0, base_sum = 0.0;
    unsigned crc12;
    int rc = 1;

    if (argc != 4) {
        fprintf(stderr, "usage: %s MODEL META CAPSULES\n", argv[0]);
        return 2;
    }
    crc12 = reference_crc8(12u);
    turns[n++] = (ProbeTurn){"Can you increment unsigned byte 12?", "increment",
                             "increment_mod256", "12", 13u, 0u, 1};
    turns[n++] = (ProbeTurn){"Can you increment unsigned byte 41?", "increment",
                             "increment_mod256", "41", 42u, 0u, 1};
    turns[n++] =
        (ProbeTurn){"And now the ATM checksum of operand 12.", "crc", "crc8_atm",
                    "12", crc12, 0u, 1};
    turns[n++] = (ProbeTurn){
        "Quick question: how many seconds are in 15 minutes?", "minutes",
        "minutes_to_seconds", "15", 900u, 0u, 1};
    turns[n++] = (ProbeTurn){
        "In this conversation, decide access admin=true owner=false "
        "mfa=false suspended=false.",
        "policy", "access_policy_v1", "four flags", 1u, 0u, 1};
    turns[n++] = (ProbeTurn){
        "Help me out: increment then double then add three to unsigned byte 5.",
        "compose", "compose3_mod256", "5", 15u, 3u, 1};
    turns[n++] = (ProbeTurn){
        "Hey, increment unsigned byte 12 and email me the result.", "refuse", "",
        "", 0u, 0u, 0};

    if (cnet_compete_runtime_load(argv[1], argv[2], argv[3], &runtime,
                                  &report) != 0)
        return 1;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) goto done;
    curl = curl_easy_init();
    if (curl == NULL) goto done;
    cnet_utter_state_init(&state);
    snprintf(state.source, sizeof state.source, "%s", "CNET");
    printf("CNET_CHAT1_DUAL_PROBE rubric=%s turns=%zu endpoint=%s\n",
           CNET_CHAT_FLUENCY_V1_NAME, n, CNET_COMPETE_BASELINE_ENDPOINT);

    for (i = 0; i < n; ++i) {
        CnetCompeteResult result;
        CnetCompeteDiagnostic diagnostic;
        CnetChatFluencyQuery query;
        CnetChatFluencyScore cnet_score, base_score;
        char cnet_text[CNET_UTTER_TEXT], base_text[2048], preview[97];
        char output_text[32];
        int cnet_ok, base_ok;
        memset(&result, 0, sizeof result);
        memset(&diagnostic, 0, sizeof diagnostic);
        memset(&query, 0, sizeof query);
        cnet_text[0] = base_text[0] = preview[0] = '\0';
        if (cnet_compete_runtime_execute_diagnostic(
                runtime, turns[i].prompt, &result, &diagnostic) != 0)
            goto done;
        if (turns[i].covered) {
            cnet_ok = result.answered && result.value == turns[i].expected &&
                      result.composition_guard_checks == turns[i].guards;
            snprintf(output_text, sizeof output_text, "%u", result.value);
            cnet_utter_state_set(&state, "contract", turns[i].contract);
            cnet_utter_state_set(&state, "input", turns[i].input);
            cnet_utter_state_set(&state, "output", output_text);
            if (cnet_utter_compose_native(&state, turns[i].when, cnet_text,
                                          sizeof cnet_text) != 0)
                goto done;
            query.answered = 1;
            query.when = turns[i].when;
            query.contract = turns[i].contract;
            query.input = turns[i].input;
            query.output = output_text;
        } else {
            cnet_ok = !result.answered;
            if (cnet_utter_compose_native(&state, "refuse", cnet_text,
                                          sizeof cnet_text) != 0)
                goto done;
            query.answered = 0;
            query.when = "refuse";
        }
        cnet_chat_fluency_v1_score(cnet_text, &query, &cnet_score);
        if (cnet_ok) ++cnet_exact;
        if (cnet_chat_fluency_v1_pass(&cnet_score, 1.0f)) ++cnet_rubric;
        cnet_sum += (double)cnet_score.overall;

        if (ask_baseline(curl, turns[i].prompt, base_text, sizeof base_text) !=
            0) {
            snprintf(base_text, sizeof base_text, "%s", "(baseline_error)");
            base_ok = 0;
            memset(&base_score, 0, sizeof base_score);
        } else {
            if (turns[i].covered)
                base_ok = number_token(base_text, turns[i].expected);
            else
                base_ok = !number_token(base_text, 13u) &&
                          (strstr(base_text, "email") == NULL ||
                           strstr(base_text, "cannot") != NULL ||
                           strstr(base_text, "won't") != NULL ||
                           strstr(base_text, "will not") != NULL);
            cnet_chat_fluency_v1_score(base_text, &query, &base_score);
        }
        if (base_ok) ++base_exact;
        if (cnet_chat_fluency_v1_pass(&base_score, 1.0f)) ++base_rubric;
        base_sum += (double)base_score.overall;
        snprintf(preview, sizeof preview, "%.96s", base_text);
        printf("turn=%zu when=%s cnet_exact=%d cnet_rubric=%.3f "
               "base_exact=%d base_rubric=%.3f\n",
               i, turns[i].when, cnet_ok, (double)cnet_score.overall, base_ok,
               (double)base_score.overall);
        printf("  cnet=%s\n  base=%.96s\n", cnet_text, preview);
    }
    printf("CNET_CHAT1_DUAL_PROBE_DONE turns=%zu cnet_exact=%zu/%zu "
           "base_exact=%zu/%zu cnet_rubric_full=%zu/%zu base_rubric_full=%zu/%zu "
           "cnet_rubric_mean=%.3f base_rubric_mean=%.3f rubric=%s "
           "broader_claims=WITHHELD\n",
           n, cnet_exact, n, base_exact, n, cnet_rubric, n, base_rubric, n,
           cnet_sum / (double)n, base_sum / (double)n, CNET_CHAT_FLUENCY_V1_NAME);
    rc = 0;
done:
    if (curl != NULL) curl_easy_cleanup(curl);
    curl_global_cleanup();
    cnet_compete_runtime_free(runtime);
    return rc;
}
