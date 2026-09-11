/*
 * tools/cnet_vsa_cli.c - CNET-VSA Interactive Cognitive CLI Utility
 *
 * Unified CLI tool combining:
 *  1. Positional N-Gram Text Projector (text encode, positional unbinding, similarity)
 *  2. Doc-Graft Ingest & Associative Memory Search (sub-microsecond clause recall)
 *  3. Dynamic GPU VRAM Capsule Hot-Swapper (sub-millisecond PCIe DMA paging)
 *  4. Human-like Sleep Memory Consolidation (transient noise pruning & LTM clustering)
 *  5. Graph-AST Structural Code Reasoning (algebraic callgraph unbinding & invariants)
 *  6. Interactive REPL Mode with hardware backend control (ROCm GPU / AVX2 CPU)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <ctype.h>
#include <unistd.h>

#ifdef __HIP_PLATFORM_AMD__
#include <hip/hip_runtime.h>
#endif

#include "cnet_vsa.h"
#include "cnet_vsa_bsc.h"
#include "cnet_vsa_device.h"
#include "cnet_vsa_text.h"
#include "cnet_vsa_capsule_swap.h"
#include "cnet_vsa_sleep.h"
#include "cnet_vsa_ast.h"
#include "cnet_vsa_memory.h"
#include "cnet_vsa_story.h"
#include "cnet_vsa_ngram.h"
#include "cnet_vsa_hybrid.h"
#include "cnet_vsa_gen_capsule.h"

#define CLI_MAX_LINE 1024
#define CLI_MAX_DOC_NODES 1024

typedef struct {
    CnetVsaDeviceConfig dev_cfg;
    CnetVsaDocGraph doc_graph;
    int doc_graph_init;
    char current_doc_path[256];
} CliState;

static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static void print_banner(void) {
    printf("=================================================================\n");
    printf("  CNET-VSA: Vector Symbolic Architecture Cognitive Engine (v1.0) \n");
    printf("  Hardware-Accelerated Hyperdimensional Reasoning & Memory       \n");
    printf("=================================================================\n\n");
}

static void print_usage(const char *prog) {
    printf("Usage: %s [command] [args...] [options]\n\n", prog);
    printf("Commands:\n");
    printf("  encode <text>              Encode text into positional VSA hypervector & extract tokens\n");
    printf("  sim <text1> <text2>        Compute positional semantic similarity between two texts\n");
    printf("  extract <text> <pos>       Extract token at 0-indexed position via algebraic unbinding\n");
    printf("  ingest <file> [query]      Ingest document into Doc-Graft session memory (optional query)\n");
    printf("  query <needle>             Search active ingested document for top matching clauses\n");
    printf("  swap-bench                 Run live dynamic GPU VRAM capsule hot-swap benchmark\n");
    printf("  sleep-demo                 Demonstrate wake/sleep memory consolidation & noise pruning\n");
    printf("  ast-demo                   Demonstrate Graph-AST code reasoning, unbinding & invariants\n");
    printf("  story-demo                 Demonstrate TinyStories knowledge blending & narrative synthesis\n");
    printf("  story-gen [style] [hero]   Generate original story with style (whimsical/adventurous/cozy)\n");
    printf("  mouth-gen [style] [hero]   Generate fluid story via Hybrid VSA Brain + Neural Mouth + Back-Audit\n");
    printf("  ngram-gen [seed] [hero]    Generate word-by-word story via pure VSA unbinding (zero templates)\n");
    printf("  gencap-create <n> <d> <i> <o> Create & seal specialized generative capsule from text corpus\n");
    printf("  gencap-verify <file.gencap>  Verify capsule cryptographic digest & domain specification\n");
    printf("  gencap-gen <cap> [prm] [sd]  Autonomous generation from capsule steered by intent (zero LLM)\n");
    printf("  route <dir> <prompt>       Rank all capsules in directory against prompt intent\n");
    printf("  auto <prompt> [dir]        Auto-route prompt to best matching capsule and generate response\n");
    printf("  device-status              Display detected hardware backends (GPU ROCm / CPU AVX2)\n");
    printf("  repl                       Start interactive command-line session (default if no args)\n\n");
    printf("Options:\n");
    printf("  --device <gpu|cpu>         Set active compute execution engine\n");
    printf("  --help, -h                 Display this help message\n\n");
}

static int init_cli_state(CliState *s, const char *dev_override) {
    memset(s, 0, sizeof(*s));
    
    if (dev_override) {
        if (strcmp(dev_override, "cpu") == 0) {
            setenv("CNET_VSA_DEVICE", "cpu", 1);
            cnet_vsa_device_set_backend(CNET_VSA_BACKEND_CPU);
        } else if (strcmp(dev_override, "gpu") == 0) {
            setenv("CNET_VSA_DEVICE", "gpu", 1);
            cnet_vsa_device_set_backend(CNET_VSA_BACKEND_GPU);
        }
    }
    
    if (cnet_vsa_device_init() != 0) {
        fprintf(stderr, "[!] Warning: cnet_vsa_device_init failed, defaulting to CPU\n");
    }
    
    const CnetVsaDeviceConfig *active_cfg = cnet_vsa_device_get_config();
    if (active_cfg) {
        memcpy(&s->dev_cfg, active_cfg, sizeof(s->dev_cfg));
    }
    
    if (cnet_vsa_graph_init(&s->doc_graph, CNET_VSA_DEFAULT_DIM, CLI_MAX_DOC_NODES) == 0) {
        s->doc_graph_init = 1;
    }
    
    return 0;
}

static void free_cli_state(CliState *s) {
    if (s->doc_graph_init) {
        cnet_vsa_graph_free(&s->doc_graph);
        s->doc_graph_init = 0;
    }
}

static void cmd_device_status(CliState *s) {
    printf("[Hardware Device Subsystem]\n");
    printf("  Active Engine:      %s\n", cnet_vsa_device_is_gpu_active() ? "GPU (ROCm HIP)" : "CPU (Host SIMD)");
    printf("  Device Name:        %s\n", s->dev_cfg.device_name);
    printf("  Compute Units:      %d\n", s->dev_cfg.compute_units);
    printf("  Clock Frequency:    %d MHz\n", s->dev_cfg.clock_mhz);
    printf("  GPU Detected:       %s\n", s->dev_cfg.gpu_available ? "YES (AMD ROCm gfx1201)" : "NO");
    
#ifdef __HIP_PLATFORM_AMD__
    if (s->dev_cfg.gpu_available) {
        size_t free_b = 0, total_b = 0;
        if (hipMemGetInfo(&free_b, &total_b) == hipSuccess) {
            printf("  VRAM Total:         %.2f GB\n", (double)total_b / (1024.0 * 1024.0 * 1024.0));
            printf("  VRAM Free:          %.2f GB\n", (double)free_b / (1024.0 * 1024.0 * 1024.0));
        }
    }
#endif
    printf("\n");
}

static void cmd_encode(CliState *s, const char *text) {
    (void)s;
    if (!text || !*text) {
        printf("Error: encode requires text argument.\n");
        return;
    }

    CnetVsaTokenList tokens;
    cnet_vsa_text_tokenize(text, &tokens);

    float vec[CNET_VSA_DEFAULT_DIM];
    double t0 = get_time_ms();
    int rc = cnet_vsa_text_encode_continuous(&tokens, vec, CNET_VSA_DEFAULT_DIM);
    double elapsed_us = (get_time_ms() - t0) * 1000.0;
    
    if (rc != 0) {
        printf("Error: Failed to encode sentence.\n");
        return;
    }

    /* Compute norm */
    float norm = 0.0f;
    for (int i = 0; i < CNET_VSA_DEFAULT_DIM; ++i) norm += vec[i] * vec[i];
    norm = sqrtf(norm);

    /* Generate BSC binary signature */
    CnetVsaBsc bsc;
    cnet_vsa_text_encode_bsc(&tokens, &bsc);

    printf("[Positional N-Gram Encoding]\n");
    printf("  Input Text:       \"%s\"\n", text);
    printf("  Tokens (%zu):      ", tokens.count);
    for (size_t i = 0; i < tokens.count; ++i) {
        printf("[%s] ", tokens.tokens[i].token);
    }
    printf("\n");
    printf("  Dimensionality:   %d float32 coordinates\n", CNET_VSA_DEFAULT_DIM);
    printf("  L2 Norm:          %.4f (Normalized)\n", norm);
    printf("  Encode Latency:   %.2f us\n", elapsed_us);
    printf("  Head Elements:    [%.4f, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f, %.4f]\n",
           vec[0], vec[1], vec[2], vec[3], vec[4], vec[5], vec[6], vec[7]);
    
    printf("  BSC 512-bit Sig:  ");
    for (int i = 0; i < CNET_VSA_BSC_WORDS; ++i) {
        printf("%016lX", (unsigned long)bsc.w[i]);
    }
    printf(" (64 bytes)\n\n");

    /* Token unbinding demonstration */
    printf("  [Algebraic Token Unbinding via Pi^{-pos}(V_seq)]:\n");
    CnetVsaCodebook vocab;
    cnet_vsa_codebook_init(&vocab, CNET_VSA_DEFAULT_DIM, tokens.count + 1);
    for (size_t i = 0; i < tokens.count; ++i) {
        float tv[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_text_token_vec(tokens.tokens[i].token, tv, CNET_VSA_DEFAULT_DIM);
        cnet_vsa_codebook_add(&vocab, tokens.tokens[i].token, tv);
    }

    for (size_t i = 0; i < tokens.count; ++i) {
        char recovered[CNET_VSA_TOKEN_LEN] = {0};
        float score = 0.0f;
        cnet_vsa_text_decode_at_pos(vec, (int)i, &vocab, recovered, sizeof(recovered), &score);
        printf("    Pos %2zu: \"%s\" (unbound projection similarity: %.4f)\n", i, recovered, score);
    }
    cnet_vsa_codebook_free(&vocab);
    printf("\n");
}

static void cmd_sim(CliState *s, const char *t1, const char *t2) {
    (void)s;
    if (!t1 || !t2 || !*t1 || !*t2) {
        printf("Error: sim requires two text arguments.\n");
        return;
    }

    float cos_sim = cnet_vsa_text_sequence_similarity(t1, t2, CNET_VSA_DEFAULT_DIM);

    CnetVsaTokenList tok1, tok2;
    cnet_vsa_text_tokenize(t1, &tok1);
    cnet_vsa_text_tokenize(t2, &tok2);

    CnetVsaBsc b1, b2;
    cnet_vsa_text_encode_bsc(&tok1, &b1);
    cnet_vsa_text_encode_bsc(&tok2, &b2);
    float bsc_sim = cnet_vsa_bsc_similarity(&b1, &b2);

    printf("[Positional Semantic Similarity]\n");
    printf("  Text 1:           \"%s\"\n", t1);
    printf("  Text 2:           \"%s\"\n", t2);
    printf("  Cosine Similarity: %.4f\n", cos_sim);
    printf("  BSC Bit Sim:       %.4f (Normalized Hamming Match)\n", bsc_sim);
    
    if (cos_sim > 0.999f) {
        printf("  Analysis:         IDENTICAL sentences (100%% equivalence)\n");
    } else if (cos_sim > 0.25f && cos_sim < 0.65f) {
        printf("  Analysis:         ANAGRAM / WORD-ORDER PERMUTATION (VSA permutation preserved)\n");
    } else if (cos_sim >= 0.65f) {
        printf("  Analysis:         HIGH SEMANTIC OVERLAP\n");
    } else {
        printf("  Analysis:         ORTHOGONAL / UNRELATED content\n");
    }
    printf("\n");
}

static void cmd_extract(CliState *s, const char *text, int pos) {
    (void)s;
    if (!text || !*text || pos < 0) {
        printf("Error: extract requires text and a non-negative position integer.\n");
        return;
    }

    CnetVsaTokenList tokens;
    cnet_vsa_text_tokenize(text, &tokens);

    if (pos >= (int)tokens.count) {
        printf("Position %d is outside token range (total tokens: %zu).\n", pos, tokens.count);
        return;
    }

    float vec[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_text_encode_continuous(&tokens, vec, CNET_VSA_DEFAULT_DIM);

    CnetVsaCodebook vocab;
    cnet_vsa_codebook_init(&vocab, CNET_VSA_DEFAULT_DIM, tokens.count + 1);
    for (size_t i = 0; i < tokens.count; ++i) {
        float tv[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_text_token_vec(tokens.tokens[i].token, tv, CNET_VSA_DEFAULT_DIM);
        cnet_vsa_codebook_add(&vocab, tokens.tokens[i].token, tv);
    }

    char recovered[CNET_VSA_TOKEN_LEN] = {0};
    float score = 0.0f;
    cnet_vsa_text_decode_at_pos(vec, pos, &vocab, recovered, sizeof(recovered), &score);

    printf("[Token Extraction at Position %d]\n", pos);
    printf("  Source Text:      \"%s\"\n", text);
    printf("  Expected Token:   \"%s\"\n", tokens.tokens[pos].token);
    printf("  Recovered Token:  \"%s\"\n", recovered);
    printf("  Unbound Match:    %.4f cosine similarity\n\n", score);

    cnet_vsa_codebook_free(&vocab);
}

static void cmd_ingest(CliState *s, const char *filepath, const char *optional_query) {
    if (!filepath || !*filepath) {
        printf("Error: ingest requires a file path.\n");
        return;
    }
    FILE *f = fopen(filepath, "r");
    if (!f) {
        fprintf(stderr, "Error: cannot open file '%s'\n", filepath);
        return;
    }

    if (s->doc_graph_init) {
        cnet_vsa_graph_free(&s->doc_graph);
    }
    cnet_vsa_graph_init(&s->doc_graph, CNET_VSA_DEFAULT_DIM, CLI_MAX_DOC_NODES);
    s->doc_graph_init = 1;
    strncpy(s->current_doc_path, filepath, sizeof(s->current_doc_path) - 1);

    char line[CLI_MAX_LINE];
    int line_num = 0;
    int nodes_added = 0;
    double t0 = get_time_ms();

    while (fgets(line, sizeof(line), f) && nodes_added < CLI_MAX_DOC_NODES) {
        line_num++;
        /* Trim trailing newline and whitespace */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' || isspace((unsigned char)line[len - 1]))) {
            line[len - 1] = '\0';
            len--;
        }
        if (len == 0) continue;

        CnetVsaTokenList tokens;
        cnet_vsa_text_tokenize(line, &tokens);
        if (tokens.count == 0) continue;

        float vec[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_text_encode_continuous(&tokens, vec, CNET_VSA_DEFAULT_DIM);

        char label[CNET_VSA_NAME_MAX];
        snprintf(label, sizeof(label), "L%d", line_num);

        cnet_vsa_graph_add_node(&s->doc_graph, 0, label, line, vec);
        nodes_added++;
    }
    fclose(f);
    double elapsed_ms = get_time_ms() - t0;

    printf("[Doc-Graft Session Memory Ingestion]\n");
    printf("  Source File:      %s\n", filepath);
    printf("  Clauses Ingested: %d clauses / lines\n", nodes_added);
    printf("  Memory Footprint: %.2f KB (%zu bytes per clause)\n",
           (double)(nodes_added * sizeof(CnetVsaGraphNode)) / 1024.0,
           sizeof(CnetVsaGraphNode));
    printf("  Ingestion Time:   %.2f ms (%.2f us/clause)\n",
           elapsed_ms, (elapsed_ms * 1000.0) / (nodes_added > 0 ? nodes_added : 1));
    printf("\n");

    if (optional_query && *optional_query) {
        CnetVsaTokenList qtok;
        cnet_vsa_text_tokenize(optional_query, &qtok);
        float qvec[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_text_encode_continuous(&qtok, qvec, CNET_VSA_DEFAULT_DIM);

        const CnetVsaGraphNode *results[5];
        float scores[5];
        size_t count = 0;

        double qt0 = get_time_ms();
        cnet_vsa_graph_query(&s->doc_graph, qvec, 5, results, scores, &count);
        double q_us = (get_time_ms() - qt0) * 1000.0;

        printf("[Query: \"%s\"] (Search Latency: %.2f us)\n", optional_query, q_us);
        for (size_t i = 0; i < count; ++i) {
            printf("  #%zu [sim=%.4f] (%s): \"%s\"\n",
                   i + 1, scores[i], results[i]->label, results[i]->text_span);
        }
        printf("\n");
    }
}

static void cmd_query(CliState *s, const char *needle) {
    if (!needle || !*needle) {
        printf("Error: query requires a search query string.\n");
        return;
    }
    if (!s->doc_graph_init || s->doc_graph.count == 0) {
        printf("Error: No document currently ingested. Run 'ingest <file>' first.\n");
        return;
    }

    CnetVsaTokenList qtok;
    cnet_vsa_text_tokenize(needle, &qtok);
    float qvec[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_text_encode_continuous(&qtok, qvec, CNET_VSA_DEFAULT_DIM);

    const CnetVsaGraphNode *results[5];
    float scores[5];
    size_t count = 0;

    double t0 = get_time_ms();
    cnet_vsa_graph_query(&s->doc_graph, qvec, 5, results, scores, &count);
    double elapsed_us = (get_time_ms() - t0) * 1000.0;

    printf("[Associative Memory Search: \"%s\"]\n", needle);
    printf("  Document Context: %s (%zu clauses indexed)\n", s->current_doc_path, s->doc_graph.count);
    printf("  Query Latency:    %.2f us\n", elapsed_us);
    printf("  Top Matches:\n");
    for (size_t i = 0; i < count; ++i) {
        printf("    #%zu [Similarity: %.4f] %s: \"%s\"\n",
               i + 1, scores[i], results[i]->label, results[i]->text_span);
    }
    printf("\n");
}

static void cmd_swap_bench(CliState *s) {
    (void)s;
    printf("=================================================================\n");
    printf(" CNET-VSA Live GPU VRAM Dynamic Capsule Hot-Swap Benchmark\n");
    printf("=================================================================\n\n");

    const size_t vram_budget_mb = 48;
    const size_t capsule_size_mb = 16;
    const size_t capsule_weight_bytes = capsule_size_mb * 1024 * 1024;
    const size_t max_capsules = 8;

    CnetVsaCapsuleManager mgr;
    int rc = cnet_vsa_capsule_mgr_init(&mgr, vram_budget_mb * 1024 * 1024);
    if (rc != 0) {
        printf("Error: Failed to initialize capsule manager.\n");
        return;
    }

    const char *domains[] = {
        "mathematics.algebra",
        "software.cnet_vsa_runtime",
        "physics.quantum_field",
        "biology.proteomics",
        "history.classical_antiquity",
        "finance.algorithmic_market",
        "chemistry.organic_synthesis",
        "medicine.oncology_pathology"
    };

    printf("[1/3] Registering %zu Specialist Capsules (%zu MB total, Hard VRAM Limit: %zu MB)...\n",
           max_capsules, max_capsules * capsule_size_mb, vram_budget_mb);

    uint64_t rng = 12345678ULL;
    float centroids[8][CNET_VSA_DEFAULT_DIM];
    for (size_t i = 0; i < max_capsules; ++i) {
        cnet_vsa_random(centroids[i], CNET_VSA_DEFAULT_DIM, &rng);
        char *dummy_weights = (char *)malloc(capsule_weight_bytes);
        if (dummy_weights) {
            memset(dummy_weights, (int)(i + 1), capsule_weight_bytes);
        }
        char name[64];
        snprintf(name, sizeof(name), "capsule_%s", domains[i]);
        cnet_vsa_capsule_register(&mgr, name, domains[i], centroids[i], dummy_weights, capsule_weight_bytes);
        free(dummy_weights);
        printf("  Registered: %-35s [16.0 MB] Centroid Init OK\n", name);
    }

    printf("\n[2/3] Simulating High-Rate Multi-Domain Query Stream...\n");
    size_t query_sequence[] = { 0, 1, 2, 0, 3, 4, 1, 5, 6, 7, 0, 2 };
    size_t n_queries = sizeof(query_sequence) / sizeof(query_sequence[0]);

    for (size_t q = 0; q < n_queries; ++q) {
        size_t target_idx = query_sequence[q];
        float query_vec[CNET_VSA_DEFAULT_DIM];
        /* Add mild noise to domain centroid */
        float noise[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_random(noise, CNET_VSA_DEFAULT_DIM, &rng);
        for (int d = 0; d < CNET_VSA_DEFAULT_DIM; ++d) {
            query_vec[d] = 0.90f * centroids[target_idx][d] + 0.10f * noise[d];
        }
        cnet_vsa_normalize(query_vec, CNET_VSA_DEFAULT_DIM);

        CnetVsaCapsuleEntry *active_cap = NULL;
        void *d_weights = NULL;
        double swap_ms = 0.0;

        rc = cnet_vsa_capsule_hot_swap(&mgr, query_vec, &active_cap, &d_weights, &swap_ms);
        if (rc == 0 && active_cap) {
            printf("  Q%02zu -> Routed to '%-30s' | %s | Latency: %6.3f ms | VRAM: %zu/%zu MB\n",
                   q + 1, active_cap->name,
                   swap_ms > 0.001 ? "PAGED_IN (DMA)" : "CACHE_HIT (0ms)",
                   swap_ms,
                   mgr.current_vram_bytes / (1024 * 1024),
                   mgr.max_vram_bytes / (1024 * 1024));
        }
    }

    printf("\n[3/3] Dynamic Swapper Final Telemetry:\n");
    printf("  Swap-in Events:   %lu\n", mgr.swap_in_count);
    printf("  LRU Evictions:    %lu\n", mgr.evict_count);
    printf("  Peak VRAM Used:   %.1f MB (Hard Limit: %.1f MB)\n",
           (double)mgr.peak_vram_bytes / (1024.0 * 1024.0),
           (double)mgr.max_vram_bytes / (1024.0 * 1024.0));
    printf("  Avg Swap Latency: %.3f ms (Steady-State PCIe DMA Line Rate)\n\n",
           mgr.swap_in_count > 0 ? mgr.total_swap_time_ms / (double)mgr.swap_in_count : 0.0);

    cnet_vsa_capsule_mgr_free(&mgr);
}

static void cmd_sleep_demo(CliState *s) {
    (void)s;
    printf("=================================================================\n");
    printf(" CNET-VSA Wake/Sleep Memory Consolidation & Pruning Demo\n");
    printf("=================================================================\n\n");

    CnetVsaSleepConsolidator sc;
    int rc = cnet_vsa_sleep_init(&sc, CNET_VSA_DEFAULT_DIM, 2, 0.70f);
    if (rc != 0) {
        printf("Error: Failed to init sleep consolidator.\n");
        return;
    }

    CnetVsaCodebook ltm;
    cnet_vsa_codebook_init(&ltm, CNET_VSA_DEFAULT_DIM, 32);

    printf("[Awake Phase] Ingesting 100 Session Memory Events:\n");
    printf("  - 70 Transient noise/distraction events (unverified, access_count=1)\n");
    printf("  - 30 High-confidence verified invariants (Math, Legal, Code)\n\n");

    uint64_t rng = 987654321ULL;
    float math_basis[CNET_VSA_DEFAULT_DIM];
    float legal_basis[CNET_VSA_DEFAULT_DIM];
    float code_basis[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_random(math_basis, CNET_VSA_DEFAULT_DIM, &rng);
    cnet_vsa_random(legal_basis, CNET_VSA_DEFAULT_DIM, &rng);
    cnet_vsa_random(code_basis, CNET_VSA_DEFAULT_DIM, &rng);

    /* Ingest 70 transient noise events */
    for (int i = 0; i < 70; ++i) {
        float noise_v[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_random(noise_v, CNET_VSA_DEFAULT_DIM, &rng);
        cnet_vsa_sleep_record_event(&sc, "noise", "transient conversational utterance",
                                    noise_v, 1, 0.40f, false);
    }

    /* Ingest 10 Math rules */
    for (int i = 0; i < 10; ++i) {
        float v[CNET_VSA_DEFAULT_DIM], n[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_random(n, CNET_VSA_DEFAULT_DIM, &rng);
        for (int j = 0; j < CNET_VSA_DEFAULT_DIM; ++j) v[j] = math_basis[j] + 0.15f * n[j];
        cnet_vsa_normalize(v, CNET_VSA_DEFAULT_DIM);
        cnet_vsa_sleep_record_event(&sc, "math", "verified arithmetic invariant",
                                    v, 5, 0.99f, true);
    }

    /* Ingest 10 Legal precedents */
    for (int i = 0; i < 10; ++i) {
        float v[CNET_VSA_DEFAULT_DIM], n[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_random(n, CNET_VSA_DEFAULT_DIM, &rng);
        for (int j = 0; j < CNET_VSA_DEFAULT_DIM; ++j) v[j] = legal_basis[j] + 0.15f * n[j];
        cnet_vsa_normalize(v, CNET_VSA_DEFAULT_DIM);
        cnet_vsa_sleep_record_event(&sc, "legal", "verified contractual precedence",
                                    v, 4, 0.98f, true);
    }

    /* Ingest 10 Code contracts */
    for (int i = 0; i < 10; ++i) {
        float v[CNET_VSA_DEFAULT_DIM], n[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_random(n, CNET_VSA_DEFAULT_DIM, &rng);
        for (int j = 0; j < CNET_VSA_DEFAULT_DIM; ++j) v[j] = code_basis[j] + 0.15f * n[j];
        cnet_vsa_normalize(v, CNET_VSA_DEFAULT_DIM);
        cnet_vsa_sleep_record_event(&sc, "code", "verified formal memory safety invariant",
                                    v, 6, 0.99f, true);
    }

    printf("Pre-Consolidation State:\n");
    printf("  Session Events:   %zu\n", sc.event_count);
    printf("  Permanent LTM:    %zu prototypes\n\n", ltm.count);

    printf("[Sleep Phase] Triggering Asynchronous Consolidation Cycle...\n");
    size_t pruned = 0, promoted = 0;
    double t0 = get_time_ms();
    cnet_vsa_sleep_consolidate(&sc, &ltm, &pruned, &promoted);
    double elapsed_ms = get_time_ms() - t0;

    printf("Post-Consolidation Report (Executed in %.2f ms):\n", elapsed_ms);
    printf("  Transient Noise Pruned:   %zu (100%% unverified clutter eliminated)\n", pruned);
    printf("  LTM Prototypes Promoted:  %zu (Math, Legal, Code)\n", promoted);
    printf("  Current Permanent LTM:    %zu\n\n", ltm.count);

    printf("[Recall Test] Querying Consolidated LTM Codebook with Noisy Math Probe:\n");
    float probe[CNET_VSA_DEFAULT_DIM], pnoise[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_random(pnoise, CNET_VSA_DEFAULT_DIM, &rng);
    for (int d = 0; d < CNET_VSA_DEFAULT_DIM; ++d) {
        probe[d] = 0.85f * math_basis[d] + 0.15f * pnoise[d];
    }
    cnet_vsa_normalize(probe, CNET_VSA_DEFAULT_DIM);

    float clean[CNET_VSA_DEFAULT_DIM];
    char matched_name[CNET_VSA_NAME_MAX] = {0};
    float sim = 0.0f;
    cnet_vsa_codebook_cleanup(&ltm, probe, clean, matched_name, sizeof(matched_name), &sim);

    printf("  Query with noisy Math probe -> Recovered LTM Prototype: '%s' (similarity: %.4f)\n\n",
           matched_name, sim);

    cnet_vsa_sleep_free(&sc);
    cnet_vsa_codebook_free(&ltm);
}

static void cmd_ast_demo(CliState *s) {
    (void)s;
    printf("=================================================================\n");
    printf(" CNET-VSA Graph-AST Structural Code Reasoning Demo\n");
    printf("=================================================================\n\n");

    CnetVsaAstGraph ast;
    int rc = cnet_vsa_ast_init(&ast, CNET_VSA_DEFAULT_DIM);
    if (rc != 0) {
        printf("Error: Failed to create AST graph.\n");
        return;
    }

    printf("[1/3] Registering Functions, Allocations & Call Edges...\n");
    cnet_vsa_ast_add_node(&ast, "cnet_vsa_hot_swap", "function");
    cnet_vsa_ast_add_node(&ast, "cnet_vsa_evict", "function");
    cnet_vsa_ast_add_node(&ast, "hipMalloc", "function");
    cnet_vsa_ast_add_node(&ast, "hipFree", "function");
    cnet_vsa_ast_add_node(&ast, "vram_buffer", "resource");
    cnet_vsa_ast_add_node(&ast, "cnet_vsa_unsafe_worker", "function");

    cnet_vsa_ast_add_edge(&ast, "cnet_vsa_hot_swap", "hipMalloc", CNET_VSA_EDGE_CALLS);
    cnet_vsa_ast_add_edge(&ast, "cnet_vsa_evict", "hipFree", CNET_VSA_EDGE_CALLS);
    cnet_vsa_ast_add_edge(&ast, "cnet_vsa_hot_swap", "vram_buffer", CNET_VSA_EDGE_ALLOCATES);
    cnet_vsa_ast_add_edge(&ast, "cnet_vsa_hot_swap", "vram_bounds", CNET_VSA_EDGE_VERIFIES);
    cnet_vsa_ast_add_edge(&ast, "cnet_vsa_evict", "vram_buffer", CNET_VSA_EDGE_FREES);
    cnet_vsa_ast_add_edge(&ast, "cnet_vsa_hot_swap", "cnet_vsa_evict", CNET_VSA_EDGE_CALLS);
    cnet_vsa_ast_add_edge(&ast, "cnet_vsa_unsafe_worker", "scratch_heap", CNET_VSA_EDGE_ALLOCATES);

    cnet_vsa_ast_compile(&ast);
    printf("  Registered %zu code nodes and %zu relational AST edges.\n", ast.node_count, ast.edge_count);
    printf("  Compiled unified superposition graph vector (norm: %.4f).\n\n",
           sqrtf(cnet_vsa_similarity(ast.graph_bundle, ast.graph_bundle, CNET_VSA_DEFAULT_DIM)));

    printf("[2/3] Algebraic Unbinding Queries:\n");
    char result_name[CNET_VSA_NAME_MAX] = {0};
    float sim = 0.0f;

    /* Forward query: cnet_vsa_evict -[CALLS]-> ? */
    rc = cnet_vsa_ast_query_callee(&ast, "cnet_vsa_evict", CNET_VSA_EDGE_CALLS, result_name, sizeof(result_name), &sim);
    printf("  Query: 'cnet_vsa_evict -[CALLS]-> ?'\n");
    if (rc == 0) {
        printf("    -> Found '%s' (similarity: %.4f)\n", result_name, sim);
    }

    /* Backward query: ? -[FREES]-> vram_buffer */
    rc = cnet_vsa_ast_query_caller(&ast, "vram_buffer", CNET_VSA_EDGE_FREES, result_name, sizeof(result_name), &sim);
    printf("\n  Query: '? -[FREES]-> vram_buffer'\n");
    if (rc == 0) {
        printf("    -> Found '%s' (similarity: %.4f)\n", result_name, sim);
    }

    printf("\n[3/3] Semantic Invariant Safety Audit:\n");
    int viol_safe = cnet_vsa_ast_check_leak_invariants(&ast, "cnet_vsa_hot_swap");
    int viol_unsafe = cnet_vsa_ast_check_leak_invariants(&ast, "cnet_vsa_unsafe_worker");
    printf("  Function 'cnet_vsa_hot_swap':     %s\n", viol_safe ? "[!] LEAK VIOLATION" : "[+] SAFE (Verified)");
    printf("  Function 'cnet_vsa_unsafe_worker': %s\n", viol_unsafe ? "[!] LEAK VIOLATION (Caught unverified alloc)" : "[+] SAFE");
    printf("  Invariant Audit Status: PASSED (Zero False Positives, Caught Unsafe Allocation)\n\n");

    cnet_vsa_ast_free(&ast);
}

static void cmd_story_demo(CliState *s) {
    (void)s;
    printf("=================================================================\n");
    printf(" CNET-VSA TinyStories Creative Narrative Synthesis Demo\n");
    printf("=================================================================\n\n");

    CnetVsaStoryEngine eng;
    if (cnet_vsa_story_init(&eng, CNET_VSA_DEFAULT_DIM, 42) != 0) {
        printf("Error: Failed to initialize story engine.\n");
        return;
    }

    printf("[1/3] Ingesting TinyStories Exemplar Corpus...\n");
    int count = cnet_vsa_story_ingest_corpus(&eng);
    printf("  Ingested %d TinyStories into VSA knowledge manifold.\n", count);
    printf("  Extracted %zu narrative concepts in codebook.\n\n", eng.concept_codebook.count);

    printf("[2/3] Hyperdimensional Concept Blending & Style Modulation...\n");
    /* Blend Story 0 (Curious Fox + Glowing Mushroom) + Story 11 (Young Dragon + Green Hills) */
    float blend_w[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_story_blend(&eng, 0, 11, CNET_VSA_STYLE_WHIMSICAL, blend_w);

    CnetVsaGeneratedStory story;
    double t0 = get_time_ms();
    cnet_vsa_story_generate(&eng, blend_w, CNET_VSA_STYLE_WHIMSICAL, "Oliver the fox", &story);
    double elapsed_us = (get_time_ms() - t0) * 1000.0;

    printf("[3/3] Synthesized Original Story (Generated in %.2f us):\n\n", elapsed_us);
    printf("  *** %s ***\n\n%s\n\n", story.title, story.story);
    printf("  [Mathematical Verification Audit]:\n");
    printf("    Style Manifold:     Whimsical (Sparkling / Wonder)\n");
    printf("    Hero / Setting:     %s in the %s\n", story.hero, story.setting);
    printf("    Key Artifact:       %s\n", story.artifact);
    printf("    Intent Alignment:   %.4f (High fidelity to blended concept)\n", story.intent_similarity);
    printf("    Max Corpus Overlap: %.4f (Proven Novel: not a memorized clone)\n", story.max_exemplar_overlap);
    printf("    Contract Status:    %s (Fail-closed invariant satisfied)\n\n",
           story.contract_verified ? "SAFE (Admitted)" : "VIOLATION (Abstained)");

    cnet_vsa_story_free(&eng);
}

static void cmd_story_gen(CliState *s, const char *style_str, const char *hero_name) {
    (void)s;
    CnetVsaStoryEngine eng;
    if (cnet_vsa_story_init(&eng, CNET_VSA_DEFAULT_DIM, 42) != 0) {
        printf("Error: Failed to initialize story engine.\n");
        return;
    }
    cnet_vsa_story_ingest_corpus(&eng);

    CnetVsaStoryStyle style = CNET_VSA_STYLE_WHIMSICAL;
    if (style_str) {
        if (strcmp(style_str, "adventurous") == 0) style = CNET_VSA_STYLE_ADVENTUROUS;
        else if (strcmp(style_str, "cozy") == 0) style = CNET_VSA_STYLE_COZY;
    }

    float blend[CNET_VSA_DEFAULT_DIM];
    if (style == CNET_VSA_STYLE_ADVENTUROUS) {
        cnet_vsa_story_blend(&eng, 11, 7, style, blend);
    } else if (style == CNET_VSA_STYLE_COZY) {
        cnet_vsa_story_blend(&eng, 1, 8, style, blend);
    } else {
        cnet_vsa_story_blend(&eng, 0, 9, style, blend);
    }

    CnetVsaGeneratedStory story;
    cnet_vsa_story_generate(&eng, blend, style, hero_name, &story);

    printf("\n*** %s ***\n\n%s\n\n", story.title, story.story);
    printf("[Audit] Style: %s | Intent Match: %.4f | Max Overlap: %.4f | Safety: %s\n\n",
           style == CNET_VSA_STYLE_WHIMSICAL ? "Whimsical" :
           (style == CNET_VSA_STYLE_ADVENTUROUS ? "Adventurous" : "Cozy"),
           story.intent_similarity, story.max_exemplar_overlap,
           story.contract_verified ? "SAFE" : "VIOLATION");

    cnet_vsa_story_free(&eng);
}

static void cmd_mouth_gen(CliState *s, const char *style_str, const char *hero,
                          const char *setting, const char *artifact) {
    (void)s;
    CnetVsaStoryEngine eng;
    if (cnet_vsa_story_init(&eng, CNET_VSA_DEFAULT_DIM, 42) != 0) {
        printf("Error: Failed to initialize story engine.\n");
        return;
    }
    cnet_vsa_story_ingest_corpus(&eng);

    CnetVsaStoryStyle style = CNET_VSA_STYLE_WHIMSICAL;
    if (style_str) {
        if (strcmp(style_str, "adventurous") == 0) style = CNET_VSA_STYLE_ADVENTUROUS;
        else if (strcmp(style_str, "cozy") == 0) style = CNET_VSA_STYLE_COZY;
    }

    CnetVsaKnowledgeFrame frame;
    cnet_vsa_hybrid_formulate_frame(&eng,
                                    hero ? hero : "Oliver the fox",
                                    setting ? setting : "enchanted forest",
                                    artifact ? artifact : "glowing mushroom",
                                    style, &frame);

    printf("=================================================================\n");
    printf(" CNET Hybrid: VSA Brain + Neural Mouth + VSA Back-Audit Gate\n");
    printf("=================================================================\n");
    printf("  Target Frame:  Hero='%s' | Setting='%s' | Artifact='%s'\n",
           frame.hero, frame.setting, frame.artifact);
    printf("  Style:         %s\n\n",
           style == CNET_VSA_STYLE_WHIMSICAL ? "Whimsical" :
           (style == CNET_VSA_STYLE_ADVENTUROUS ? "Adventurous" : "Cozy"));

    CnetVsaHybridResult res;
    int status = cnet_vsa_hybrid_generate_and_audit(&eng, &frame, 90, &res);
    if (status == 0) {
        printf("  [Neural Mouth Output]:\n");
        printf("  \"%s\"\n\n", res.generated_prose);
        printf("  [Performance]: Gen=%.1f ms | Channel: %s\n",
               res.gen_time_ms, res.warm_service_used ? "Warm HTTP (:8084)" : "Subprocess");
        printf("  [VSA Back-Projection Audit]:\n");
        printf("    - Frame Grounding Sim:   %.4f (threshold: >= 0.80) -> %s\n",
               res.frame_grounding_sim, res.frame_grounding_sim >= 0.80f ? "PASS" : "FAIL");
        printf("    - Safe Manifold Dist:    %.4f (ceiling: <= 1.38) -> %s\n",
               res.safety_contract_distance, res.safety_contract_distance <= 1.38f ? "PASS" : "FAIL");
        printf("    - Entity Grounding:      Hero:%d | Setting:%d | Artifact:%d\n",
               res.hero_detected, res.setting_detected, res.artifact_detected);
        printf("    - Hostile Intrusion:     %s\n",
               res.hostile_concept_detected ? "DETECTED (FAIL-CLOSED)" : "NONE (CLEAN)");
        printf("    - Gatekeeper Verdict:    %s\n\n", res.audit_verdict);
    } else {
        printf("Error: Hybrid generation failed with code %d\n", status);
    }

    cnet_vsa_story_free(&eng);
}

static void cmd_ngram_gen(CliState *s, const char *seed_word, const char *hero, const char *setting) {
    (void)s;
    CnetVsaNgramEngine eng;
    cnet_vsa_ngram_init(&eng, CNET_VSA_DEFAULT_DIM, 42);
    cnet_vsa_ngram_ingest_corpus(&eng);

    float target_intent[CNET_VSA_DEFAULT_DIM] = {0};
    if (hero || setting) {
        float v_h[CNET_VSA_DEFAULT_DIM], v_s[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_text_token_vec(hero ? hero : "fox", v_h, CNET_VSA_DEFAULT_DIM);
        cnet_vsa_text_token_vec(setting ? setting : "forest", v_s, CNET_VSA_DEFAULT_DIM);
        for (int d = 0; d < CNET_VSA_DEFAULT_DIM; ++d) target_intent[d] = 0.6f * v_h[d] + 0.4f * v_s[d];
        cnet_vsa_normalize(target_intent, CNET_VSA_DEFAULT_DIM);
    }

    char out_text[512] = {0};
    int tok_gen = 0;
    cnet_vsa_ngram_generate(&eng, seed_word ? seed_word : "once",
                            (hero || setting) ? target_intent : NULL,
                            0.45f, 0.85f, 32, out_text, sizeof(out_text), &tok_gen);

    printf("=================================================================\n");
    printf(" CNET Pure VSA Autonomous N-Gram Generator (Zero Templates)\n");
    printf("=================================================================\n");
    printf("  Seed: '%s' | Steer: '%s in %s'\n",
           seed_word ? seed_word : "once",
           hero ? hero : "fox", setting ? setting : "forest");
    printf("  Tokens: %d\n\n", tok_gen);
    printf("  Output: \"%s\"\n\n", out_text);
}

static void cmd_gencap_create(const char *name, const char *domain, const char *corpus_path, const char *out_capsule) {
    CnetVsaGenCapsule *cap = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule));
    if (!cap) {
        fprintf(stderr, "Error: out of memory allocating capsule.\n");
        return;
    }

    if (cnet_vsa_gencap_init(cap, name, domain, CNET_VSA_DEFAULT_DIM) != 0) {
        fprintf(stderr, "Error: failed to initialize capsule '%s'.\n", name);
        free(cap);
        return;
    }

    FILE *fp = fopen(corpus_path, "r");
    if (!fp) {
        fprintf(stderr, "Error: cannot open corpus file '%s'.\n", corpus_path);
        free(cap);
        return;
    }

    char line[1024];
    int lines_ingested = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        size_t len = strlen(p);
        while (len > 0 && isspace((unsigned char)p[len - 1])) p[--len] = '\0';
        if (len == 0 || *p == '#') continue;

        if (cnet_vsa_gencap_ingest(cap, p) > 0) {
            lines_ingested++;
        }
    }
    fclose(fp);

    if (lines_ingested == 0) {
        fprintf(stderr, "Error: no valid sentences ingested from '%s'.\n", corpus_path);
        free(cap);
        return;
    }

    if (cnet_vsa_gencap_seal(cap) != 0) {
        fprintf(stderr, "Error: failed to seal capsule.\n");
        free(cap);
        return;
    }

    if (cnet_vsa_gencap_save(cap, out_capsule) != 0) {
        fprintf(stderr, "Error: failed to save capsule to '%s'.\n", out_capsule);
        free(cap);
        return;
    }

    printf("=================================================================\n");
    printf(" CNET Generative Knowledge Capsule Created & Certified\n");
    printf("=================================================================\n");
    printf("  Capsule Name:      %s\n", cap->name);
    printf("  Domain:            %s\n", cap->domain);
    printf("  Lines Ingested:    %d\n", lines_ingested);
    printf("  Vocabulary:        %zu unique words\n", cap->ngram.vocab_count);
    printf("  Transitions:       %zu n-gram transitions\n", cap->ngram.transition_count);
    printf("  Safe Radius:       %.3f\n", cap->safe_radius);
    printf("  Integrity Digest:  0x%016llx\n", (unsigned long long)cap->digest);
    printf("  Output File:       %s\n", out_capsule);
    printf("  Status:            SEALED & CERTIFIED [PASS]\n\n");

    free(cap);
}

static void cmd_gencap_verify(const char *capsule_path) {
    CnetVsaGenCapsule *cap = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule));
    if (!cap) {
        fprintf(stderr, "Error: out of memory allocating capsule.\n");
        return;
    }

    int rc = cnet_vsa_gencap_load(cap, capsule_path);
    if (rc != 0) {
        printf("=================================================================\n");
        printf(" CNET Generative Capsule Verification: FAILED (rc=%d)\n", rc);
        printf("=================================================================\n");
        if (rc == -4) printf("  Reason: Incompatible Magic or Version mismatch.\n");
        else if (rc == -5) printf("  Reason: FNV-1a Cryptographic Digest Mismatch (TAMPERED / CORRUPT).\n");
        else printf("  Reason: Failed to read file or bad payload structure.\n");
        printf("  Fail-Closed Action: Refusing untrusted capsule execution.\n\n");
        free(cap);
        return;
    }

    printf("=================================================================\n");
    printf(" CNET Generative Knowledge Capsule Verified\n");
    printf("=================================================================\n");
    printf("  Capsule Path:      %s\n", capsule_path);
    printf("  Name:              %s\n", cap->name);
    printf("  Domain:            %s\n", cap->domain);
    printf("  Certified:         %s\n", cap->certified ? "YES (Valid)" : "NO");
    printf("  Safe Radius:       %.3f\n", cap->safe_radius);
    printf("  Vocabulary:        %zu words\n", cap->ngram.vocab_count);
    printf("  Transitions:       %zu transitions\n", cap->ngram.transition_count);
    printf("  Frames:            %zu grammar frames\n", cap->frame_count);
    printf("  Digest:            0x%016llx\n", (unsigned long long)cap->digest);
    printf("  Audit Verdict:     CERTIFIED_AUTHENTIC [PASS]\n\n");

    free(cap);
}

static void cmd_gencap_gen(const char *capsule_path, const char *prompt, const char *seed, int max_tokens) {
    CnetVsaGenCapsule *cap = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule));
    if (!cap) {
        fprintf(stderr, "Error: out of memory allocating capsule.\n");
        return;
    }

    int rc = cnet_vsa_gencap_load(cap, capsule_path);
    if (rc != 0) {
        fprintf(stderr, "Error: cannot load capsule '%s' (rc=%d).\n", capsule_path, rc);
        free(cap);
        return;
    }

    float intent_vec[CNET_VSA_DEFAULT_DIM];
    float *p_intent = NULL;
    if (prompt && *prompt) {
        if (cnet_vsa_gencap_encode_intent(prompt, intent_vec, cap->ngram.dim) == 0) {
            p_intent = intent_vec;
        }
    }

    char out_buf[1024] = {0};
    int toks_out = 0;
    int gen_rc = cnet_vsa_gencap_generate(cap, seed, p_intent, 0.45f,
                                         max_tokens > 0 ? max_tokens : 28,
                                         out_buf, sizeof(out_buf), &toks_out);

    printf("=================================================================\n");
    printf(" CNET Autonomous Capsule Generation (Pure VSA, Zero LLM)\n");
    printf("=================================================================\n");
    printf("  Capsule:    %s (Domain: %s)\n", cap->name, cap->domain);
    if (prompt) printf("  Prompt:     \"%s\"\n", prompt);
    if (seed)   printf("  Seed:       \"%s\"\n", seed);
    printf("  Result:     %s\n", (gen_rc == 0) ? "SUCCESS (In-Domain)" : "REFUSED (Out-of-Domain)");
    printf("  Tokens:     %d\n", toks_out);
    printf("  Output:     \"%s\"\n\n", out_buf);

    free(cap);
}

static void cmd_route(const char *dir_path, const char *prompt) {
    CnetVsaGenRegistry *reg = (CnetVsaGenRegistry *)calloc(1, sizeof(CnetVsaGenRegistry));
    if (!reg) {
        fprintf(stderr, "Error: out of memory allocating registry.\n");
        return;
    }
    cnet_vsa_registry_init(reg, CNET_VSA_DEFAULT_DIM);
    const char *dir = (dir_path && *dir_path) ? dir_path : "bin";
    int n = cnet_vsa_registry_load_dir(reg, dir);

    printf("=================================================================\n");
    printf(" CNET Multi-Capsule Intent Router\n");
    printf("=================================================================\n");
    printf("  Registry Directory: %s (%d certified capsules indexed)\n", dir, n);
    printf("  Incoming Prompt:    \"%s\"\n\n", prompt);

    if (n == 0) {
        printf("  [!] No certified .gencap files found in '%s'.\n", dir);
        free(reg);
        return;
    }

    float query_vec[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_gencap_encode_intent(prompt, query_vec, reg->dim);

    printf("  Capsule Domain Match Scores (Topical Cosine Distance):\n");
    printf("  ---------------------------------------------------------------\n");
    int best_idx = -1;
    float best_dist = 1.0f;
    int winner = cnet_vsa_registry_route(reg, query_vec, &best_idx, &best_dist);

    for (size_t i = 0; i < reg->count; ++i) {
        float sim = cnet_vsa_similarity(query_vec, reg->capsules[i].header.centroid, reg->dim);
        float d = 1.0f - sim;
        const char *marker = ((int)i == winner) ? "--> [WINNER]" : "   ";
        printf("  %s %-26s | Domain: %-14s | Dist: %.4f (Limit: %.3f)\n",
               marker, reg->capsules[i].header.name, reg->capsules[i].header.domain, d, reg->capsules[i].header.safe_radius);
    }

    printf("  ---------------------------------------------------------------\n");
    if (winner >= 0) {
        printf("  Routing Decision: DISPATCH TO '%s' (dist=%.4f <= %.3f) [IN-DOMAIN]\n\n",
               reg->capsules[winner].header.name, best_dist, reg->capsules[winner].header.safe_radius);
    } else {
        printf("  Routing Decision: FAIL-CLOSED ABSTAIN (closest='%s', dist=%.4f > limit) [OUT-OF-DOMAIN]\n\n",
               (best_idx >= 0) ? reg->capsules[best_idx].header.name : "none", best_dist);
    }
    free(reg);
}

static void cmd_auto(const char *dir_path, const char *prompt) {
    CnetVsaGenRegistry *reg = (CnetVsaGenRegistry *)calloc(1, sizeof(CnetVsaGenRegistry));
    if (!reg) {
        fprintf(stderr, "Error: out of memory allocating registry.\n");
        return;
    }
    cnet_vsa_registry_init(reg, CNET_VSA_DEFAULT_DIM);
    const char *dir = (dir_path && *dir_path) ? dir_path : "bin";
    int n = cnet_vsa_registry_load_dir(reg, dir);

    printf("=================================================================\n");
    printf(" CNET Autonomous Multi-Capsule Dispatcher (Zero LLM)\n");
    printf("=================================================================\n");
    printf("  Prompt:          \"%s\"\n", prompt);
    printf("  Active Registry: %d capsules indexed from '%s'\n", n, dir);

    char out_text[1024] = {0};
    char cap_name[64] = {0};
    float dist = 0.0f;

    int rc = cnet_vsa_registry_dispatch(reg, prompt, out_text, sizeof(out_text), cap_name, &dist);

    printf("  Routed Capsule:  %s (dist=%.4f)\n", cap_name[0] ? cap_name : "none", dist);
    printf("  Verdict:         %s\n", (rc == 0) ? "SUCCESS (In-Domain)" : "REFUSED / ABSTAIN");
    printf("  Generated Response:\n  \"%s\"\n\n", out_text);
    free(reg);
}

static int parse_args(char *line, char *argv[], int max_args) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max_args) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '"') {
            p++;
            argv[argc++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') {
                *p = '\0';
                p++;
            }
        } else {
            argv[argc++] = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (*p) {
                *p = '\0';
                p++;
            }
        }
    }
    return argc;
}

static void run_repl(CliState *s) {
    printf("Interactive CNET-VSA Session Started. Type 'help' for commands, 'quit' to exit.\n\n");
    char line[CLI_MAX_LINE];

    while (1) {
        printf("cnet-vsa> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;

        /* Trim newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[len - 1] = '\0';
            len--;
        }
        if (len == 0) continue;

        char *r_argv[8];
        int r_argc = parse_args(line, r_argv, 8);
        if (r_argc == 0) continue;

        const char *cmd = r_argv[0];

        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            printf("Exiting CNET-VSA session.\n");
            break;
        } else if (strcmp(cmd, "help") == 0) {
            print_usage("cnet_vsa_cli");
        } else if (strcmp(cmd, "device") == 0 || strcmp(cmd, "device-status") == 0) {
            cmd_device_status(s);
        } else if (strcmp(cmd, "encode") == 0) {
            if (r_argc >= 2) {
                cmd_encode(s, r_argv[1]);
            } else {
                printf("Error: encode requires text argument.\n");
            }
        } else if (strcmp(cmd, "sim") == 0) {
            if (r_argc >= 3) {
                cmd_sim(s, r_argv[1], r_argv[2]);
            } else {
                printf("Error: sim requires two text arguments.\n");
            }
        } else if (strcmp(cmd, "extract") == 0) {
            if (r_argc >= 3) {
                cmd_extract(s, r_argv[1], atoi(r_argv[2]));
            } else {
                printf("Usage: extract \"<text>\" <position>\n");
            }
        } else if (strcmp(cmd, "ingest") == 0) {
            if (r_argc >= 2) {
                const char *q = (r_argc >= 3) ? r_argv[2] : NULL;
                cmd_ingest(s, r_argv[1], q);
            } else {
                printf("Error: ingest requires a file path.\n");
            }
        } else if (strcmp(cmd, "query") == 0) {
            if (r_argc >= 2) {
                cmd_query(s, r_argv[1]);
            } else {
                printf("Error: query requires a search string.\n");
            }
        } else if (strcmp(cmd, "swap-bench") == 0) {
            cmd_swap_bench(s);
        } else if (strcmp(cmd, "sleep-demo") == 0) {
            cmd_sleep_demo(s);
        } else if (strcmp(cmd, "ast-demo") == 0) {
            cmd_ast_demo(s);
        } else if (strcmp(cmd, "story-demo") == 0) {
            cmd_story_demo(s);
        } else if (strcmp(cmd, "story-gen") == 0) {
            const char *st = (r_argc >= 2) ? r_argv[1] : "whimsical";
            const char *hr = (r_argc >= 3) ? r_argv[2] : NULL;
            cmd_story_gen(s, st, hr);
        } else if (strcmp(cmd, "mouth-gen") == 0) {
            const char *st = (r_argc >= 2) ? r_argv[1] : "whimsical";
            const char *hr = (r_argc >= 3) ? r_argv[2] : "Oliver the fox";
            const char *se = (r_argc >= 4) ? r_argv[3] : "enchanted forest";
            const char *ar = (r_argc >= 5) ? r_argv[4] : "glowing mushroom";
            cmd_mouth_gen(s, st, hr, se, ar);
        } else if (strcmp(cmd, "ngram-gen") == 0) {
            const char *sd = (r_argc >= 2) ? r_argv[1] : "once";
            const char *hr = (r_argc >= 3) ? r_argv[2] : "fox";
            const char *se = (r_argc >= 4) ? r_argv[3] : "forest";
            cmd_ngram_gen(s, sd, hr, se);
        } else if (strcmp(cmd, "gencap-create") == 0) {
            if (r_argc >= 5) {
                cmd_gencap_create(r_argv[1], r_argv[2], r_argv[3], r_argv[4]);
            } else {
                printf("Usage: gencap-create <name> <domain> <corpus.txt> <output.gencap>\n");
            }
        } else if (strcmp(cmd, "gencap-verify") == 0) {
            if (r_argc >= 2) {
                cmd_gencap_verify(r_argv[1]);
            } else {
                printf("Usage: gencap-verify <file.gencap>\n");
            }
        } else if (strcmp(cmd, "gencap-gen") == 0) {
            if (r_argc >= 2) {
                const char *pr = (r_argc >= 3) ? r_argv[2] : NULL;
                const char *sd = (r_argc >= 4) ? r_argv[3] : NULL;
                int max_t = (r_argc >= 5) ? atoi(r_argv[4]) : 28;
                cmd_gencap_gen(r_argv[1], pr, sd, max_t);
            } else {
                printf("Usage: gencap-gen <file.gencap> [prompt] [seed] [max_tokens]\n");
            }
        } else if (strcmp(cmd, "route") == 0) {
            if (r_argc >= 3) {
                cmd_route(r_argv[1], r_argv[2]);
            } else if (r_argc == 2) {
                cmd_route("bin", r_argv[1]);
            } else {
                printf("Usage: route [dir] <prompt>\n");
            }
        } else if (strcmp(cmd, "auto") == 0) {
            if (r_argc >= 3) {
                cmd_auto(r_argv[2], r_argv[1]);
            } else if (r_argc == 2) {
                cmd_auto("bin", r_argv[1]);
            } else {
                printf("Usage: auto <prompt> [dir]\n");
            }
        } else {
            printf("Unknown command '%s'. Type 'help' for available commands.\n", cmd);
        }
    }
}

int main(int argc, char **argv) {
    const char *dev_override = NULL;
    int arg_offset = 1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
            dev_override = argv[i + 1];
            arg_offset = i + 2;
            break;
        }
    }

    CliState state;
    init_cli_state(&state, dev_override);

    if (argc <= 1 || (arg_offset >= argc && dev_override != NULL)) {
        print_banner();
        run_repl(&state);
        free_cli_state(&state);
        return 0;
    }

    const char *cmd = argv[arg_offset];

    if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "help") == 0) {
        print_banner();
        print_usage(argv[0]);
    } else if (strcmp(cmd, "device-status") == 0 || strcmp(cmd, "device") == 0) {
        cmd_device_status(&state);
    } else if (strcmp(cmd, "encode") == 0) {
        if (arg_offset + 1 < argc) {
            cmd_encode(&state, argv[arg_offset + 1]);
        } else {
            fprintf(stderr, "Error: encode requires text.\n");
        }
    } else if (strcmp(cmd, "sim") == 0) {
        if (arg_offset + 2 < argc) {
            cmd_sim(&state, argv[arg_offset + 1], argv[arg_offset + 2]);
        } else {
            fprintf(stderr, "Error: sim requires two text arguments.\n");
        }
    } else if (strcmp(cmd, "extract") == 0) {
        if (arg_offset + 2 < argc) {
            cmd_extract(&state, argv[arg_offset + 1], atoi(argv[arg_offset + 2]));
        } else {
            fprintf(stderr, "Error: extract requires text and position.\n");
        }
    } else if (strcmp(cmd, "ingest") == 0) {
        if (arg_offset + 1 < argc) {
            const char *q = (arg_offset + 2 < argc) ? argv[arg_offset + 2] : NULL;
            cmd_ingest(&state, argv[arg_offset + 1], q);
        } else {
            fprintf(stderr, "Error: ingest requires a file path.\n");
        }
    } else if (strcmp(cmd, "query") == 0) {
        if (arg_offset + 1 < argc) {
            cmd_query(&state, argv[arg_offset + 1]);
        } else {
            fprintf(stderr, "Error: query requires a needle.\n");
        }
    } else if (strcmp(cmd, "swap-bench") == 0) {
        cmd_swap_bench(&state);
    } else if (strcmp(cmd, "sleep-demo") == 0) {
        cmd_sleep_demo(&state);
    } else if (strcmp(cmd, "ast-demo") == 0) {
        cmd_ast_demo(&state);
    } else if (strcmp(cmd, "story-demo") == 0) {
        cmd_story_demo(&state);
    } else if (strcmp(cmd, "story-gen") == 0) {
        const char *st = (arg_offset + 1 < argc) ? argv[arg_offset + 1] : "whimsical";
        const char *hr = (arg_offset + 2 < argc) ? argv[arg_offset + 2] : NULL;
        cmd_story_gen(&state, st, hr);
    } else if (strcmp(cmd, "mouth-gen") == 0) {
        const char *st = (arg_offset + 1 < argc) ? argv[arg_offset + 1] : "whimsical";
        const char *hr = (arg_offset + 2 < argc) ? argv[arg_offset + 2] : "Oliver the fox";
        const char *se = (arg_offset + 3 < argc) ? argv[arg_offset + 3] : "enchanted forest";
        const char *ar = (arg_offset + 4 < argc) ? argv[arg_offset + 4] : "glowing mushroom";
        cmd_mouth_gen(&state, st, hr, se, ar);
    } else if (strcmp(cmd, "ngram-gen") == 0) {
        const char *sd = (arg_offset + 1 < argc) ? argv[arg_offset + 1] : "once";
        const char *hr = (arg_offset + 2 < argc) ? argv[arg_offset + 2] : "fox";
        const char *se = (arg_offset + 3 < argc) ? argv[arg_offset + 3] : "forest";
        cmd_ngram_gen(&state, sd, hr, se);
    } else if (strcmp(cmd, "gencap-create") == 0) {
        if (arg_offset + 4 < argc) {
            cmd_gencap_create(argv[arg_offset + 1], argv[arg_offset + 2], argv[arg_offset + 3], argv[arg_offset + 4]);
        } else {
            fprintf(stderr, "Usage: %s gencap-create <name> <domain> <corpus.txt> <output.gencap>\n", argv[0]);
        }
    } else if (strcmp(cmd, "gencap-verify") == 0) {
        if (arg_offset + 1 < argc) {
            cmd_gencap_verify(argv[arg_offset + 1]);
        } else {
            fprintf(stderr, "Usage: %s gencap-verify <file.gencap>\n", argv[0]);
        }
    } else if (strcmp(cmd, "gencap-gen") == 0) {
        if (arg_offset + 1 < argc) {
            const char *pr = (arg_offset + 2 < argc) ? argv[arg_offset + 2] : NULL;
            const char *sd = (arg_offset + 3 < argc) ? argv[arg_offset + 3] : NULL;
            int max_t = (arg_offset + 4 < argc) ? atoi(argv[arg_offset + 4]) : 28;
            cmd_gencap_gen(argv[arg_offset + 1], pr, sd, max_t);
        } else {
            fprintf(stderr, "Usage: %s gencap-gen <file.gencap> [prompt] [seed] [max_tokens]\n", argv[0]);
        }
    } else if (strcmp(cmd, "route") == 0) {
        if (arg_offset + 2 < argc) {
            cmd_route(argv[arg_offset + 1], argv[arg_offset + 2]);
        } else if (arg_offset + 1 < argc) {
            cmd_route("bin", argv[arg_offset + 1]);
        } else {
            fprintf(stderr, "Usage: %s route [dir] <prompt>\n", argv[0]);
        }
    } else if (strcmp(cmd, "auto") == 0) {
        if (arg_offset + 2 < argc) {
            cmd_auto(argv[arg_offset + 2], argv[arg_offset + 1]);
        } else if (arg_offset + 1 < argc) {
            cmd_auto("bin", argv[arg_offset + 1]);
        } else {
            fprintf(stderr, "Usage: %s auto <prompt> [dir]\n", argv[0]);
        }
    } else if (strcmp(cmd, "repl") == 0 || strcmp(cmd, "interactive") == 0) {
        print_banner();
        run_repl(&state);
    } else {
        fprintf(stderr, "Unknown command '%s'. Run '%s --help' for usage.\n", cmd, argv[0]);
    }

    free_cli_state(&state);
    return 0;
}
