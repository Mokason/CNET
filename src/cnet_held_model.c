#include "cnet_held_model.h"

/* dlopen/dlsym/dlclose come from include/cnet_platform.h, which maps them to
   LoadLibrary/GetProcAddress/FreeLibrary on Windows. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CNET_HAVE_CURL
#include <curl/curl.h>
#endif

#include "cnet_harness.h"
#include "cce/cce_kv_page.h"

static CnetHeldModelHook g_hook;
static char g_endpoint[512];
static char g_path[1024];
static char g_name[256];
static void *g_plugin;
static CnetHarnessSession *g_session;

static void copy_text(char *dst, size_t cap, const char *src) {
    size_t n;
    if (dst == NULL || cap == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void cnet_held_model_set_hook(CnetHeldModelHook hook) { g_hook = hook; }

int cnet_held_model_set_endpoint(const char *url) {
    if (url == NULL || url[0] == '\0') {
        g_endpoint[0] = '\0';
        return 0;
    }
    copy_text(g_endpoint, sizeof g_endpoint, url);
    return 0;
}

int cnet_held_model_set_path(const char *gguf_path) {
    if (gguf_path == NULL || gguf_path[0] == '\0') {
        g_path[0] = '\0';
        return 0;
    }
    copy_text(g_path, sizeof g_path, gguf_path);
    return 0;
}

int cnet_held_model_set_name(const char *model_name) {
    if (model_name == NULL || model_name[0] == '\0') {
        g_name[0] = '\0';
        return 0;
    }
    copy_text(g_name, sizeof g_name, model_name);
    return 0;
}

#if CNET_HAVE_CURL
static size_t json_escape(const char *src, char *dst, size_t cap) {
    size_t o = 0;
    if (dst == NULL || cap == 0) return 0;
    dst[0] = '\0';
    if (src == NULL) return 0;
    for (; *src != '\0' && o + 2u < cap; ++src) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            if (o + 3u >= cap) break;
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c == '\n') {
            if (o + 3u >= cap) break;
            dst[o++] = '\\';
            dst[o++] = 'n';
        } else if (c == '\r' || c == '\t') {
            dst[o++] = ' ';
        } else if (c < 32u) {
            continue;
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
    return o;
}

typedef struct {
    char *data;
    size_t length;
} HeldBody;

static size_t write_body(char *ptr, size_t size, size_t nmemb, void *ud) {
    HeldBody *b = (HeldBody *)ud;
    size_t add = size * nmemb;
    char *grown;
    if (b->length + add > 65536u) return 0;
    grown = (char *)realloc(b->data, b->length + add + 1u);
    if (grown == NULL) return 0;
    b->data = grown;
    memcpy(b->data + b->length, ptr, add);
    b->length += add;
    b->data[b->length] = '\0';
    return add;
}

static int extract_content(const char *json, char *out, size_t cap) {
    const char *key, *src;
    size_t o = 0;
    if (json == NULL || out == NULL || cap == 0) return 1;
    out[0] = '\0';
    key = strstr(json, "\"content\":\"");
    if (key == NULL) return 1;
    src = key + 11;
    while (*src != '\0' && *src != '"' && o + 1u < cap) {
        if (*src == '\\' && src[1] != '\0') {
            ++src;
            if (*src == 'n' || *src == 'r' || *src == 't')
                out[o++] = ' ';
            else
                out[o++] = *src;
            ++src;
            continue;
        }
        out[o++] = *src++;
    }
    out[o] = '\0';
    return o > 0u ? 0 : 1;
}

static int ask_http(const char *url, const char *turn, char *out, size_t cap) {
    CURL *curl;
    struct curl_slist *headers = NULL;
    HeldBody body;
    char esc[1024];
    char payload[1600];
    char model_id[256];
    const char *model_env;
    long status = 0;
    int rc = 1;

    memset(&body, 0, sizeof body);
    /* MAX and similar servers require the real served model id; llama-server
       often accepts a placeholder. Override via set_name or CNET_HELD_MODEL_NAME. */
    if (g_name[0] != '\0')
        copy_text(model_id, sizeof model_id, g_name);
    else {
        model_env = getenv("CNET_HELD_MODEL_NAME");
        if (model_env != NULL && model_env[0] != '\0')
            copy_text(model_id, sizeof model_id, model_env);
        else
            copy_text(model_id, sizeof model_id, "held");
    }
    json_escape(turn, esc, sizeof esc);
    snprintf(payload, sizeof payload,
             "{\"model\":\"%s\",\"messages\":["
             "{\"role\":\"system\",\"content\":\"Answer the user request.\"},"
             "{\"role\":\"user\",\"content\":\"%s\"}],"
             "\"temperature\":0,\"max_tokens\":128,\"stream\":false}",
             model_id, esc);
    curl = curl_easy_init();
    if (curl == NULL) return 1;
    headers = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 120000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 4000L);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (curl_easy_perform(curl) == CURLE_OK &&
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) == CURLE_OK &&
        status == 200 && body.data != NULL)
        rc = extract_content(body.data, out, cap);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body.data);
    return rc;
}
#endif

typedef int (*HeldOpenFn)(const CnetHarnessConfig *, CnetHarnessSession **);
typedef int (*HeldGenFn)(CnetHarnessSession *, const CnetHarnessGenerateOptions *,
                         CnetHarnessGeneration **);
typedef void (*HeldGenFreeFn)(CnetHarnessGeneration *);
typedef int (*HeldCloseFn)(CnetHarnessSession *);

static void *held_sym(void *lib, const char *name) {
    return lib == NULL || name == NULL ? NULL : cnet_dlsym(lib, name);
}

static HeldOpenFn as_open(void *p) {
    union {
        void *o;
        HeldOpenFn f;
    } u;
    u.o = p;
    return u.f;
}

static HeldGenFn as_gen(void *p) {
    union {
        void *o;
        HeldGenFn f;
    } u;
    u.o = p;
    return u.f;
}

static HeldGenFreeFn as_gen_free(void *p) {
    union {
        void *o;
        HeldGenFreeFn f;
    } u;
    u.o = p;
    return u.f;
}

static HeldCloseFn as_close(void *p) {
    union {
        void *o;
        HeldCloseFn f;
    } u;
    u.o = p;
    return u.f;
}

static int plugin_open(const char *path) {
    const char *lib;
    HeldOpenFn open_fn;
    CnetHarnessConfig cfg;

    if (g_session != NULL) return 0;
    lib = getenv("CNET_HARNESS_LIBRARY");
    if (lib == NULL || lib[0] == '\0') lib = "bin/libcnet_harness.so";
    if (g_plugin == NULL) {
        g_plugin = cnet_dlopen(lib);
        if (g_plugin == NULL) return 1;
    }
    open_fn = as_open(held_sym(g_plugin, "cnet_harness_open"));
    if (open_fn == NULL) return 1;
    memset(&cfg, 0, sizeof cfg);
    cfg.abi_version = CNET_HARNESS_ABI_VERSION;
    cfg.struct_size = (uint32_t)sizeof cfg;
    cfg.model_id = "held";
    cfg.model_path = path;
    cfg.resource_mask = CNET_HARNESS_RESOURCE_CPU;
    cfg.budget_bytes = 8ull * 1024ull * 1024ull * 1024ull;
    cfg.main_gpu = 0;
    {
        const char *cx = getenv("CNET_HELD_N_CTX");
        unsigned nctx = 8192u;
        if (cx == NULL || cx[0] == '\0') cx = getenv("CNET_CTX_LEGAL");
        if (cx && cx[0]) {
            unsigned long v = strtoul(cx, NULL, 10);
            if (v >= 256ul && v <= (unsigned long)CNET_CTX_LEGAL_MAX)
                nctx = (unsigned)v;
        }
        cfg.n_ctx = nctx;
    }
    cfg.n_batch = 256;
    cfg.n_threads = 4;
    cfg.aicimo_num_ops = 4;
    cfg.aicimo_base_dim = 8;
    if (open_fn(&cfg, &g_session) != CNET_HARNESS_OK) {
        g_session = NULL;
        return 1;
    }
    return 0;
}

static int ask_plugin(const char *turn, char *out, size_t cap) {
    HeldGenFn gen_fn;
    HeldGenFreeFn free_fn;
    CnetHarnessGenerateOptions opt;
    CnetHarnessGeneration *gen = NULL;
    int rc;

    if (g_session == NULL) return 1;
    if (g_plugin == NULL) return 1;
    gen_fn = as_gen(held_sym(g_plugin, "cnet_harness_generate"));
    free_fn = as_gen_free(held_sym(g_plugin, "cnet_harness_generation_free"));
    if (gen_fn == NULL || free_fn == NULL) return 1;
    memset(&opt, 0, sizeof opt);
    opt.abi_version = CNET_HARNESS_ABI_VERSION;
    opt.struct_size = (uint32_t)sizeof opt;
    opt.system = "Answer the user request.";
    opt.user = turn;
    opt.role = "assistant";
    opt.max_tokens = 128;
    opt.seed = 1;
    opt.sampling = CNET_HARNESS_SAMPLING_DETERMINISTIC;
    rc = gen_fn(g_session, &opt, &gen);
    if (rc != CNET_HARNESS_OK || gen == NULL || gen->text == NULL ||
        gen->text[0] == '\0') {
        if (free_fn) free_fn(gen);
        return 1;
    }
    copy_text(out, cap, gen->text);
    free_fn(gen);
    return 0;
}

void cnet_held_model_close(void) {
    if (g_plugin != NULL && g_session != NULL) {
        HeldCloseFn close_fn = as_close(held_sym(g_plugin, "cnet_harness_close"));
        if (close_fn != NULL) close_fn(g_session);
    }
    g_session = NULL;
    if (g_plugin != NULL) {
        cnet_dlclose(g_plugin);
        g_plugin = NULL;
    }
}

int cnet_held_model_ask(const char *turn, char *out, size_t cap) {
    const char *env_ep;
    const char *env_path;

    if (out == NULL || cap == 0) return -1;
    out[0] = '\0';
    if (turn == NULL || turn[0] == '\0') return 1;
    if (g_hook != NULL) return g_hook(turn, out, cap) == 0 && out[0] != '\0' ? 0 : 1;

    env_ep = getenv("CNET_HELD_MODEL_ENDPOINT");
    env_path = getenv("CNET_HELD_MODEL_PATH");
    if (g_path[0] == '\0' && env_path != NULL && env_path[0] != '\0')
        copy_text(g_path, sizeof g_path, env_path);
    if (g_endpoint[0] == '\0' && env_ep != NULL && env_ep[0] != '\0')
        copy_text(g_endpoint, sizeof g_endpoint, env_ep);

    if (g_path[0] != '\0') {
        if (g_session == NULL && plugin_open(g_path) != 0) {
            /* path set but plugin missing: fall through to HTTP */
        } else if (ask_plugin(turn, out, cap) == 0) {
            return 0;
        }
    }

#if CNET_HAVE_CURL
    if (g_endpoint[0] != '\0')
        return ask_http(g_endpoint, turn, out, cap);
#endif
    return 1;
}
