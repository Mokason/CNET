/*
 * test_cnet_harness_contract.c — hermetic ABI + AICIMO contract test.
 *
 * Exercises the real cnet_harness_core.c compiled into the test binary. No
 * llama.cpp linkage. The plugin's default weak backend hooks fail closed
 * (return ERR_BACKEND), so this file provides STRONG fake backend hooks that
 * override the weak defaults. That override is the ONLY reason the route-
 * only surface can be exercised hermetically.
 *
 * The companion test in tests/test_cnet_harness_failclosed.c links the same
 * cnet_harness_core.c but provides no fakes; it proves that a readable dummy
 * model path still returns ERR_BACKEND and leaves session_out NULL.
 *
 * Proves:
 *   - ABI/struct validation fails closed (bad abi_version, bad struct_size,
 *     NULL, empty strings, out-of-range dims).
 *   - Multi-bit / unknown resource masks are rejected.
 *   - Missing model_path returns CNET_HARNESS_ERR_MODEL_LOAD and does not
 *     leak a session (out pointer stays NULL).
 *   - A real role-biased AICIMO decision through cnet_harness_probe_route
 *     distributes over at least two adapters and never all to adapter 0;
 *     uncertainties stay in [0,1]; the effective_* parameter fields agree
 *     with the effective_sampling profile.
 *   - Distinct profiles carry distinct numeric parameters.
 *   - cnet_harness_error_string returns non-NULL for every documented code
 *     and for an unknown code.
 *   - cnet_harness_generation_free(NULL) and cnet_harness_close(NULL) are
 *     safe no-ops.
 *   - Two-phase backend teardown is invoked in order: prepare_close BEFORE
 *     model release/manager close, finish_close AFTER.
 */
#include "../include/cnet_harness.h"
#include "../src/cnet_harness/cnet_harness_private.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int pass_count = 0;
static int fail_count = 0;

#define CHECK(cond, msg) do {                                       \
    if (cond) { pass_count++; }                                     \
    else { fail_count++; fprintf(stderr, "FAIL: %s\n", msg); }      \
} while (0)

/* ---- Strong fake backend hooks. These override the plugin's weak
 * fail-closed defaults so the route-only surface can be exercised without
 * llama.cpp. The strong linkage is the mechanism the plugin uses in
 * production too — the real llama TU provides these symbols. ---- */

static int fake_open_calls = 0;
static int fake_prepare_close_calls = 0;
static int fake_finish_close_calls = 0;
static int fake_teardown_order_ok = 1;
static int fake_teardown_stage = 0;
static int fake_observed_offload_enabled = 0;
static CnetHarnessOffloadPolicy fake_observed_offload_policy;

int harness_backend_open(struct CnetHarnessSession *session) {
    fake_open_calls++;
    fake_observed_offload_enabled = session->offload_enabled;
    memset(&fake_observed_offload_policy, 0,
           sizeof(fake_observed_offload_policy));
    if (session->offload_enabled) {
        fake_observed_offload_policy = session->offload_policy;
        session->offload_info.applied_gpu_layers =
            session->offload_policy.gpu_layer_count;
        session->offload_info.model_layer_count = 36;
        session->offload_info.device_count =
            session->offload_policy.device_count;
        session->offload_info.device_indices[0] =
            session->offload_policy.device_indices[0];
        session->offload_info.device_indices[1] =
            session->offload_policy.device_indices[1];
        session->offload_info.vram_bytes[0] = 400000000ull;
        session->offload_info.vram_bytes[1] = 420000000ull;
    }
    return CNET_HARNESS_OK;
}

int harness_backend_generate(struct CnetHarnessSession *session,
                              const CnetHarnessGenerateOptions *options,
                              CnetHarnessSamplingMode effective,
                              CnetHarnessSamplingParams params,
                              CnetHarnessGeneration *generation) {
    (void)session; (void)options; (void)effective; (void)params; (void)generation;
    /* The contract test does not exercise the generate() text path; return
     * ERR_BACKEND deliberately so tests that call generate() see the
     * error-handling path. */
    return CNET_HARNESS_ERR_BACKEND;
}

void harness_backend_prepare_close(struct CnetHarnessSession *session) {
    (void)session;
    fake_prepare_close_calls++;
    if (fake_teardown_stage != 0) fake_teardown_order_ok = 0;
    fake_teardown_stage = 1;
}

void harness_backend_finish_close(struct CnetHarnessSession *session) {
    (void)session;
    fake_finish_close_calls++;
    if (fake_teardown_stage != 1) fake_teardown_order_ok = 0;
    fake_teardown_stage = 2;
}

static char temp_path[512];

static void make_temp_model_file(void) {
    const char *base = "/tmp";
    snprintf(temp_path, sizeof(temp_path),
             "%s/cnet_harness_contract_%d.gguf", base, (int)getpid());
    FILE *f = fopen(temp_path, "wb");
    if (!f) {
        fprintf(stderr, "cannot create temp file %s: %s\n",
                temp_path, strerror(errno));
        exit(2);
    }
    /* Contents do not matter for the hermetic path; the core only checks
     * access(R_OK). */
    fwrite("stub", 1u, 4u, f);
    fclose(f);
}

static void remove_temp_model_file(void) {
    if (temp_path[0]) unlink(temp_path);
}

static CnetHarnessConfig valid_config(void) {
    CnetHarnessConfig c;
    memset(&c, 0, sizeof(c));
    c.abi_version = CNET_HARNESS_ABI_VERSION;
    c.struct_size = (uint32_t)sizeof(c);
    c.model_id = "qwythos-9b";
    c.model_path = temp_path;
    c.resource_mask = (uint64_t)CNET_HARNESS_RESOURCE_GPU1;
    c.budget_bytes = 4ull * 1024ull * 1024ull * 1024ull;
    c.main_gpu = 0;
    c.n_ctx = 2048;
    c.n_batch = 512;
    c.n_threads = 8;
    c.aicimo_num_ops = 4;
    c.aicimo_base_dim = 32;
    return c;
}

static void test_abi_validation(void) {
    CnetHarnessConfig c = valid_config();
    CnetHarnessSession *s = (CnetHarnessSession *)0x1;

    CHECK(cnet_harness_open(NULL, &s) == CNET_HARNESS_ERR_INVALID,
          "abi: NULL config rejected");
    CHECK(s == NULL, "abi: NULL config leaves session NULL");

    s = (CnetHarnessSession *)0x1;
    CHECK(cnet_harness_open(&c, NULL) == CNET_HARNESS_ERR_INVALID,
          "abi: NULL session_out rejected");

    s = (CnetHarnessSession *)0x1;
    CnetHarnessConfig bad = c;
    bad.abi_version = 999u;
    CHECK(cnet_harness_open(&bad, &s) == CNET_HARNESS_ERR_INVALID,
          "abi: bad abi_version rejected");
    CHECK(s == NULL, "abi: bad abi_version leaves session NULL");

    s = (CnetHarnessSession *)0x1;
    bad = c;
    bad.struct_size = (uint32_t)(sizeof(bad) - 4u);
    CHECK(cnet_harness_open(&bad, &s) == CNET_HARNESS_ERR_INVALID,
          "abi: bad struct_size rejected");
    CHECK(s == NULL, "abi: bad struct_size leaves session NULL");

    s = (CnetHarnessSession *)0x1;
    bad = c;
    bad.model_id = "";
    CHECK(cnet_harness_open(&bad, &s) == CNET_HARNESS_ERR_INVALID,
          "abi: empty model_id rejected");
    CHECK(s == NULL, "abi: empty model_id leaves session NULL");

    s = (CnetHarnessSession *)0x1;
    bad = c;
    bad.model_path = NULL;
    CHECK(cnet_harness_open(&bad, &s) == CNET_HARNESS_ERR_INVALID,
          "abi: NULL model_path rejected");
    CHECK(s == NULL, "abi: NULL model_path leaves session NULL");

    s = (CnetHarnessSession *)0x1;
    bad = c;
    bad.aicimo_num_ops = 2u;
    CHECK(cnet_harness_open(&bad, &s) == CNET_HARNESS_ERR_INVALID,
          "abi: aicimo_num_ops < 4 rejected");

    s = (CnetHarnessSession *)0x1;
    bad = c;
    bad.n_ctx = 128u;
    CHECK(cnet_harness_open(&bad, &s) == CNET_HARNESS_ERR_INVALID,
          "abi: n_ctx < 256 rejected");

    s = (CnetHarnessSession *)0x1;
    bad = c;
    bad.n_batch = bad.n_ctx + 1u;
    CHECK(cnet_harness_open(&bad, &s) == CNET_HARNESS_ERR_INVALID,
          "abi: n_batch > n_ctx rejected");

    s = (CnetHarnessSession *)0x1;
    bad = c;
    bad.budget_bytes = 0ull;
    CHECK(cnet_harness_open(&bad, &s) == CNET_HARNESS_ERR_INVALID,
          "abi: zero model budget rejected");

    s = (CnetHarnessSession *)0x1;
    bad = c;
    bad.resource_mask = 0ull;
    CHECK(cnet_harness_open(&bad, &s) == CNET_HARNESS_ERR_INVALID,
          "abi: empty resource mask rejected");

    /* Multi-bit mask must be rejected: CPU + GPU0 has two set bits. */
    s = (CnetHarnessSession *)0x1;
    bad = c;
    bad.resource_mask =
        (uint64_t)CNET_HARNESS_RESOURCE_CPU |
        (uint64_t)CNET_HARNESS_RESOURCE_GPU0;
    CHECK(cnet_harness_open(&bad, &s) == CNET_HARNESS_ERR_INVALID,
          "abi: multi-bit resource mask rejected");

    /* Unknown bit not in the documented set must be rejected even if
     * single-bit. */
    s = (CnetHarnessSession *)0x1;
    bad = c;
    bad.resource_mask = 1ull << 20;
    CHECK(cnet_harness_open(&bad, &s) == CNET_HARNESS_ERR_INVALID,
          "abi: unknown resource bit rejected");

    fprintf(stderr, "  test_abi_validation: done\n");
}

static void test_missing_model_returns_model_load(void) {
    CnetHarnessConfig c = valid_config();
    c.model_path = "/definitely/does/not/exist/model.gguf";
    CnetHarnessSession *s = (CnetHarnessSession *)0x1;
    int rc = cnet_harness_open(&c, &s);
    CHECK(rc == CNET_HARNESS_ERR_MODEL_LOAD,
          "missing: MODEL_LOAD returned for absent path");
    CHECK(s == NULL, "missing: session_out stays NULL (no leaked session)");
    fprintf(stderr, "  test_missing_model_returns_model_load: done\n");
}

static void test_role_distribution_via_probe(void) {
    CnetHarnessConfig c = valid_config();
    CnetHarnessSession *s = NULL;
    int rc = cnet_harness_open(&c, &s);
    CHECK(rc == CNET_HARNESS_OK, "route: open ok");
    CHECK(s != NULL, "route: session created");

    static const char *roles[] = {
        "narrative", "analytical", "planner", "critic",
        "coder",     "summarizer", "verifier", "explorer",
        "arbiter",   "empathic",   "trickster", "synthesizer",
    };
    const int nroles = (int)(sizeof(roles) / sizeof(roles[0]));
    int hits[4] = {0, 0, 0, 0};
    int uncertainty_ok = 1;
    int effective_params_ok = 1;

    for (int i = 0; i < nroles; ++i) {
        CnetHarnessRouteInfo info;
        memset(&info, 0, sizeof(info));
        info.abi_version = CNET_HARNESS_ABI_VERSION;
        info.struct_size = (uint32_t)sizeof(info);
        int prc = cnet_harness_probe_route(
            s, roles[i], CNET_HARNESS_SAMPLING_AUTO, &info);
        CHECK(prc == CNET_HARNESS_OK, "route: probe ok");
        if (info.selected_adapter < 4u) hits[info.selected_adapter]++;
        if (!(info.route_uncertainty >= 0.0f && info.route_uncertainty <= 1.0f)) {
            uncertainty_ok = 0;
        }
        CHECK(info.effective_sampling != CNET_HARNESS_SAMPLING_AUTO,
              "route: effective_sampling resolved");
        /* Effective params must agree with the profile (single source of truth). */
        CnetHarnessSamplingParams expected =
            cnet_harness__profile_params(info.effective_sampling);
        if (info.effective_temperature != expected.temperature ||
            info.effective_top_p != expected.top_p ||
            info.effective_top_k != expected.top_k ||
            info.effective_min_p != expected.min_p) {
            effective_params_ok = 0;
        }
    }
    CHECK(effective_params_ok,
          "route: effective params match profile table");

    int distinct = 0;
    for (int i = 0; i < 4; ++i) if (hits[i] > 0) distinct++;
    CHECK(distinct >= 2,
          "route: role probes distribute across >= 2 adapters");
    CHECK(!(hits[0] == nroles),
          "route: not every role collapses to adapter 0");
    CHECK(uncertainty_ok,
          "route: uncertainty always in [0,1]");

    /* Deterministic override: report as override, keep AICIMO metadata. */
    CnetHarnessRouteInfo info;
    memset(&info, 0, sizeof(info));
    info.abi_version = CNET_HARNESS_ABI_VERSION;
    info.struct_size = (uint32_t)sizeof(info);
    int prc = cnet_harness_probe_route(
        s, "narrative", CNET_HARNESS_SAMPLING_DETERMINISTIC, &info);
    CHECK(prc == CNET_HARNESS_OK, "route: probe with override ok");
    CHECK(info.effective_sampling == CNET_HARNESS_SAMPLING_DETERMINISTIC,
          "route: override respected");
    CHECK(info.effective_temperature == 0.0f,
          "route: deterministic reports temp=0 honestly");
    CHECK(info.effective_top_p == 1.0f,
          "route: deterministic reports top_p=1 honestly");
    CHECK(info.effective_top_k == 0u,
          "route: deterministic reports top_k=0 honestly");

    prc = cnet_harness_probe_route(
        s, "narrative", (CnetHarnessSamplingMode)999u, &info);
    CHECK(prc == CNET_HARNESS_ERR_INVALID,
          "route: invalid sampling override rejected");

    /* Bad probe struct rejected. */
    memset(&info, 0, sizeof(info));
    prc = cnet_harness_probe_route(
        s, "narrative", CNET_HARNESS_SAMPLING_AUTO, &info);
    CHECK(prc == CNET_HARNESS_ERR_INVALID,
          "route: unversioned info struct rejected");

    prc = cnet_harness_probe_route(s, NULL, CNET_HARNESS_SAMPLING_AUTO, &info);
    CHECK(prc == CNET_HARNESS_ERR_INVALID || prc == CNET_HARNESS_ERR_STATE,
          "route: NULL role rejected");

    /* Known agent roles: AUTO sampling starts from policy preference
     * (uncertainty may only downgrade). Explicit override still wins. */
    memset(&info, 0, sizeof(info));
    info.abi_version = CNET_HARNESS_ABI_VERSION;
    info.struct_size = (uint32_t)sizeof(info);
    prc = cnet_harness_probe_route(
        s, "auditor", CNET_HARNESS_SAMPLING_AUTO, &info);
    CHECK(prc == CNET_HARNESS_OK, "agent_role: auditor probe ok");
    CHECK(info.effective_sampling == CNET_HARNESS_SAMPLING_DETERMINISTIC,
          "agent_role: auditor prefers DETERMINISTIC");

    memset(&info, 0, sizeof(info));
    info.abi_version = CNET_HARNESS_ABI_VERSION;
    info.struct_size = (uint32_t)sizeof(info);
    prc = cnet_harness_probe_route(
        s, "memory_witness", CNET_HARNESS_SAMPLING_AUTO, &info);
    CHECK(prc == CNET_HARNESS_OK, "agent_role: memory_witness alias ok");
    CHECK(info.effective_sampling == CNET_HARNESS_SAMPLING_DETERMINISTIC,
          "agent_role: memory-witness prefers DETERMINISTIC");

    memset(&info, 0, sizeof(info));
    info.abi_version = CNET_HARNESS_ABI_VERSION;
    info.struct_size = (uint32_t)sizeof(info);
    prc = cnet_harness_probe_route(
        s, "coder", CNET_HARNESS_SAMPLING_EXPLORATORY, &info);
    CHECK(prc == CNET_HARNESS_OK, "agent_role: coder override probe ok");
    CHECK(info.effective_sampling == CNET_HARNESS_SAMPLING_EXPLORATORY,
          "agent_role: explicit sampling override wins over coder policy");

    /* Reset teardown-order tracker and observe. */
    fake_teardown_stage = 0;
    fake_teardown_order_ok = 1;
    fake_prepare_close_calls = 0;
    fake_finish_close_calls = 0;

    int cc = cnet_harness_close(s);
    CHECK(cc == CNET_HARNESS_OK, "route: close ok");
    CHECK(fake_prepare_close_calls == 1,
          "teardown: prepare_close called exactly once");
    CHECK(fake_finish_close_calls == 1,
          "teardown: finish_close called exactly once");
    CHECK(fake_teardown_order_ok,
          "teardown: prepare_close ran before finish_close");

    fprintf(stderr,
            "  test_role_distribution_via_probe: done "
            "(distinct=%d hits={%d,%d,%d,%d})\n",
            distinct, hits[0], hits[1], hits[2], hits[3]);
}

static void test_profile_params_distinct(void) {
    CnetHarnessSamplingParams det =
        cnet_harness__profile_params(CNET_HARNESS_SAMPLING_DETERMINISTIC);
    CnetHarnessSamplingParams foc =
        cnet_harness__profile_params(CNET_HARNESS_SAMPLING_FOCUSED);
    CnetHarnessSamplingParams bal =
        cnet_harness__profile_params(CNET_HARNESS_SAMPLING_BALANCED);
    CnetHarnessSamplingParams exp =
        cnet_harness__profile_params(CNET_HARNESS_SAMPLING_EXPLORATORY);

    CHECK(det.temperature == 0.0f && det.top_p == 1.0f && det.top_k == 0u,
          "profile: deterministic is honest zeros");
    CHECK(foc.temperature != bal.temperature,
          "profile: focused and balanced differ (temperature)");
    CHECK(exp.top_k != bal.top_k,
          "profile: exploratory and balanced differ (top_k)");
    CHECK(foc.top_p != exp.top_p,
          "profile: focused and exploratory differ (top_p)");
    fprintf(stderr, "  test_profile_params_distinct: done\n");
}

static void test_error_strings(void) {
    const int codes[] = {
        CNET_HARNESS_OK,
        CNET_HARNESS_ERR_INVALID,
        CNET_HARNESS_ERR_MODEL_LOAD,
        CNET_HARNESS_ERR_BACKEND,
        CNET_HARNESS_ERR_STATE,
        CNET_HARNESS_ERR_INTERNAL,
        1234567
    };
    for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i) {
        const char *msg = cnet_harness_error_string(codes[i]);
        CHECK(msg != NULL, "err: error_string non-NULL");
        CHECK(strlen(msg) > 0u, "err: error_string non-empty");
    }
    fprintf(stderr, "  test_error_strings: done\n");
}

static void test_safe_null_teardown(void) {
    cnet_harness_generation_free(NULL);
    int rc = cnet_harness_close(NULL);
    CHECK(rc == CNET_HARNESS_OK, "null: close(NULL) returns OK");
    fprintf(stderr, "  test_safe_null_teardown: done\n");
}

static void test_generate_options_validated(void) {
    CnetHarnessConfig c = valid_config();
    CnetHarnessSession *s = NULL;
    int rc = cnet_harness_open(&c, &s);
    CHECK(rc == CNET_HARNESS_OK, "gen_opts: open ok");
    CHECK(s != NULL, "gen_opts: session created");

    CnetHarnessGenerateOptions o;
    memset(&o, 0, sizeof(o));
    o.abi_version = CNET_HARNESS_ABI_VERSION;
    o.struct_size = (uint32_t)sizeof(o);
    o.system = "You are a coherent assistant.";
    o.user = "Hello";
    o.role = "planner";
    o.max_tokens = 32;
    o.seed = 424242;
    o.sampling = CNET_HARNESS_SAMPLING_AUTO;

    CnetHarnessGeneration *gen = (CnetHarnessGeneration *)0x1;
    int grc = cnet_harness_generate(s, &o, &gen);
    /* The fake backend_generate returns ERR_BACKEND, but the ABI validation
     * and AICIMO decision must have run successfully before the backend
     * hook: therefore ERR_BACKEND is the expected code (never INVALID). */
    CHECK(grc == CNET_HARNESS_ERR_BACKEND,
          "gen_opts: fake backend returns ERR_BACKEND");
    CHECK(gen == NULL, "gen_opts: no leaked generation on failure");

    CnetHarnessGenerateOptions bad = o;
    bad.abi_version = 999u;
    CHECK(cnet_harness_generate(s, &bad, &gen) == CNET_HARNESS_ERR_INVALID,
          "gen_opts: bad abi_version rejected");

    bad = o;
    bad.user = NULL;
    CHECK(cnet_harness_generate(s, &bad, &gen) == CNET_HARNESS_ERR_INVALID,
          "gen_opts: NULL user rejected");

    bad = o;
    bad.role = "";
    CHECK(cnet_harness_generate(s, &bad, &gen) == CNET_HARNESS_ERR_INVALID,
          "gen_opts: empty role rejected");

    bad = o;
    bad.max_tokens = 0;
    CHECK(cnet_harness_generate(s, &bad, &gen) == CNET_HARNESS_ERR_INVALID,
          "gen_opts: max_tokens=0 rejected");

    bad = o;
    bad.sampling = (CnetHarnessSamplingMode)99;
    CHECK(cnet_harness_generate(s, &bad, &gen) == CNET_HARNESS_ERR_INVALID,
          "gen_opts: bad sampling enum rejected");

    cnet_harness_close(s);
    fprintf(stderr, "  test_generate_options_validated: done\n");
}

static CnetHarnessOffloadPolicy valid_offload_policy(void) {
    CnetHarnessOffloadPolicy p;
    memset(&p, 0, sizeof(p));
    p.abi_version = CNET_HARNESS_OFFLOAD_ABI_VERSION;
    p.struct_size = (uint32_t)sizeof(p);
    p.gpu_layer_count = 8;
    p.device_count = 2;
    p.device_indices[0] = 0;
    p.device_indices[1] = 1;
    p.tensor_split[0] = 1.0f;
    p.tensor_split[1] = 1.0f;
    p.split_mode = CNET_HARNESS_SPLIT_LAYER;
    p.offload_kqv = 1u;
    p.max_vram_bytes_per_device = 2ull * 1024ull * 1024ull * 1024ull;
    return p;
}

static void test_additive_offload_policy(void) {
    CnetHarnessConfig c = valid_config();
    CnetHarnessSession *s = NULL;

    fake_observed_offload_enabled = -1;
    CHECK(cnet_harness_open(&c, &s) == CNET_HARNESS_OK,
          "offload: legacy open remains valid");
    CHECK(fake_observed_offload_enabled == 0,
          "offload: legacy open carries no policy");
    cnet_harness_close(s);

    CnetHarnessOffloadPolicy p = valid_offload_policy();
    s = NULL;
    CHECK(cnet_harness_open_with_offload(&c, &p, &s) == CNET_HARNESS_OK,
          "offload: additive open accepts valid bounded policy");
    CHECK(s != NULL && fake_observed_offload_enabled == 1,
          "offload: backend observes enabled policy");
    CHECK(fake_observed_offload_policy.gpu_layer_count == 8 &&
          fake_observed_offload_policy.device_count == 2 &&
          fake_observed_offload_policy.device_indices[0] == 0 &&
          fake_observed_offload_policy.device_indices[1] == 1 &&
          fake_observed_offload_policy.tensor_split[0] == 1.0f &&
          fake_observed_offload_policy.tensor_split[1] == 1.0f &&
          fake_observed_offload_policy.max_vram_bytes_per_device ==
              2ull * 1024ull * 1024ull * 1024ull,
          "offload: policy copied exactly before backend open");

    CnetHarnessOffloadInfo info;
    memset(&info, 0, sizeof(info));
    info.abi_version = CNET_HARNESS_OFFLOAD_ABI_VERSION;
    info.struct_size = (uint32_t)sizeof(info);
    CHECK(cnet_harness_get_offload_info(s, &info) == CNET_HARNESS_OK,
          "offload: introspection succeeds");
    CHECK(info.applied_gpu_layers == 8 && info.model_layer_count == 36 &&
          info.device_count == 2 && info.device_indices[0] == 0 &&
          info.device_indices[1] == 1 && info.vram_bytes[0] == 400000000ull &&
          info.vram_bytes[1] == 420000000ull,
          "offload: introspection projects applied policy and residency");
    info.struct_size--;
    CHECK(cnet_harness_get_offload_info(s, &info) == CNET_HARNESS_ERR_INVALID,
          "offload: introspection rejects bad struct size");
    cnet_harness_close(s);

    CnetHarnessOffloadPolicy bad = p;
    bad.abi_version++;
    s = (CnetHarnessSession *)0x1;
    CHECK(cnet_harness_open_with_offload(&c, &bad, &s) ==
              CNET_HARNESS_ERR_INVALID && s == NULL,
          "offload: bad policy ABI rejected before allocation");

    bad = p;
    bad.gpu_layer_count = 0;
    CHECK(cnet_harness_open_with_offload(&c, &bad, &s) ==
              CNET_HARNESS_ERR_INVALID,
          "offload: zero layer count rejected");

    bad = p;
    bad.device_indices[1] = bad.device_indices[0];
    CHECK(cnet_harness_open_with_offload(&c, &bad, &s) ==
              CNET_HARNESS_ERR_INVALID,
          "offload: duplicate devices rejected");

    bad = p;
    bad.split_mode = CNET_HARNESS_SPLIT_NONE;
    CHECK(cnet_harness_open_with_offload(&c, &bad, &s) ==
              CNET_HARNESS_ERR_INVALID,
          "offload: multi-device NONE split rejected");

    bad = p;
    bad.tensor_split[0] = NAN;
    CHECK(cnet_harness_open_with_offload(&c, &bad, &s) ==
              CNET_HARNESS_ERR_INVALID,
          "offload: non-finite tensor split rejected");

    bad = p;
    bad.offload_kqv = 2u;
    CHECK(cnet_harness_open_with_offload(&c, &bad, &s) ==
              CNET_HARNESS_ERR_INVALID,
          "offload: non-boolean KQV flag rejected");

    CnetHarnessConfig cpu = c;
    cpu.resource_mask = CNET_HARNESS_RESOURCE_CPU;
    CHECK(cnet_harness_open_with_offload(&cpu, &p, &s) ==
              CNET_HARNESS_ERR_INVALID,
          "offload: CPU resource plus GPU policy rejected");

    fprintf(stderr, "  test_additive_offload_policy: done\n");
}

/* Real routing log: mechanism, selected expert, entropy, outcome, latency, cost. */
static void test_route_decision_log(void) {
    char log_path[] = "tmp_harness_route_XXXXXX";
    int fd = mkstemp(log_path);
    CHECK(fd >= 0, "route_log: mkstemp");
    if (fd >= 0) close(fd);
    unlink(log_path);
    setenv("CNET_ROUTE_LOG", log_path, 1);

    CnetHarnessConfig c = valid_config();
    CnetHarnessSession *s = NULL;
    int rc = cnet_harness_open(&c, &s);
    CHECK(rc == CNET_HARNESS_OK && s != NULL, "route_log: open ok");

    CnetHarnessRouteInfo info;
    memset(&info, 0, sizeof(info));
    info.abi_version = CNET_HARNESS_ABI_VERSION;
    info.struct_size = (uint32_t)sizeof(info);
    int prc = cnet_harness_probe_route(
        s, "coder", CNET_HARNESS_SAMPLING_AUTO, &info);
    CHECK(prc == CNET_HARNESS_OK, "route_log: probe coder ok");

    CnetHarnessGenerateOptions o;
    memset(&o, 0, sizeof(o));
    o.abi_version = CNET_HARNESS_ABI_VERSION;
    o.struct_size = (uint32_t)sizeof(o);
    o.user = "implement route log";
    o.role = "coder";
    o.max_tokens = 16;
    o.seed = 1;
    o.sampling = CNET_HARNESS_SAMPLING_AUTO;
    CnetHarnessGeneration *gen = (CnetHarnessGeneration *)0x1;
    int grc = cnet_harness_generate(s, &o, &gen);
    CHECK(grc == CNET_HARNESS_ERR_BACKEND, "route_log: generate hits fake backend");
    CHECK(gen == NULL, "route_log: no generation leak");

    cnet_harness_close(s);
    unsetenv("CNET_ROUTE_LOG");

    FILE *f = fopen(log_path, "r");
    CHECK(f != NULL, "route_log: log file exists");
    int lines = 0;
    int has_mech = 0, has_expert = 0, has_entropy = 0, has_outcome = 0;
    int has_latency = 0, has_cost = 0, has_agent = 0, has_backend = 0;
    char line[1600];
    if (f) {
        while (fgets(line, sizeof line, f)) {
            lines++;
            if (strstr(line, "\"mechanism\"")) has_mech = 1;
            if (strstr(line, "\"selected_expert\"")) has_expert = 1;
            if (strstr(line, "\"entropy\"")) has_entropy = 1;
            if (strstr(line, "\"outcome\"")) has_outcome = 1;
            if (strstr(line, "route_latency_ms")) has_latency = 1;
            if (strstr(line, "\"cost\"")) has_cost = 1;
            if (strstr(line, "aicimo_agent_role")) has_agent = 1;
            if (strstr(line, "err_backend")) has_backend = 1;
        }
        fclose(f);
    }
    CHECK(lines >= 2, "route_log: at least probe+generate lines");
    CHECK(has_mech, "route_log: mechanism field");
    CHECK(has_expert, "route_log: selected_expert field");
    CHECK(has_entropy, "route_log: entropy field");
    CHECK(has_outcome, "route_log: outcome field");
    CHECK(has_latency, "route_log: latency field");
    CHECK(has_cost, "route_log: cost field");
    CHECK(has_agent, "route_log: coder uses aicimo_agent_role");
    CHECK(has_backend, "route_log: generate outcome err_backend");
    unlink(log_path);
    fprintf(stderr, "  test_route_decision_log: done (lines=%d)\n", lines);
}

int main(void) {
    printf("=== CNET harness contract test ===\n");
    make_temp_model_file();

    test_abi_validation();
    test_missing_model_returns_model_load();
    test_role_distribution_via_probe();
    test_profile_params_distinct();
    test_error_strings();
    test_safe_null_teardown();
    test_generate_options_validated();
    test_additive_offload_policy();
    test_route_decision_log();

    remove_temp_model_file();

    printf("\nResults: %d passed, %d failed\n", pass_count, fail_count);
    if (fail_count > 0) {
        printf("CNET_HARNESS_CONTRACT_TEST_FAIL\n");
        return 1;
    }
    printf("CNET_HARNESS_CONTRACT_TEST_PASS\n");
    return 0;
}
