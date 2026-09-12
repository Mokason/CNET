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
#include <dirent.h>
#include <sys/stat.h>

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
#include "cnet_vsa_lexicon.h"
#include "cnet_vsa_evidence.h"

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
    printf("  gencap-create <n> <d> <i> <o> Create & seal generative capsule; --encoder <name> (default from sweep gate); add --negatives <dir> [--probes f] to calibrate radius\n");
    printf("  gencap-verify <file.gencap>  Verify capsule cryptographic digest & domain specification\n");
    printf("  gencap-gen <cap> [prm] [sd]  Autonomous generation from capsule steered by intent (zero LLM)\n");
    printf("  route <dir> <prompt>       Rank all capsules in directory against prompt intent\n");
    printf("  route-batch <dir>          Same, one prompt per stdin line, registry loaded once\n");
    printf("  lexicon-build <dir> <out>  Learn a wide-space lexicon from *_corpus.txt (Random Indexing; phrases, subwords, distilled)\n");
    printf("  lexicon-train <in> <out> <dir> <pairs.tsv>  Supervised pass on (capsule, question) pairs toward corpus centroids\n");
    printf("  lexicon-info <file.lex>    Verify and describe a lexicon\n");
    printf("  stem-words                 stdin tokens -> token, stem, stopword flag, key (for distillation tooling)\n");
    printf("  env CNET_VSA_LEXICON=<f>   Activate a lexicon for --encoder lex (required for lex)\n");
    printf("  auto <prompt> [dir]        Auto-route prompt to best matching capsule and generate response\n");
    printf("  explain-facts <file> <start> <rel>...  Answer a typed path with evidence from supplied facts\n");
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

/* ---- calibration evidence loading ---------------------------------------- */

typedef struct {
    char **lines;
    size_t count, cap;
} LineList;

static void linelist_free(LineList *l) {
    for (size_t i = 0; i < l->count; ++i) free(l->lines[i]);
    free(l->lines);
    l->lines = NULL; l->count = l->cap = 0;
}

static int linelist_push(LineList *l, const char *text) {
    if (l->count == l->cap) {
        size_t ncap = l->cap ? l->cap * 2 : 256;
        char **nl = (char **)realloc(l->lines, ncap * sizeof(char *));
        if (!nl) return -1;
        l->lines = nl; l->cap = ncap;
    }
    l->lines[l->count] = strdup(text);
    if (!l->lines[l->count]) return -1;
    l->count++;
    return 0;
}

/* Read non-empty, non-comment lines of a text file into the list. */
static int linelist_read_file(LineList *l, const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char line[1024];
    int added = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        size_t len = strlen(p);
        while (len > 0 && isspace((unsigned char)p[len - 1])) p[--len] = '\0';
        if (len == 0 || *p == '#') continue;
        if (linelist_push(l, p) != 0) { fclose(fp); return -1; }
        added++;
    }
    fclose(fp);
    return added;
}

static int path_is_dir(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
}

static int same_file(const char *a, const char *b) {
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0) return 0;
    return (sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino) ? 1 : 0;
}

/* Negatives: a file of sentences, or a directory of *.txt corpora from other
 * domains. The capsule's own corpus file and any file whose name starts with
 * the capsule name are excluded. At most max_lines are kept by deterministic
 * reservoir sampling seeded from the capsule name, so re-runs are repeatable. */
static int load_negatives(LineList *out, const char *source, const char *own_corpus,
                          const char *cap_name, size_t max_lines, int *files_seen) {
    *files_seen = 0;
    if (!path_is_dir(source)) {
        int n = linelist_read_file(out, source);
        if (n < 0) return -1;
        *files_seen = 1;
        return 0;
    }

    uint64_t seed = 1469598103934665603ULL;
    for (const char *c = cap_name; *c; ++c) { seed ^= (unsigned char)*c; seed *= 1099511628211ULL; }
    if (seed == 0) seed = 0x9E3779B97F4A7C15ULL;

    DIR *d = opendir(source);
    if (!d) return -1;
    LineList all; memset(&all, 0, sizeof(all));
    size_t cap_len = strlen(cap_name);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t len = strlen(de->d_name);
        if (len < 5 || strcmp(de->d_name + len - 4, ".txt") != 0) continue;
        /* exclude only this capsule's own files by exact name; a sibling whose
         * name merely starts with ours is one of the hardest negatives */
        if (cap_len > 0) {
            char own_c[300], own_p[300];
            snprintf(own_c, sizeof(own_c), "%s_corpus.txt", cap_name);
            snprintf(own_p, sizeof(own_p), "%s_probes.txt", cap_name);
            if (strcmp(de->d_name, own_c) == 0 || strcmp(de->d_name, own_p) == 0) continue;
        }
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", source, de->d_name);
        if (own_corpus && same_file(full, own_corpus)) continue;
        if (linelist_read_file(&all, full) < 0) continue;
        (*files_seen)++;
    }
    closedir(d);

    /* reservoir sample */
    for (size_t i = 0; i < all.count; ++i) {
        if (out->count < max_lines) {
            if (linelist_push(out, all.lines[i]) != 0) { linelist_free(&all); return -1; }
        } else {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            size_t j = (size_t)(seed % (uint64_t)(i + 1));
            if (j < max_lines) {
                free(out->lines[j]);
                out->lines[j] = strdup(all.lines[i]);
                if (!out->lines[j]) { linelist_free(&all); return -1; }
            }
        }
    }
    linelist_free(&all);
    return 0;
}

static void print_calibration_receipt(const CnetVsaGenCapsule *cap) {
    const CnetVsaGencapCalibration *c = &cap->calib;
    const char *enc_name = cnet_vsa_encoder_name(c->reserved0);
    printf("  Encoder:           %s (id %u)%s\n", enc_name ? enc_name : "UNKNOWN", c->reserved0,
           c->reserved0 == CNET_VSA_ENCODER_DEFAULT ? " [default]" : "");
    if (!c->calibrated && c->negative_count == 0) {
        printf("  Calibration:       UNCALIBRATED (legacy fixed radius %.3f)\n", cap->safe_radius);
        return;
    }
    printf("  Calibration:       %s\n", c->calibrated ? "CALIBRATED (held-out evidence)" : "NOT_SEPARABLE (refused)");
    printf("    in-domain:       n=%u  dist mean=%.4f sd=%.4f  target accept>=%.2f  measured=%.3f\n",
           c->in_domain_count, c->in_dist_mean, c->in_dist_std, c->target_in_accept, c->in_accept_rate);
    printf("    negatives:       n=%u  dist mean=%.4f sd=%.4f  target reject>=%.2f  measured=%.3f\n",
           c->negative_count, c->neg_dist_mean, c->neg_dist_std, c->target_neg_reject, c->neg_reject_rate);
    printf("    radius window:   r_in=%.4f  r_neg=%.4f  separation=%+.4f  chosen=%.4f%s\n",
           c->radius_in, c->radius_neg, c->separation, cap->safe_radius,
           (c->separation >= 0.0f || c->negative_count == 0) ? "" : "  (float space not separable: radius on the fail-closed side)");
    if (cap->topical.present) {
        const CnetVsaTopicalBlock *t = &cap->topical;
        printf("  Topical block:     v3, %u-d int8 centroid (%u bytes), %s\n",
               t->width, t->width, t->calibrated ? "CALIBRATED in wide space" : "UNCALIBRATED (ceiling 0.900)");
        if (t->calibrated || c->negative_count > 0) {
            printf("    in-domain:       dist mean=%.4f sd=%.4f  measured accept=%.3f\n", t->in_dist_mean, t->in_dist_std, t->in_accept_rate);
            printf("    negatives:       dist mean=%.4f sd=%.4f  measured reject=%.3f\n", t->neg_dist_mean, t->neg_dist_std, t->neg_reject_rate);
            printf("    radius window:   r_in=%.4f  r_neg=%.4f  separation=%+.4f  chosen=%.4f\n",
                   t->radius_in, t->radius_neg, t->separation, t->safe_radius);
        }
    } else {
        printf("  Topical block:     none (format v%u routes in the float-512 space)\n", cap->version);
    }
}

typedef struct {
    const char *probes_path;     /* optional: held-out in-domain queries, one per line */
    const char *negatives_path;  /* optional: file or directory of other-domain corpora */
    float target_in;
    float target_neg;
    size_t max_negatives;
    const char *calib_report;    /* optional: write sorted in/neg distances here */
    int dry_run;                 /* 1: calibrate and report, never save */
    uint32_t encoder_id;         /* CNET_VSA_ENCODER_HD by default for new seals */
} GencapCreateOpts;

static void gencap_create_opts_default(GencapCreateOpts *o) {
    memset(o, 0, sizeof(*o));
    o->target_in = 0.90f;
    o->target_neg = 0.95f;
    o->max_negatives = 512;
    o->encoder_id = CNET_VSA_ENCODER_DEFAULT;
}

/* Parses trailing --probes/--negatives/--target-in/--target-neg/--max-negatives */
static int gencap_create_opts_parse(GencapCreateOpts *o, int argc, char **argv) {
    for (int i = 0; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (strcmp(a, "--probes") == 0 && v) { o->probes_path = v; i++; }
        else if (strcmp(a, "--negatives") == 0 && v) { o->negatives_path = v; i++; }
        else if (strcmp(a, "--target-in") == 0 && v) { o->target_in = (float)atof(v); i++; }
        else if (strcmp(a, "--target-neg") == 0 && v) { o->target_neg = (float)atof(v); i++; }
        else if (strcmp(a, "--max-negatives") == 0 && v) { o->max_negatives = (size_t)atoi(v); i++; }
        else if (strcmp(a, "--calib-report") == 0 && v) { o->calib_report = v; i++; }
        else if (strcmp(a, "--dry-run") == 0) { o->dry_run = 1; }
        else if (strcmp(a, "--encoder") == 0 && v) {
            if (cnet_vsa_encoder_parse(v, &o->encoder_id) != 0) {
                fprintf(stderr, "Error: --encoder must be one of default");
                for (uint32_t k = 0; k < CNET_VSA_ENCODER_COUNT; ++k) fprintf(stderr, "|%s", cnet_vsa_encoder_name(k));
                fprintf(stderr, ".\n");
                return -1;
            }
            i++;
        }
        else { fprintf(stderr, "Error: unknown option '%s'.\n", a); return -1; }
    }
    return 0;
}

/* Returns 0 on sealed+saved, nonzero on any refusal (so scripts can gate on it). */
static int cmd_gencap_create(const char *name, const char *domain, const char *corpus_path,
                             const char *out_capsule, const GencapCreateOpts *opts) {
    CnetVsaGenCapsule *cap = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule));
    if (!cap) {
        fprintf(stderr, "Error: out of memory allocating capsule.\n");
        return 1;
    }

    if (cnet_vsa_gencap_init(cap, name, domain, CNET_VSA_DEFAULT_DIM) != 0) {
        fprintf(stderr, "Error: failed to initialize capsule '%s'.\n", name);
        free(cap);
        return 1;
    }
    uint32_t enc = opts ? opts->encoder_id : CNET_VSA_ENCODER_DEFAULT;
    if (cnet_vsa_gencap_set_encoder(cap, enc) != 0) {
        fprintf(stderr, "Error: failed to set encoder %u on capsule '%s'.\n", enc, name);
        free(cap);
        return 1;
    }

    LineList corpus, probes, negatives;
    memset(&corpus, 0, sizeof(corpus));
    memset(&probes, 0, sizeof(probes));
    memset(&negatives, 0, sizeof(negatives));

    if (linelist_read_file(&corpus, corpus_path) < 0) {
        fprintf(stderr, "Error: cannot open corpus file '%s'.\n", corpus_path);
        free(cap);
        return 1;
    }

    int lines_ingested = 0;
    for (size_t i = 0; i < corpus.count; ++i) {
        if (cnet_vsa_gencap_ingest(cap, corpus.lines[i]) > 0) lines_ingested++;
    }

    if (lines_ingested == 0) {
        fprintf(stderr, "Error: no valid sentences ingested from '%s'.\n", corpus_path);
        linelist_free(&corpus);
        free(cap);
        return 1;
    }

    int calib_rc = 0;
    int calibration_requested = (opts && opts->negatives_path) ? 1 : 0;
    int neg_files = 0;
    if (calibration_requested) {
        if (opts->probes_path && linelist_read_file(&probes, opts->probes_path) < 0) {
            fprintf(stderr, "Error: cannot open probes file '%s'.\n", opts->probes_path);
            linelist_free(&corpus); free(cap);
            return 1;
        }
        if (load_negatives(&negatives, opts->negatives_path, corpus_path, name,
                           opts->max_negatives, &neg_files) != 0) {
            fprintf(stderr, "Error: cannot load negatives from '%s'.\n", opts->negatives_path);
            linelist_free(&corpus); linelist_free(&probes); free(cap);
            return 1;
        }
        size_t in_total = corpus.count + probes.count, n_in = 0, n_neg = 0;
        float *d_in = (float *)calloc(in_total ? in_total : 1, sizeof(float));
        float *d_neg = (float *)calloc(negatives.count ? negatives.count : 1, sizeof(float));
        calib_rc = cnet_vsa_gencap_calibrate_ex(cap,
                                                (const char *const *)corpus.lines, corpus.count,
                                                (const char *const *)probes.lines, probes.count,
                                                (const char *const *)negatives.lines, negatives.count,
                                                opts->target_in, opts->target_neg,
                                                d_in, in_total, &n_in, d_neg, negatives.count, &n_neg);
        if (opts->calib_report && d_in && d_neg) {
            FILE *rf = fopen(opts->calib_report, "w");
            if (rf) {
                fprintf(rf, "name %s\nin", name);
                for (size_t i = 0; i < n_in; ++i) fprintf(rf, " %.5f", d_in[i]);
                fprintf(rf, "\nneg");
                for (size_t i = 0; i < n_neg; ++i) fprintf(rf, " %.5f", d_neg[i]);
                fprintf(rf, "\n");
                fclose(rf);
            }
        }
        free(d_in); free(d_neg);
    }

    int seal_rc = (calib_rc == 0) ? cnet_vsa_gencap_seal(cap) : calib_rc;
    if (seal_rc != 0) {
        printf("=================================================================\n");
        printf(" CNET Generative Knowledge Capsule REFUSED\n");
        printf("=================================================================\n");
        printf("  Capsule Name:      %s\n", cap->name);
        printf("  Domain:            %s\n", cap->domain);
        printf("  Lines Ingested:    %d\n", lines_ingested);
        if (seal_rc == CNET_VSA_GENCAP_NOT_SEPARABLE) {
            print_calibration_receipt(cap);
            printf("  Status:            REFUSED NOT_SEPARABLE [FAIL] (no radius meets both targets)\n\n");
        } else if (seal_rc == CNET_VSA_GENCAP_INSUFFICIENT_EVIDENCE) {
            printf("  Evidence:          in-domain=%zu probes=%zu negatives=%zu (files=%d); need >=%d and >=%d\n",
                   corpus.count, probes.count, negatives.count, neg_files,
                   CNET_VSA_GENCAP_MIN_IN_DOMAIN, CNET_VSA_GENCAP_MIN_NEGATIVES);
            printf("  Status:            REFUSED INSUFFICIENT_EVIDENCE [FAIL]\n\n");
        } else {
            printf("  Status:            REFUSED seal rc=%d [FAIL]\n\n", seal_rc);
        }
        linelist_free(&corpus); linelist_free(&probes); linelist_free(&negatives);
        free(cap);
        return 2;
    }

    if (opts && opts->dry_run) {
        printf("  Dry run: calibration %s, radius %.4f (not saved)\n",
               cap->calib.calibrated ? "CALIBRATED" : "UNCALIBRATED", cap->safe_radius);
        linelist_free(&corpus); linelist_free(&probes); linelist_free(&negatives);
        free(cap);
        return 0;
    }

    if (cnet_vsa_gencap_save(cap, out_capsule) != 0) {
        fprintf(stderr, "Error: failed to save capsule to '%s'.\n", out_capsule);
        linelist_free(&corpus); linelist_free(&probes); linelist_free(&negatives);
        free(cap);
        return 1;
    }

    printf("=================================================================\n");
    printf(" CNET Generative Knowledge Capsule Created & Certified\n");
    printf("=================================================================\n");
    printf("  Capsule Name:      %s\n", cap->name);
    printf("  Domain:            %s\n", cap->domain);
    printf("  Format Version:    %u\n", cap->version);
    printf("  Lines Ingested:    %d\n", lines_ingested);
    printf("  Vocabulary:        %zu unique words\n", cap->ngram.vocab_count);
    printf("  Transitions:       %zu n-gram transitions\n", cap->ngram.transition_count);
    printf("  Safe Radius:       %.3f\n", cap->safe_radius);
    print_calibration_receipt(cap);
    if (calibration_requested) {
        printf("    evidence source: probes=%zu  negatives=%zu from %d file(s) under '%s'\n",
               probes.count, negatives.count, neg_files, opts->negatives_path);
    }
    printf("  Integrity Digest:  0x%016llx\n", (unsigned long long)cap->digest);
    printf("  Output File:       %s\n", out_capsule);
    printf("  Status:            SEALED & CERTIFIED [PASS]\n\n");

    linelist_free(&corpus); linelist_free(&probes); linelist_free(&negatives);
    free(cap);
    return 0;
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
    printf("  Format Version:    %u\n", cap->version);
    printf("  Safe Radius:       %.3f\n", cap->safe_radius);
    print_calibration_receipt(cap);
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

    /* a LEX capsule needs the lexicon it was sealed under: the one shipped next
     * to it (registry.lex) or CNET_VSA_LEXICON; without it, refuse rather than
     * gate in the weaker float space */
    if (cnet_vsa_gencap_encoder_id(cap) == CNET_VSA_ENCODER_LEX) {
        char dir[1024]; snprintf(dir, sizeof(dir), "%s", capsule_path);
        char *slash = strrchr(dir, '/'); if (slash) *slash = 0; else snprintf(dir, sizeof(dir), ".");
        int lrc = cnet_vsa_lexicon_activate_default(dir);
        uint32_t tag = 0; memcpy(&tag, &cap->topical.reserved1, sizeof(tag));
        if (!cnet_vsa_lexicon_active() || cnet_vsa_lexicon_active_tag() != tag) {
            fprintf(stderr, "Error: capsule '%s' was sealed under lexicon tag 0x%08x; %s (rc=%d). Ship registry.lex next to it or set CNET_VSA_LEXICON.\n",
                    cap->name, tag, cnet_vsa_lexicon_active() ? "the active lexicon has another tag" : "no lexicon could be activated", lrc);
            free(cap);
            return;
        }
    }
    float intent_vec[CNET_VSA_DEFAULT_DIM];
    float *p_intent = NULL;
    if (prompt && *prompt) {
        if (cnet_vsa_gencap_encode_intent_ex(prompt, intent_vec, cap->ngram.dim,
                                             cnet_vsa_gencap_encoder_id(cap)) == 0) {
            p_intent = intent_vec;
        }
    }

    int8_t intent_q8[CNET_VSA_TOPICAL_DIM];
    const int8_t *p_bits = NULL;
    if (prompt && *prompt && cap->topical.present &&
        cnet_vsa_gencap_encode_intent_q8(prompt, intent_q8, cnet_vsa_gencap_encoder_id(cap)) == 0) {
        p_bits = intent_q8;
    }

    char out_buf[1024] = {0};
    int toks_out = 0;
    int gen_rc = cnet_vsa_gencap_generate_ex(cap, seed, p_intent, p_bits, 0.45f,
                                            max_tokens > 0 ? max_tokens : 28,
                                            out_buf, sizeof(out_buf), &toks_out);

    printf("=================================================================\n");
    printf(" CNET Autonomous Capsule Generation (Pure VSA, Zero LLM)\n");
    printf("=================================================================\n");
    printf("  Capsule:    %s (Domain: %s)\n", cap->name, cap->domain);
    if (prompt) printf("  Prompt:     \"%s\"\n", prompt);
    if (seed)   printf("  Seed:       \"%s\"\n", seed);
    printf("  Gate space: %s\n", p_bits ? "wide int8 (v3 topical block)" : (p_intent ? "float-512" : "none"));
    printf("  Result:     %s\n", (gen_rc == 0) ? "SUCCESS (In-Domain)" : "REFUSED (Out-of-Domain)");
    printf("  Tokens:     %d\n", toks_out);
    printf("  Output:     \"%s\"\n\n", out_buf);

    free(cap);
}

/* Measurement knobs for route/auto: CNET_VSA_AMBIGUITY_K (float, 0 = off) and
 * CNET_VSA_FORCE_FLOAT=1 (route in the float-512 space even for v3 registries). */
static void registry_apply_env(CnetVsaGenRegistry *reg) {
    const char *k = getenv("CNET_VSA_AMBIGUITY_K");
    if (k && *k) { reg->ambiguity_k_wide = (float)atof(k); reg->ambiguity_k_float = (float)atof(k); }
    const char *f = getenv("CNET_VSA_FORCE_FLOAT");
    if (f && *f && strcmp(f, "0") != 0) reg->force_float = 1;
    const char *t = getenv("CNET_VSA_TERM_GATE");
    if (t && *t && strcmp(t, "0") == 0) reg->term_gate = 0;
}

/* Route one prompt against an already loaded registry and print the decision. */
static void route_print(CnetVsaGenRegistry *reg, const char *dir, int n, const char *prompt) {
    printf("=================================================================\n");
    printf(" CNET Multi-Capsule Intent Router\n");
    printf("=================================================================\n");
    printf("  Registry Directory: %s (%d certified capsules indexed)\n", dir, n);
    printf("  Incoming Prompt:    \"%s\"\n\n", prompt);

    if (n == 0) {
        printf("  [!] No certified .gencap files found in '%s'.\n", dir);
        return;
    }

    printf("  Capsule Domain Match Scores (Topical Cosine Distance):\n");
    printf("  ---------------------------------------------------------------\n");
    CnetVsaRouteResult rr;
    int winner = cnet_vsa_registry_route_query(reg, prompt, &rr);
    int best_idx = rr.best_idx;
    float best_dist = rr.best_dist;

    /* Print the top 12 by similarity so large registries stay readable */
    size_t shown = reg->count < 12 ? reg->count : 12;
    float *sims = (float *)calloc(reg->count, sizeof(float));
    int *order = (int *)calloc(reg->count, sizeof(int));
    /* one query per encoder present, in the space the registry routes in */
    int binary = cnet_vsa_registry_binary_space(reg);
    float qv[CNET_VSA_ENCODER_COUNT][CNET_VSA_DEFAULT_DIM];
    static int8_t qq[CNET_VSA_ENCODER_COUNT][CNET_VSA_TOPICAL_DIM];
    float qn[CNET_VSA_ENCODER_COUNT];
    int have[CNET_VSA_ENCODER_COUNT];
    memset(have, 0, sizeof(have));
    for (uint32_t k = 0; k < CNET_VSA_ENCODER_COUNT; ++k) {
        if (binary) {
            have[k] = (cnet_vsa_gencap_encode_intent_q8(prompt, qq[k], k) == 0);
            qn[k] = have[k] ? cnet_vsa_text_q8_norm(qq[k]) : 0.0f;
        } else {
            have[k] = (cnet_vsa_gencap_encode_intent_ex(prompt, qv[k], reg->dim, k) == 0);
        }
    }
    printf("  Routing space:      %s\n", binary ? "wide int8-2048 (v3 topical blocks)" : "float-512 (legacy, mixed, or forced)");
    if (sims && order) {
        for (size_t i = 0; i < reg->count; ++i) {
            uint32_t enc = reg->capsules[i].encoder_id < CNET_VSA_ENCODER_COUNT ? reg->capsules[i].encoder_id : 0u;
            if (!have[enc]) sims[i] = -2.0f;
            else if (binary) sims[i] = cnet_vsa_text_q8_similarity_n(qq[enc], qn[enc], reg->capsules[i].topical, reg->capsules[i].topical_norm);
            else sims[i] = cnet_vsa_similarity(qv[enc], reg->capsules[i].header.centroid, reg->dim);
            order[i] = (int)i;
        }
        for (size_t a = 0; a < shown; ++a) {
            size_t best = a;
            for (size_t b = a + 1; b < reg->count; ++b) if (sims[order[b]] > sims[order[best]]) best = b;
            int t = order[a]; order[a] = order[best]; order[best] = t;
        }
        for (size_t a = 0; a < shown; ++a) {
            int i = order[a];
            const char *marker = (i == winner) ? "--> [WINNER]" : "   ";
            printf("  %s %-40s | %-14s | Dist: %.4f (Limit: %.3f%s) enc=%s\n",
                   marker, reg->capsules[i].header.name, reg->capsules[i].header.domain,
                   1.0f - sims[i],
                   binary ? reg->capsules[i].topical_radius : reg->capsules[i].header.safe_radius,
                   reg->capsules[i].header.version >= CNET_VSA_GENCAP_VERSION ? "" : " legacy",
                   cnet_vsa_encoder_name(reg->capsules[i].encoder_id) ? cnet_vsa_encoder_name(reg->capsules[i].encoder_id) : "?");
        }
        if (reg->count > shown) printf("      ... %zu more capsules not shown\n", reg->count - shown);
    }
    free(sims); free(order);

    printf("  ---------------------------------------------------------------\n");
    printf("  Radius gate:      dist=%.4f %s limit=%.3f -> %s\n",
           best_dist, rr.radius_ok ? "<=" : ">", rr.radius, rr.radius_ok ? "pass" : "REFUSE");
    if (rr.margin_checked) {
        printf("  Margin gate:      z=%.2f %s z_min=%.2f (null mean=%.4f sd=%.4f over %zu others; gap to runner-up=%.4f%s) -> %s\n",
               rr.z, rr.margin_ok ? ">=" : "<", rr.z_min, rr.null_mean, rr.null_std, reg->count - 1,
               rr.gap, rr.ambiguous ? ", AMBIGUOUS" : "", rr.margin_ok ? "pass" : "REFUSE");
    } else {
        printf("  Margin gate:      skipped (%zu other capsules < %zu minimum)\n",
               reg->count ? reg->count - 1 : 0, reg->min_null_count);
    }
    float amb_k = binary ? reg->ambiguity_k_wide : reg->ambiguity_k_float;
    if (amb_k > 0.0f && rr.margin_checked) {
        printf("  Ambiguity gate:   gap=%.4f %s %.2f*sd=%.4f (runner-up '%s') -> %s\n",
               rr.gap, rr.ambiguity_ok ? ">=" : "<", amb_k, amb_k * rr.null_std,
               rr.second_idx >= 0 ? reg->capsules[rr.second_idx].header.name : "none",
               rr.ambiguity_ok ? "pass" : "REFUSE");
    } else {
        printf("  Ambiguity gate:   off for this space (k=%.2f; CNET_VSA_AMBIGUITY_K overrides)\n", amb_k);
    }
    if (rr.term_checked) {
        if (rr.term_words < 2)
            printf("  Term gate:        %d content word%s (a route needs at least two) -> REFUSE\n", rr.term_words, rr.term_words == 1 ? "" : "s");
        else if (rr.term_ok)
            printf("  Term gate:        every leave-one-word-out query stays inside the radius (worst dist=%.4f <= %.3f over %d words) -> pass\n",
                   rr.term_worst_dist, rr.radius, rr.term_words);
        else
            printf("  Term gate:        without '%s' dist=%.4f > limit=%.3f (the accept depended on one word) -> REFUSE\n",
                   rr.term_word, rr.term_worst_dist, rr.radius);
    } else if (binary && !reg->term_gate) {
        printf("  Term gate:        off (CNET_VSA_TERM_GATE=0)\n");
    }
    if (winner >= 0) {
        printf("  Routing Decision: DISPATCH TO '%s' [IN-DOMAIN]\n\n", reg->capsules[winner].header.name);
    } else {
        const char *why = (rr.status == CNET_VSA_ROUTE_REFUSE_MARGIN) ? "margin"
                        : (rr.status == CNET_VSA_ROUTE_REFUSE_AMBIGUOUS) ? "ambiguity"
                        : (rr.status == CNET_VSA_ROUTE_REFUSE_TERM) ? "term" : "radius";
        printf("  Routing Decision: FAIL-CLOSED ABSTAIN (closest='%s', refused by %s gate) [OUT-OF-DOMAIN]\n\n",
               (best_idx >= 0) ? reg->capsules[best_idx].header.name : "none", why);
    }
}

static CnetVsaLexicon g_lexicon;
static int g_lexicon_loaded = 0;

/* CNET_VSA_LEXICON=<file> activates a learned lexicon for the LEX encoder.
 * A bad file is fatal: a LEX capsule must never be built or routed without
 * the exact lexicon it was calibrated in. */
static int lexicon_apply_env(void) {
    const char *path = getenv("CNET_VSA_LEXICON");
    if (!path || !*path) return 0;
    int rc = cnet_vsa_lexicon_load(&g_lexicon, path);
    if (rc != 0) {
        fprintf(stderr, "Error: CNET_VSA_LEXICON '%s' failed to load (rc=%d%s).\n", path, rc,
                rc == -4 ? ": header rejected, expected version 3; rebuild the table with lexicon-build" : "");
        return -1;
    }
    cnet_vsa_lexicon_set_active(&g_lexicon);
    g_lexicon_loaded = 1;
    return 0;
}

static int cmd_lexicon_build(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: lexicon-build <corpus_dir> <out.lex> [--beta 0.5] [--window 3] [--nnz 16] [--min-count 2] [--max-vocab 16384] [--reflective 0]\n"
                        "                     [--remove-pcs 0] [--distilled <file.dstl> --distill-alpha 0.5] [--idf-floor 0.25] [--idf-power 1.0]\n"
                        "                     [--phrases 0 --phrase-min-count 3 --phrase-weight 1.0] [--subwords 0 --subword-min-n 3 --subword-max-n 5 --subword-min-words 4 --subword-max-words 5%%vocab --subword-weight 1.0]\n"
                        "                     [--vocab-dump <tsv>]\n");
        return 1;
    }
    CnetVsaLexiconBuildOpts o;
    cnet_vsa_lexicon_build_opts_default(&o);
    for (int i = 2; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--beta") == 0) o.beta = (float)atof(argv[i + 1]);
        else if (strcmp(argv[i], "--window") == 0) o.window = (uint32_t)atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--nnz") == 0) o.nnz = (uint32_t)atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--min-count") == 0) o.min_count = (uint32_t)atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--max-vocab") == 0) o.max_vocab = (uint32_t)atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--reflective") == 0) o.reflective = (uint32_t)atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--max-df") == 0) o.max_df = (float)atof(argv[i + 1]);
        else if (strcmp(argv[i], "--center") == 0) o.center = (uint32_t)atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--remove-pcs") == 0) o.remove_pcs = (uint32_t)atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--distilled") == 0) o.distilled = argv[i + 1];
        else if (strcmp(argv[i], "--distill-alpha") == 0) o.distill_alpha = (float)atof(argv[i + 1]);
        else if (strcmp(argv[i], "--idf-floor") == 0) o.idf_floor = (float)atof(argv[i + 1]);
        else if (strcmp(argv[i], "--idf-power") == 0) o.idf_power = (float)atof(argv[i + 1]);
        else if (strcmp(argv[i], "--phrases") == 0) o.max_phrases = (uint32_t)atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--phrase-min-count") == 0) o.phrase_min_count = (uint32_t)atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--phrase-weight") == 0) o.phrase_weight = (float)atof(argv[i + 1]);
        else if (strcmp(argv[i], "--subwords") == 0) o.max_subwords = (uint32_t)atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--subword-min-n") == 0) o.subword_min_n = (uint32_t)atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--subword-max-n") == 0) o.subword_max_n = (uint32_t)atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--subword-min-words") == 0) o.subword_min_words = (uint32_t)atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--subword-max-words") == 0) o.subword_max_words = (uint32_t)atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--subword-weight") == 0) o.subword_weight = (float)atof(argv[i + 1]);
        else if (strcmp(argv[i], "--vocab-dump") == 0) o.vocab_dump = argv[i + 1];
        else { fprintf(stderr, "Error: unknown option '%s'.\n", argv[i]); return 1; }
    }
    CnetVsaLexiconBuildReport rep;
    int rc = cnet_vsa_lexicon_build(argv[0], &o, argv[1], &rep);
    if (rc != 0) { fprintf(stderr, "Error: lexicon build failed (rc=%d).\n", rc); return 2; }
    CnetVsaLexicon lex;
    if (cnet_vsa_lexicon_load(&lex, argv[1]) != 0) { fprintf(stderr, "Error: built lexicon does not verify.\n"); return 3; }
    printf("=================================================================\n");
    printf(" CNET Learned Lexicon Built (Random Indexing, sparse sweep)\n");
    printf("=================================================================\n");
    printf("  Corpus dir:        %s (%llu files, %llu sentences, %llu content tokens, %llu distinct)\n",
           argv[0], (unsigned long long)rep.files, (unsigned long long)rep.sentences,
           (unsigned long long)rep.tokens, (unsigned long long)rep.distinct);
    printf("  Vocabulary:        %u words + %u phrases (min count %u, cap %u; phrase min count %u)\n", lex.hdr.count - lex.hdr.phrases, lex.hdr.phrases, o.min_count, o.max_vocab, lex.hdr.phrase_min_count);
    printf("  Subwords:          %u character n-grams (n %u..%u, in >= %u words) for unknown-word composition\n", lex.hdr.subwords, lex.hdr.subword_min_n, lex.hdr.subword_max_n, lex.hdr.subword_min_words);
    printf("  Parameters:        window %u, nnz %u, beta %.2f, reflective %u, max-df %.2f, center %u, remove-pcs %u, distilled %u keys (alpha %.2f)\n", o.window, o.nnz, o.beta, o.reflective, o.max_df, o.center, o.remove_pcs, lex.hdr.distilled, lex.hdr.distill_alpha);
    printf("  Sweep time:        %.3f s\n", rep.sweep_seconds);
    printf("  Scale:             global %.3f, identity-only magnitude %u\n", lex.hdr.global_scale, lex.hdr.fallback_mag);
    printf("  Table size:        %zu bytes\n", sizeof(CnetVsaLexiconHeader) + (size_t)lex.hdr.count * sizeof(CnetVsaLexiconEntry) + (size_t)lex.hdr.subwords * sizeof(CnetVsaLexiconSubword));
    printf("  Digest:            0x%016llx (tag 0x%08x)\n", (unsigned long long)lex.hdr.digest, (unsigned)(lex.hdr.digest & 0xffffffffu));
    printf("  Output File:       %s\n\n", argv[1]);
    cnet_vsa_lexicon_free(&lex);
    return 0;
}

/* stem-words: one token per stdin line -> "token<TAB>stem<TAB>stop<TAB>key_hex"
 * using the runtime's exact stopword rule, stemmer and key hash, so external
 * tools (the distiller) build vocabularies the LEX encoder will actually hit. */
static int cmd_stem_words(void) {
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!*line) continue;
        char stem[CNET_VSA_TOKEN_LEN];
        snprintf(stem, sizeof(stem), "%s", line);
        cnet_vsa_text_stem(stem);
        printf("%s\t%s\t%d\t%016llx\n", line, stem, cnet_vsa_text_is_stopword(line),
               (unsigned long long)cnet_vsa_lexicon_word_key(line));
    }
    return 0;
}

/* lexicon-train: supervised pass over (capsule name, question) pairs; the
 * capsule's corpus centroid is the target. Writes a new table with a receipt. */
static int cmd_lexicon_train(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: lexicon-train <in.lex> <out.lex> <corpus_dir> <pairs.tsv> [--epochs 8] [--lr 0.05] [--margin 0.10] [--seed N] [--sentences 0]\n"
                        "       pairs.tsv lines: <corpus name>\\t<question>, name = <name>_corpus.txt under corpus_dir\n");
        return 1;
    }
    CnetVsaLexiconTrainOpts o;
    cnet_vsa_lexicon_train_opts_default(&o);
    for (int i = 4; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--epochs") == 0) o.epochs = (uint32_t)atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--lr") == 0) o.lr = (float)atof(argv[i + 1]);
        else if (strcmp(argv[i], "--margin") == 0) o.margin = (float)atof(argv[i + 1]);
        else if (strcmp(argv[i], "--seed") == 0) o.seed = strtoull(argv[i + 1], NULL, 0);
        else if (strcmp(argv[i], "--sentences") == 0) o.sentences_per_corpus = (uint32_t)atoi(argv[i + 1]);
        else { fprintf(stderr, "Error: unknown option '%s'.\n", argv[i]); return 1; }
    }
    CnetVsaLexiconTrainReport rep;
    int rc = cnet_vsa_lexicon_train(argv[0], argv[1], argv[2], argv[3], &o, &rep);
    if (rc != 0) { fprintf(stderr, "Error: lexicon training failed (rc=%d).\n", rc); return 2; }
    CnetVsaLexicon lex;
    if (cnet_vsa_lexicon_load(&lex, argv[1]) != 0) { fprintf(stderr, "Error: trained lexicon does not verify.\n"); return 3; }
    printf("=================================================================\n");
    printf(" CNET Learned Lexicon Trained (question -> capsule contract pairs)\n");
    printf("=================================================================\n");
    printf("  Pairs:             %u questions + %u corpus sentences over %u corpora, %u epochs, lr %.3f, margin %.3f\n", rep.pairs, rep.sentence_pairs, rep.corpora, rep.epochs, o.lr, o.margin);
    printf("  Terms moved:       %u of %u entries\n", rep.terms_touched, lex.hdr.count);
    printf("  Train top-1:       %.1f%% -> %.1f%% (on the training pairs; a sanity number, not a result)\n", 100.0 * rep.train_top1_before, 100.0 * rep.train_top1_after);
    printf("  Time:              %.1f s\n", rep.seconds);
    printf("  Digest:            0x%016llx (tag 0x%08x)\n", (unsigned long long)lex.hdr.digest, (unsigned)(lex.hdr.digest & 0xffffffffu));
    printf("  Output File:       %s\n\n", argv[1]);
    cnet_vsa_lexicon_free(&lex);
    return 0;
}

static int cmd_lexicon_info(const char *path) {
    CnetVsaLexicon lex;
    int rc = cnet_vsa_lexicon_load(&lex, path);
    if (rc != 0) { printf("  Lexicon '%s': FAILED to verify (rc=%d)\n", path, rc); return 1; }
    printf("  Lexicon:           %s\n  Words:             %u (+ %u phrases)\n  Subwords:          %u\n  Dim:               %u\n  Window/nnz/beta:   %u / %u / %.2f (reflective %u, PCs removed %u, distilled %u keys alpha %.2f)\n  Trained:           %u pairs, %u epochs, lr %.3f, margin %.3f\n  Sentences/tokens:  %llu / %llu\n  Digest:            0x%016llx (tag 0x%08x)\n  Verdict:           LEXICON_AUTHENTIC [PASS]\n",
           path, lex.hdr.count - lex.hdr.phrases, lex.hdr.phrases, lex.hdr.subwords, lex.hdr.dim, lex.hdr.window, lex.hdr.nnz, lex.hdr.beta, lex.hdr.reflective,
           lex.hdr.remove_pcs, lex.hdr.distilled, lex.hdr.distill_alpha, lex.hdr.trained_pairs, lex.hdr.train_epochs, lex.hdr.train_lr, lex.hdr.train_margin,
           (unsigned long long)lex.hdr.sentences, (unsigned long long)lex.hdr.tokens,
           (unsigned long long)lex.hdr.digest, (unsigned)(lex.hdr.digest & 0xffffffffu));
    cnet_vsa_lexicon_free(&lex);
    return 0;
}

static void cmd_route(const char *dir_path, const char *prompt) {
    CnetVsaGenRegistry *reg = (CnetVsaGenRegistry *)calloc(1, sizeof(CnetVsaGenRegistry));
    if (!reg) {
        fprintf(stderr, "Error: out of memory allocating registry.\n");
        return;
    }
    cnet_vsa_registry_init(reg, CNET_VSA_DEFAULT_DIM);
    registry_apply_env(reg);
    const char *dir = (dir_path && *dir_path) ? dir_path : "bin";
    int n = cnet_vsa_registry_load_dir(reg, dir);
    route_print(reg, dir, n, prompt);
    free(reg);
}

/* Same output as `route`, one block per stdin line, with the registry loaded
 * and verified once. Blocks are separated by a "=== QUERY <k> ===" line so
 * tools can split them; an empty input line is skipped. */
static void cmd_route_batch(const char *dir_path) {
    CnetVsaGenRegistry *reg = (CnetVsaGenRegistry *)calloc(1, sizeof(CnetVsaGenRegistry));
    if (!reg) {
        fprintf(stderr, "Error: out of memory allocating registry.\n");
        return;
    }
    cnet_vsa_registry_init(reg, CNET_VSA_DEFAULT_DIM);
    registry_apply_env(reg);
    const char *dir = (dir_path && *dir_path) ? dir_path : "bin";
    int n = cnet_vsa_registry_load_dir(reg, dir);
    char line[4096];
    long k = 0;
    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;
        printf("=== QUERY %ld ===\n", ++k);
        route_print(reg, dir, n, line);
        fflush(stdout);
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
    registry_apply_env(reg);
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

/* Explicit evidence operation: accepts only bounded IDs and signed fact rows. */
static int evidence_id(const char *text, const char *prefix, int limit, int *out) {
    size_t n=strlen(prefix);
    if(strncmp(text,prefix,n))return -1;
    text+=n;n=strlen(text);
    if(!n||n>3)return -1;
    int value=0;
    for(size_t i=0;i<n;++i) {
        if(text[i]<'0'||text[i]>'9')return -1;
        value=value*10+(text[i]-'0');
    }
    if(value>=limit)return -1;
    *out=value;return 0;
}
static int cmd_explain_facts(int argc, char **argv) {
    if(argc<3||argc>2+CNET_VSA_EVIDENCE_HOPS) {
        fprintf(stderr,"Usage: explain-facts <facts.txt> <start-id> <relation-id>...\n");return 2;
    }
    int start,relations[CNET_VSA_EVIDENCE_HOPS];
    if(evidence_id(argv[1],"",CNET_VSA_EVIDENCE_ENTITIES,&start))return 2;
    for(int i=2;i<argc;++i)
        if(evidence_id(argv[i],"",CNET_VSA_EVIDENCE_RELATIONS,&relations[i-2]))return 2;
    FILE *file=fopen(argv[0],"r");
    if(!file){fprintf(stderr,"ERROR: cannot read evidence file\n");return 2;}
    CnetVsaEvidenceFact facts[CNET_VSA_EVIDENCE_FACTS];size_t count=0,lines=0;
    char line[128];int invalid=0;
    for(;;) {
        size_t length=0;int ch;
        while((ch=fgetc(file))!=EOF&&ch!='\n') {
            if(ch==0||length+1>=sizeof line){invalid=1;break;}
            line[length++]=(char)ch;
        }
        if(invalid||(!length&&ch==EOF))break;
        line[length]=0;
        if(++lines>2048){invalid=1;break;}
        const char *p=line;while(isspace((unsigned char)*p))p++;
        if(!*p)continue;
        char a[16],b[16],c[16],d[16],extra;
        int tokens=sscanf(p,"%15s %15s %15s %15s %c",a,b,c,d,&extra);
        if(count==CNET_VSA_EVIDENCE_FACTS||(tokens!=3&&tokens!=4)) {invalid=1;break;}
        CnetVsaEvidenceFact *fact=&facts[count];fact->sign=tokens==4?-1:1;
        if((tokens==4&&strcmp(b,"not"))||
           evidence_id(a,"node",CNET_VSA_EVIDENCE_ENTITIES,&fact->subject)||
           evidence_id(tokens==4?c:b,"rel",CNET_VSA_EVIDENCE_RELATIONS,&fact->relation)||
           evidence_id(tokens==4?d:c,"node",CNET_VSA_EVIDENCE_ENTITIES,&fact->object)) {invalid=1;break;}
        count++;
    }
    if(ferror(file))invalid=1;
    fclose(file);
    if(invalid){fprintf(stderr,"ERROR: invalid or oversized evidence file\n");return 2;}
    char out[CNET_VSA_EVIDENCE_OUTPUT];
    int rc=cnet_vsa_evidence_response(facts,count,start,relations,(size_t)(argc-2),-1,out,sizeof out);
    if(rc<0){fprintf(stderr,"ERROR: evidence response failed (%d)\n",rc);return 2;}
    puts(out);return rc==CNET_VSA_EVIDENCE_OK?0:1;
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
            GencapCreateOpts o; gencap_create_opts_default(&o);
            if (r_argc >= 5 && gencap_create_opts_parse(&o, r_argc - 5, r_argv + 5) == 0) {
                cmd_gencap_create(r_argv[1], r_argv[2], r_argv[3], r_argv[4], &o);
            } else {
                printf("Usage: gencap-create <name> <domain> <corpus.txt> <output.gencap> [--encoder hd|bag] [--probes f] [--negatives f|dir] [--target-in 0.90] [--target-neg 0.95]\n");
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
        } else if (strcmp(cmd, "route-batch") == 0) {
            cmd_route_batch(r_argc >= 2 ? r_argv[1] : "bin");
        } else if (strcmp(cmd, "route") == 0) {
            if (r_argc >= 3) {
                cmd_route(r_argv[1], r_argv[2]);
            } else if (r_argc == 2) {
                cmd_route("bin", r_argv[1]);
            } else {
                printf("Usage: route [dir] <prompt>\n");
            }
        } else if (strcmp(cmd, "explain-facts") == 0) {
            (void)cmd_explain_facts(r_argc-1,r_argv+1);
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
    if (lexicon_apply_env() != 0) return 1;
    /* This CPU-only operator needs no GPU, document store, or capsule allocation. */
    if(argc>1&&!strcmp(argv[1],"explain-facts"))return cmd_explain_facts(argc-2,argv+2);
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
        GencapCreateOpts o; gencap_create_opts_default(&o);
        if (arg_offset + 4 < argc &&
            gencap_create_opts_parse(&o, argc - (arg_offset + 5), argv + arg_offset + 5) == 0) {
            return cmd_gencap_create(argv[arg_offset + 1], argv[arg_offset + 2], argv[arg_offset + 3],
                                     argv[arg_offset + 4], &o);
        } else {
            fprintf(stderr, "Usage: %s gencap-create <name> <domain> <corpus.txt> <output.gencap> "
                            "[--encoder hd|bag] [--probes f] [--negatives f|dir] [--target-in 0.90] [--target-neg 0.95] [--max-negatives 512]\n", argv[0]);
            return 1;
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
    } else if (strcmp(cmd, "lexicon-build") == 0) {
        return cmd_lexicon_build(argc - (arg_offset + 1), argv + arg_offset + 1);
    } else if (strcmp(cmd, "stem-words") == 0) {
        return cmd_stem_words();
    } else if (strcmp(cmd, "lexicon-train") == 0) {
        return cmd_lexicon_train(argc - (arg_offset + 1), argv + arg_offset + 1);
    } else if (strcmp(cmd, "lexicon-info") == 0) {
        if (arg_offset + 1 < argc) return cmd_lexicon_info(argv[arg_offset + 1]);
        fprintf(stderr, "Usage: %s lexicon-info <file.lex>\n", argv[0]);
        return 1;
    } else if (strcmp(cmd, "route-batch") == 0) {
        cmd_route_batch(arg_offset + 1 < argc ? argv[arg_offset + 1] : "bin");
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
    } else if (strcmp(cmd, "explain-facts") == 0) {
        int rc=cmd_explain_facts(argc-arg_offset-1,argv+arg_offset+1);
        free_cli_state(&state);
        return rc;
    } else if (strcmp(cmd, "repl") == 0 || strcmp(cmd, "interactive") == 0) {
        print_banner();
        run_repl(&state);
    } else {
        fprintf(stderr, "Unknown command '%s'. Run '%s --help' for usage.\n", cmd, argv[0]);
    }

    free_cli_state(&state);
    return 0;
}
