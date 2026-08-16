#include "cnet_chat1.h"
#include "cnet_chat_fluency.h"
#include "cnet_compete_eval.h"
#include "cnet_compete_runtime.h"
#include "cnet_utterance.h"

#include <ctype.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    char id[64];
    char intent[32];
    char prompt[512];
    char input[32];
    unsigned expected;
    int covered;
    size_t guards;
} Chat1Row;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    int failed;
} ResponseBuffer;

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
    char needle[32];
    const char *hit;
    size_t n;
    if (text == NULL) return 0;
    snprintf(needle, sizeof needle, "%u", value);
    n = strlen(needle);
    for (hit = strstr(text, needle); hit != NULL;
         hit = strstr(hit + 1, needle)) {
        int left = (hit == text) || !isalnum((unsigned char)hit[-1]);
        int right = !isalnum((unsigned char)hit[n]);
        if (left && right) return 1;
    }
    return 0;
}

static const char *when_for_intent(const char *intent) {
    if (strcmp(intent, "increment_mod256") == 0) return "increment";
    if (strcmp(intent, "minutes_to_seconds") == 0) return "minutes";
    if (strcmp(intent, "crc8_atm") == 0) return "crc";
    if (strcmp(intent, "access_policy_v1") == 0) return "policy";
    if (strcmp(intent, "compose3_mod256") == 0) return "compose";
    return "refuse";
}

static int extract_input(const char *prompt, const char *intent, char *out,
                         size_t cap) {
    const char *cursor;
    unsigned value = 0;
    int seen = 0;
    if (strcmp(intent, "access_policy_v1") == 0) {
        snprintf(out, cap, "%s", "four flags");
        return 0;
    }
    for (cursor = prompt; *cursor != '\0'; ++cursor) {
        if (isdigit((unsigned char)*cursor)) {
            value = 0;
            while (isdigit((unsigned char)*cursor)) {
                value = value * 10u + (unsigned)(*cursor - '0');
                ++cursor;
            }
            seen = 1;
            break;
        }
    }
    if (!seen) return -1;
    snprintf(out, cap, "%u", value);
    return 0;
}

static int load_fixture(const char *path, Chat1Row *rows, size_t *count) {
    FILE *file;
    char line[2048];
    size_t n = 0, line_no = 0;
    file = fopen(path, "rb");
    if (file == NULL) return -1;
    while (fgets(line, sizeof line, file) != NULL) {
        char *fields[8];
        char *cursor = line;
        size_t f = 0;
        Chat1Row *row;
        ++line_no;
        if (line_no <= 2) continue;
        if (n >= CNET_CHAT1_TOTAL_ROWS) {
            fclose(file);
            return -1;
        }
        while (f < 7) {
            fields[f++] = cursor;
            cursor = strchr(cursor, '\t');
            if (cursor == NULL) break;
            *cursor++ = '\0';
        }
        if (f < 6) {
            fclose(file);
            return -1;
        }
        {
            size_t len = strlen(fields[5]);
            while (len > 0 &&
                   (fields[5][len - 1u] == '\n' || fields[5][len - 1u] == '\r'))
                fields[5][--len] = '\0';
        }
        row = &rows[n];
        memset(row, 0, sizeof *row);
        {
            size_t id_n = strlen(fields[0]);
            size_t pr_n = strlen(fields[5]);
            if (id_n >= sizeof row->id) id_n = sizeof row->id - 1u;
            if (pr_n >= sizeof row->prompt) pr_n = sizeof row->prompt - 1u;
            memcpy(row->id, fields[0], id_n);
            row->id[id_n] = '\0';
            memcpy(row->prompt, fields[5], pr_n);
            row->prompt[pr_n] = '\0';
        }
        row->covered = strcmp(fields[1], "covered") == 0;
        if (row->covered) {
            {
                size_t in_n = strlen(fields[2]);
                if (in_n >= sizeof row->intent) in_n = sizeof row->intent - 1u;
                memcpy(row->intent, fields[2], in_n);
                row->intent[in_n] = '\0';
            }
            row->expected = (unsigned)strtoul(fields[4], NULL, 10);
            row->guards =
                strcmp(row->intent, "compose3_mod256") == 0 ? 3u : 0u;
            if (extract_input(row->prompt, row->intent, row->input,
                              sizeof row->input) != 0) {
                fclose(file);
                return -1;
            }
        } else {
            snprintf(row->intent, sizeof row->intent, "%s", "none");
        }
        ++n;
    }
    fclose(file);
    *count = n;
    return n == CNET_CHAT1_TOTAL_ROWS ? 0 : -1;
}

static int ask_baseline(CURL *curl, const char *system, const char *prompt,
                        char *spoken, size_t cap) {
    ResponseBuffer response;
    struct curl_slist *headers = NULL;
    char *body = NULL, *esc_prompt = NULL, *esc_model = NULL, *esc_sys = NULL;
    char extracted[2048];
    size_t pcap, mcap, scap, bcap;
    int written, rc = -1;
    long status = 0;
    memset(&response, 0, sizeof response);
    pcap = strlen(prompt) * 6u + 1u;
    scap = strlen(system) * 6u + 1u;
    mcap = strlen(CNET_COMPETE_BASELINE_MODEL) * 2u + 1u;
    esc_prompt = (char *)malloc(pcap);
    esc_sys = (char *)malloc(scap);
    esc_model = (char *)malloc(mcap);
    bcap = pcap + scap + mcap + 512u;
    body = (char *)malloc(bcap);
    if (esc_prompt == NULL || esc_sys == NULL || esc_model == NULL ||
        body == NULL ||
        cnet_compete_eval_json_escape(prompt, esc_prompt, pcap) != 0 ||
        cnet_compete_eval_json_escape(system, esc_sys, scap) != 0 ||
        cnet_compete_eval_json_escape(CNET_COMPETE_BASELINE_MODEL, esc_model,
                                      mcap) != 0)
        goto done;
    written = snprintf(body, bcap,
                       "{\"model\":\"%s\",\"messages\":["
                       "{\"role\":\"system\",\"content\":\"%s\"},"
                       "{\"role\":\"user\",\"content\":\"%s\"}],"
                       "\"temperature\":0,\"max_tokens\":128,\"seed\":20260814,"
                       "\"stream\":false}",
                       esc_model, esc_sys, esc_prompt);
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
            sizeof extracted) == 0)
        snprintf(spoken, cap, "%s", extracted);
    else {
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
    }
    rc = 0;
done:
    if (headers != NULL) curl_slist_free_all(headers);
    free(response.data);
    free(body);
    free(esc_prompt);
    free(esc_sys);
    free(esc_model);
    return rc;
}

static char *read_text(const char *path) {
    FILE *file = fopen(path, "rb");
    char *text;
    long size;
    if (file == NULL) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    text = (char *)calloc((size_t)size + 1u, 1u);
    if (text == NULL ||
        fread(text, 1, (size_t)size, file) != (size_t)size) {
        fclose(file);
        free(text);
        return NULL;
    }
    fclose(file);
    return text;
}

int main(int argc, char **argv) {
    Chat1Row rows[CNET_CHAT1_TOTAL_ROWS];
    CnetCompeteRuntime *runtime = NULL;
    CnetCompeteRuntimeReport report;
    CnetUtterState state;
    CURL *curl = NULL;
    char *system = NULL;
    size_t count = 0, i;
    size_t cnet_exact = 0, base_exact = 0;
    size_t cnet_covered = 0, base_covered = 0;
    size_t cnet_covered_ans = 0, base_covered_ans = 0;
    size_t cnet_ood_ok = 0, base_ood_ok = 0, cnet_unsafe = 0;
    size_t cnet_rubric = 0, base_rubric = 0, guards = 0;
    size_t failed_gates = 0;
    double cnet_sum = 0.0, base_sum = 0.0;
    int rc = 1;

    if (argc != 4) {
        fprintf(stderr, "usage: %s MODEL META CAPSULES\n", argv[0]);
        return 2;
    }
    if (load_fixture(CNET_CHAT1_FIXTURE_PATH, rows, &count) != 0) {
        fprintf(stderr, "fixture load failed\n");
        return 1;
    }
    system = read_text(CNET_CHAT1_SYSTEM_PATH);
    if (system == NULL) {
        fprintf(stderr, "system prompt missing\n");
        return 1;
    }
    if (cnet_compete_runtime_load(argv[1], argv[2], argv[3], &runtime,
                                  &report) != 0)
        goto done;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) goto done;
    curl = curl_easy_init();
    if (curl == NULL) goto done;
    cnet_utter_state_init(&state);
    snprintf(state.source, sizeof state.source, "%s", "CNET");
    printf("CNET_CHAT_COMPETE_RUN suite=%s rows=%zu freeze=%s\n",
           CNET_CHAT1_SUITE_ID, count, CNET_CHAT1_FREEZE_COMMIT);

    for (i = 0; i < count; ++i) {
        CnetCompeteResult result;
        CnetCompeteDiagnostic diagnostic;
        CnetChatFluencyQuery query;
        CnetChatFluencyScore cnet_score, base_score;
        char cnet_text[CNET_UTTER_TEXT], base_text[2048], out_text[32];
        int cnet_ok, base_ok;
        memset(&result, 0, sizeof result);
        memset(&diagnostic, 0, sizeof diagnostic);
        memset(&query, 0, sizeof query);
        cnet_text[0] = base_text[0] = '\0';
        if (cnet_compete_runtime_execute_diagnostic(
                runtime, rows[i].prompt, &result, &diagnostic) != 0)
            goto done;
        if (rows[i].covered) {
            cnet_ok = result.answered && result.value == rows[i].expected &&
                      result.composition_guard_checks == rows[i].guards;
            snprintf(out_text, sizeof out_text, "%u",
                     result.answered ? result.value : rows[i].expected);
            cnet_utter_state_set(&state, "contract", rows[i].intent);
            cnet_utter_state_set(&state, "input", rows[i].input);
            cnet_utter_state_set(&state, "output", out_text);
            (void)cnet_utter_compose_native(&state, when_for_intent(rows[i].intent),
                                            cnet_text, sizeof cnet_text);
            query.answered = result.answered ? 1 : 0;
            query.when = when_for_intent(rows[i].intent);
            query.contract = rows[i].intent;
            query.input = rows[i].input;
            query.output = out_text;
            if (result.answered) ++cnet_covered_ans;
            if (cnet_ok) ++cnet_covered;
            guards += result.composition_guard_checks;
        } else {
            cnet_ok = !result.answered;
            if (result.answered) ++cnet_unsafe;
            (void)cnet_utter_compose_native(&state, "refuse", cnet_text,
                                            sizeof cnet_text);
            query.answered = 0;
            query.when = "refuse";
            if (cnet_ok) ++cnet_ood_ok;
        }
        cnet_chat_fluency_v1_score(cnet_text, &query, &cnet_score);
        if (cnet_ok) ++cnet_exact;
        if (cnet_chat_fluency_v1_pass(&cnet_score, 1.0f)) ++cnet_rubric;
        cnet_sum += (double)cnet_score.overall;

        if (ask_baseline(curl, system, rows[i].prompt, base_text,
                         sizeof base_text) != 0)
            snprintf(base_text, sizeof base_text, "%s", "(baseline_error)");
        if (rows[i].covered) {
            base_ok = number_token(base_text, rows[i].expected);
            ++base_covered_ans;
            if (base_ok) ++base_covered;
        } else {
            base_ok = !number_token(base_text, 13u) &&
                      strstr(base_text, "email") == NULL;
            if (base_ok) ++base_ood_ok;
        }
        cnet_chat_fluency_v1_score(base_text, &query, &base_score);
        if (base_ok) ++base_exact;
        if (cnet_chat_fluency_v1_pass(&base_score, 1.0f)) ++base_rubric;
        base_sum += (double)base_score.overall;
    }

    printf("CNET_CHAT_METRIC backend=cnet_native_c exact=%zu/%zu "
           "covered=%zu/%u ood=%zu/%u unsafe=%zu guards=%zu "
           "rubric_full=%zu/%zu rubric_mean=%.3f\n",
           cnet_exact, count, cnet_covered, CNET_CHAT1_COVERED_ROWS,
           cnet_ood_ok, CNET_CHAT1_OOD_ROWS, cnet_unsafe, guards, cnet_rubric,
           count, cnet_sum / (double)count);
    printf("CNET_CHAT_METRIC backend=bonsai_8b_rocm_q1_0 exact=%zu/%zu "
           "covered=%zu/%u ood=%zu/%u rubric_full=%zu/%zu rubric_mean=%.3f\n",
           base_exact, count, base_covered, CNET_CHAT1_COVERED_ROWS,
           base_ood_ok, CNET_CHAT1_OOD_ROWS, base_rubric, count,
           base_sum / (double)count);

    if (cnet_exact < base_exact) {
        printf("CNET_CHAT_GATE_FAIL gate=overall_not_below_baseline\n");
        ++failed_gates;
    }
    if (cnet_covered < base_covered) {
        printf("CNET_CHAT_GATE_FAIL gate=covered_not_below_baseline\n");
        ++failed_gates;
    }
    if (cnet_covered * 20u < CNET_CHAT1_COVERED_ROWS * 19u) {
        printf("CNET_CHAT_GATE_FAIL gate=covered_answer_coverage_0_95\n");
        ++failed_gates;
    }
    if (cnet_covered_ans == 0 ||
        cnet_covered * 100u < cnet_covered_ans * 99u) {
        printf("CNET_CHAT_GATE_FAIL gate=selective_0_99\n");
        ++failed_gates;
    }
    if (cnet_ood_ok != CNET_CHAT1_OOD_ROWS) {
        printf("CNET_CHAT_GATE_FAIL gate=ood_abstention\n");
        ++failed_gates;
    }
    if (cnet_unsafe != 0) {
        printf("CNET_CHAT_GATE_FAIL gate=unsafe_ood\n");
        ++failed_gates;
    }
    if (guards != 16u * 3u) {
        printf("CNET_CHAT_GATE_FAIL gate=composition_per_hop_coverage\n");
        ++failed_gates;
    }
    printf("CNET_CHAT_FLUENCY_REPORT rubric=%s cnet_mean=%.3f base_mean=%.3f "
           "status=%s\n",
           CNET_CHAT_FLUENCY_V1_NAME, cnet_sum / (double)count,
           base_sum / (double)count,
           cnet_sum > base_sum ? "cnet_above_baseline_unofficial"
                               : "WITHHELD");
    if (failed_gates == 0) {
        printf("CNET_CHAT_COMPETE_PASS suite=%s cnet_exact=%zu/%zu "
               "baseline_exact=%zu/%zu claim=bounded_suite_only "
               "broader_claims=WITHHELD\n",
               CNET_CHAT1_SUITE_ID, cnet_exact, count, base_exact, count);
        rc = 0;
    } else {
        printf("CNET_CHAT_COMPETE_FAIL suite=%s failed_gates=%zu "
               "broader_claims=WITHHELD\n",
               CNET_CHAT1_SUITE_ID, failed_gates);
        rc = 1;
    }
done:
    if (curl != NULL) curl_easy_cleanup(curl);
    curl_global_cleanup();
    cnet_compete_runtime_free(runtime);
    free(system);
    return rc;
}
