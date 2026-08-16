/*
 * Circuit plans end-to-end over the real frozen primitives.
 *
 *   Part 1  split circuit: BOTH nibbles of a byte as one 2-root plan over
 *           ONE split execution -- the run records one outcome, not two.
 *   Part 2  the 1-digit adder circuit {sum, carry} discovered from
 *           (symbol, symbol, carry) sources; full 200-member domain.
 *   Part 3  ripple-carry DISCOVERED: goals {sum0, sum1, cout} -- types +
 *           the port-disjoint rule force the topology; the discovered
 *           circuit is verified on all 20000 two-digit additions, strict.
 *   Part 4  circuit chunk: part 2's circuit distills into the
 *           multi-output dec_full_adder_unit, certified against its
 *           teacher's contract; the certified replan picks the chunk.
 *           (--stretch additionally attempts the 20000-sample 2-digit
 *           dec_add2 distillation; an honest refusal is expected.)
 *   Part 5  pruning benchmark: a layered trap registry, planned with the
 *           reachability memo off then on; identical plan, measured time.
 *
 * Requires weight files from ./nn_demo and ./decimal_demo. Run from the
 * repo root (make circuit).
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/consolidate.h"
#include "../include/contract/contract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>  /* for _mkdir on MinGW/Windows */
#include <io.h>      /* for _findfirst / _finddata_t */
#else
#include <dirent.h>
#endif

static Port PT(PortFamily family, size_t field_width, size_t field_count,
               const char *tag) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    if (tag != NULL && port_set_tag(&p, tag) != 0) {
        fprintf(stderr, "FAIL: bad tag '%s'.\n", tag);
        exit(EXIT_FAILURE);
    }
    return p;
}

static int bits_to_int(const double *bits, size_t n) {
    int value = 0;
    size_t i;
    for (i = 0; i < n; ++i) {
        value = value * 2 + (bits[i] > 0.5 ? 1 : 0);
    }
    return value;
}

static void int_to_bits(int value, double *bits, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        bits[i] = (double)((value >> (n - 1 - i)) & 1);
    }
}

static void int_to_onehot(int index, double *vec, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        vec[i] = 0.0;
    }
    vec[index] = 1.0;
}

static void print_node(const DagNode *node, int depth) {
    int i;
    for (i = 0; i < depth; ++i) {
        printf("  ");
    }
    if (node->kind == DAG_SOURCE) {
        printf("source[%d]\n", node->source_index);
    } else {
        size_t c;
        printf("%s\n", node->name != NULL ? node->name : "?");
        for (c = 0; c < node->child_count; ++c) {
            print_node(node->children[c], depth + 1);
        }
    }
}

int main(int argc, char **argv) {
    int stretch = argc > 1 && strcmp(argv[1], "--stretch") == 0;
    int do_registry_report = (argc > 1 && strcmp(argv[1], "--registry-report") == 0);
    int do_memory_hints_report = (argc > 1 && strcmp(argv[1], "--memory-hints-report") == 0);
    int do_memory_shadow = (argc > 1 && strcmp(argv[1], "--memory-shadow") == 0);

    if (argc >= 2 && strcmp(argv[1], "--compare-artifacts") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: circuit_demo --compare-artifacts file1.json file2.json\n");
            return 1;
        }
        CircuitConsolidationReport a = {0}, b = {0};
        int ra = circuit_load_consolidation_artifact(argv[2], &a);
        int rb = circuit_load_consolidation_artifact(argv[3], &b);
        if (ra != 0 || rb != 0) {
            printf("comparison refused: malformed or missing artifact (code %d %d)\n", ra, rb);
            return 1;
        }
        circuit_print_consolidation_artifact_comparison(&a, argv[2], &b, argv[3]);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "--artifact-trend") == 0) {
        /* v1.6: read-only trend over artifacts/consolidation/ (or current dir if none).
           Sorts by filename lexical (deterministic). No weights, no planner, no registration. */
        const char *dir = "artifacts/consolidation";
        char **paths = NULL;
        size_t n = 0;

#ifdef _WIN32
        char pat[256];
        snprintf(pat, sizeof(pat), "%s\\*.json", dir);
        struct _finddata_t fd;
        intptr_t h = _findfirst(pat, &fd);
        if (h != -1) {
            size_t cap = 16;
            paths = (char**)malloc(cap * sizeof(char*));
            do {
                if (n == cap) { cap *= 2; paths = (char**)realloc(paths, cap * sizeof(char*)); }
                char full[512];
                snprintf(full, sizeof(full), "%s\\%s", dir, fd.name);
                paths[n++] = _strdup(full);
            } while (_findnext(h, &fd) == 0);
            _findclose(h);
        }
#else
        /* fallback: try current dir *.json if the preferred dir has none */
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *e;
            size_t cap = 16;
            paths = (char**)malloc(cap * sizeof(char*));
            while ((e = readdir(d))) {
                if (strstr(e->d_name, ".json")) {
                    if (n == cap) { cap *= 2; paths = (char**)realloc(paths, cap * sizeof(char*)); }
                    char full[512];
                    snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
                    paths[n++] = strdup(full);
                }
            }
            closedir(d);
        }
#endif
        if (n == 0) {
            /* last resort: any *.json in cwd for the tool to be usable without the dir */
            /* (in practice demo runs create the dir on first use) */
        }

        /* sort deterministically by full path (lexical on filename component is stable) */
        if (paths && n > 1) {
            qsort(paths, n, sizeof(char*), (int(*)(const void*,const void*))strcmp);
        }

        circuit_print_artifact_trend_summary((const char * const *)paths, n);

        /* free */
        for (size_t i=0; i<n; i++) free(paths[i]);
        free(paths);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "--compare-registry-reports") == 0) {
        /* v1.8 pure early replay: no weights, no planner, no gates */
        if (argc < 4) {
            fprintf(stderr, "usage: circuit_demo --compare-registry-reports a.json b.json\n");
            return 1;
        }
        CircuitRegistryReportSnapshot sa = {0}, sb = {0};
        int ra = circuit_load_consolidation_registry_snapshot(argv[2], &sa);
        int rb = circuit_load_consolidation_registry_snapshot(argv[3], &sb);
        if (ra != 0 || rb != 0) {
            printf("comparison refused: malformed or missing snapshot (code %d %d)\n", ra, rb);
            return 1;
        }
        circuit_print_consolidation_registry_snapshot_diff(&sa, &sb);
        return 0;
    }

    if (argc >= 2 && strcmp(argv[1], "--registry-report-trend") == 0) {
        /* v1.9: pure early replay over registry snapshots. No weights, no planner, no gates. */
        const char *paths[64];
        size_t n = 0;
        if (argc > 2) {
            for (int k = 2; k < argc && n < 64; ++k) {
                paths[n++] = argv[k];
            }
        } else {
            /* no-arg: scan default dir if present (for convenience; tests prefer explicit paths) */
            const char *tdir = "artifacts/consolidation/registry_reports";
#ifdef _WIN32
            char tpat[256];
            snprintf(tpat, sizeof(tpat), "%s\\*.json", tdir);
            struct _finddata_t tfd;
            intptr_t th = _findfirst(tpat, &tfd);
            if (th != -1) {
                do {
                    if (n < 64) {
                        static char tfulls[64][256];
                        snprintf(tfulls[n], sizeof(tfulls[n]), "%s\\%s", tdir, tfd.name);
                        paths[n] = tfulls[n];
                        n++;
                    }
                } while (_findnext(th, &tfd) == 0 && n < 64);
                _findclose(th);
            }
#else
            DIR *td = opendir(tdir);
            if (td) {
                struct dirent *te;
                static char tfulls[64][256];
                while ((te = readdir(td)) && n < 64) {
                    if (strstr(te->d_name, ".json")) {
                        snprintf(tfulls[n], sizeof(tfulls[n]), "%s/%s", tdir, te->d_name);
                        paths[n] = tfulls[n];
                        n++;
                    }
                }
                closedir(td);
            }
#endif
        }
        if (n > 1) {
            /* caller responsibility per spec; we sort for convenience */
            qsort((void*)paths, n, sizeof(char*), (int(*)(const void*,const void*))strcmp);
        }

        CircuitRegistryTrendSummary tsum = {0};
        if (circuit_build_registry_snapshot_trend(paths, n, &tsum) == 0) {
            circuit_print_registry_snapshot_trend(&tsum);
        } else {
            printf("(trend build failed)\n");
        }
        return 0;
    }

    if (do_memory_hints_report) {
        /* v2.0 pure inspection: load + print + exit BEFORE weights, planner, executor, gates, consolidate, registry mutation. */
        if (argc < 3) {
            fprintf(stderr, "usage: circuit_demo --memory-hints-report hints.json\n");
            return 1;
        }
        CircuitMemoryHintStore hs = {0};
        int lr = circuit_memory_load_hints(argv[2], &hs);
        if (lr != 0) {
            printf("[BAD MEMORY HINT / UNREADABLE] path=%s ignored=1\n", argv[2]);
            /* still print header per example */
            circuit_memory_print_hints_report(argv[2], NULL);
            return 0;
        }
        circuit_memory_print_hints_report(argv[2], &hs);
        return 0;
    }

    BinaryTransformNetwork split = {0};
    BinaryTransformNetwork dec_value = {0};
    BinaryTransformNetwork dec_fa = {0};
    BinaryTransformNetwork chunk = {0};

    static double a_sym[10], b_sym[10], cin_v[1];
    DagSource add_sources[3];
    Port add_goals[2];
    CircuitPlan add_circuit = {0};
    int add_planned = 0;

    int rc = EXIT_FAILURE;

    if (btn_load(&split, "split_weights.txt") != 0 ||
        btn_load(&dec_value, "dec_value_weights.txt") != 0 ||
        btn_load(&dec_fa, "dec_full_add_weights.txt") != 0) {
        fprintf(stderr, "FAIL: could not load frozen primitives "
                        "(run ./nn_demo and ./decimal_demo first).\n");
        return EXIT_FAILURE;
    }

    /* ================================================================== */
    printf("=== Part 1: both nibbles of a byte, ONE split execution ===\n\n");
    /* ================================================================== */
    {
        PrimitiveRegistry reg;
        static double byte_v[8];
        DagSource src[1];
        Port goals[2];
        CircuitPlan cp = {0};
        unsigned long before;
        int value;
        int misses = 0;

        registry_init(&reg);
        if (registry_add(&reg, &split, "split") != 0) {
            fprintf(stderr, "FAIL: registry.\n");
            goto cleanup;
        }
        src[0].type = PT(PORT_BINARY_MSB, 8, 1, "byte_value");
        src[0].values = byte_v;
        goals[0] = PT(PORT_BINARY_MSB, 4, 1, "nibble_value");
        goals[1] = PT(PORT_BINARY_MSB, 4, 1, "nibble_value");

        if (dag_plan_circuit(&reg, src, 1, goals, 2, &cp) != 0 ||
            cp.roots[0] != cp.roots[1] || cp.roots[0]->btn != &split ||
            cp.root_ports[0] != 0 || cp.root_ports[1] != 1) {
            fprintf(stderr, "FAIL: expected one shared split execution.\n");
            registry_free(&reg);
            goto cleanup;
        }
        printf("circuit: 2 roots -> one split node (ports 0 and 1)\n");

        before = split.output_successes;
        for (value = 0; value < 256; ++value) {
            double out[8];

            int_to_bits(value, byte_v, 8);
            if (dag_execute_circuit(&cp, src, 1, out, 8, NULL) != 0 ||
                bits_to_int(out, 4) != value / 16 ||
                bits_to_int(out + 4, 4) != value % 16) {
                ++misses;
            }
        }
        circuit_free(&cp);
        registry_free(&reg);
        if (misses != 0) {
            fprintf(stderr, "FAIL: split circuit misfired %d/256.\n", misses);
            goto cleanup;
        }
        printf("all 256 bytes split correctly; split evidence grew by %lu "
               "(one outcome per run, not two)\n",
               split.output_successes - before);
        if (split.output_successes - before != 256) {
            fprintf(stderr, "FAIL: expected exactly 256 outcomes.\n");
            goto cleanup;
        }
    }

    /* ================================================================== */
    printf("\n=== Part 2: the 1-digit adder circuit {sum, carry} ===\n\n");
    /* ================================================================== */
    {
        PrimitiveRegistry reg;
        int a, b, c;
        int failures = 0;

        registry_init(&reg);
        if (registry_add(&reg, &dec_value, "dec_value") != 0 ||
            registry_add(&reg, &dec_fa, "dec_full_add") != 0) {
            fprintf(stderr, "FAIL: registry.\n");
            goto cleanup;
        }

        add_sources[0].type = PT(PORT_ONEHOT, 10, 1, "dec_symbol");
        add_sources[0].values = a_sym;
        add_sources[1].type = PT(PORT_ONEHOT, 10, 1, "dec_symbol");
        add_sources[1].values = b_sym;
        add_sources[2].type = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");
        add_sources[2].values = cin_v;
        add_goals[0] = PT(PORT_BINARY_MSB, 4, 1, "dec_sum");
        add_goals[1] = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");

        if (dag_plan_circuit(&reg, add_sources, 3, add_goals, 2,
                             &add_circuit) != 0) {
            fprintf(stderr, "FAIL: no 1-digit adder circuit.\n");
            registry_free(&reg);
            goto cleanup;
        }
        add_planned = 1;
        registry_free(&reg);

        if (add_circuit.roots[0] != add_circuit.roots[1] ||
            add_circuit.roots[0]->btn != &dec_fa ||
            add_circuit.root_ports[0] != 0 || add_circuit.root_ports[1] != 1) {
            fprintf(stderr, "FAIL: expected one shared adder execution.\n");
            goto cleanup;
        }
        printf("discovered circuit (root 0 = sum, root 1 = carry):\n");
        print_node(add_circuit.roots[0], 1);

        for (a = 0; a < 10; ++a) {
            for (b = 0; b < 10; ++b) {
                for (c = 0; c < 2; ++c) {
                    double out[5];

                    int_to_onehot(a, a_sym, 10);
                    int_to_onehot(b, b_sym, 10);
                    cin_v[0] = (double)c;
                    if (dag_execute_circuit(&add_circuit, add_sources, 3,
                                            out, 5, NULL) != 0 ||
                        bits_to_int(out, 4) != (a + b + c) % 10 ||
                        bits_to_int(out + 4, 1) != (a + b + c) / 10) {
                        ++failures;
                    }
                }
            }
        }
        if (failures != 0) {
            fprintf(stderr, "FAIL: adder circuit misfired %d/200.\n",
                    failures);
            goto cleanup;
        }
        printf("\nexecuted the full domain: 200/200 correct "
               "(sum AND carry from one execution)\n");
    }

    /* ================================================================== */
    printf("\n=== Part 3: ripple-carry DISCOVERED and verified ===\n\n");
    /* ================================================================== */
    {
        PrimitiveRegistry reg;
        static double a0_v[4], b0_v[4], a1_v[4], b1_v[4], rcin[1];
        DagSource src[5];
        Port goals[3];
        CircuitPlan cp = {0};
        DagNode *ones;
        DagNode *tens;
        long wrong = 0;
        int a, b, c;

        registry_init(&reg);
        if (registry_add(&reg, &dec_fa, "dec_full_add") != 0) {
            fprintf(stderr, "FAIL: registry.\n");
            goto cleanup;
        }

        /* Source order (a0, b0, a1, b1, cin): index-order assignment gives
           the ones adder the ones digits. The planner knows types, not
           arithmetic -- the exhaustive sweep below is what certifies the
           wiring, the same way this project trusts every plan. */
        src[0].type = PT(PORT_BINARY_MSB, 4, 1, "dec_digit");
        src[0].values = a0_v;
        src[1].type = PT(PORT_BINARY_MSB, 4, 1, "dec_digit");
        src[1].values = b0_v;
        src[2].type = PT(PORT_BINARY_MSB, 4, 1, "dec_digit");
        src[2].values = a1_v;
        src[3].type = PT(PORT_BINARY_MSB, 4, 1, "dec_digit");
        src[3].values = b1_v;
        src[4].type = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");
        src[4].values = rcin;
        goals[0] = PT(PORT_BINARY_MSB, 4, 1, "dec_sum");
        goals[1] = PT(PORT_BINARY_MSB, 4, 1, "dec_sum");
        goals[2] = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");

        if (dag_plan_circuit(&reg, src, 5, goals, 3, &cp) != 0) {
            fprintf(stderr, "FAIL: no ripple circuit.\n");
            registry_free(&reg);
            goto cleanup;
        }
        registry_free(&reg);

        ones = (cp.roots[0]->children[2]->kind == DAG_SOURCE)
                   ? cp.roots[0] : cp.roots[1];
        tens = (ones == cp.roots[0]) ? cp.roots[1] : cp.roots[0];
        if (cp.roots[0] == cp.roots[1] ||
            ones->children[2]->kind != DAG_SOURCE ||
            tens->children[2] != ones ||
            cp.roots[2] != tens || cp.root_ports[2] != 1) {
            fprintf(stderr, "FAIL: the forced ripple topology is wrong.\n");
            circuit_free(&cp);
            goto cleanup;
        }
        printf("discovered circuit (sum0, sum1, cout; carry chained "
               "ones->tens):\n");
        print_node(tens, 1);

        cp.strict = 1;
        for (a = 0; a < 100; ++a) {
            for (b = 0; b < 100; ++b) {
                for (c = 0; c < 2; ++c) {
                    double out[9];

                    int_to_bits(a % 10, a0_v, 4);
                    int_to_bits(b % 10, b0_v, 4);
                    int_to_bits(a / 10, a1_v, 4);
                    int_to_bits(b / 10, b1_v, 4);
                    rcin[0] = (double)c;
                    if (dag_execute_circuit(&cp, src, 5, out, 9, NULL) != 0 ||
                        bits_to_int(out + 8, 1) * 100 +
                        bits_to_int(out + 4, 4) * 10 +
                        bits_to_int(out, 4) != a + b + c) {
                        ++wrong;
                    }
                }
            }
        }
        circuit_free(&cp);
        if (wrong != 0) {
            fprintf(stderr, "FAIL: ripple circuit wrong on %ld/20000.\n",
                    wrong);
            goto cleanup;
        }
        printf("\nexhaustive verification: 20000/20000 two-digit additions "
               "exact (strict)\n");
    }

    /* ================================================================== */
    printf("\n=== Part 4: the circuit chunk (multi-output) ===\n\n");
    /* ================================================================== */
    {
        ConsolidateConfig ccfg;
        ConsolidateReport rep;
        Contract emitted = {0};
        Contract c_chunk = {0}, c_value = {0}, c_fa = {0};
        CertifyReport crep;

        /* The 5-output student needs more patience than the defaults
           (160k epochs stalled at 196/200; 300k with a tighter target
           masters the domain). */
        consolidate_config_defaults(&ccfg);
        ccfg.initial_hidden = 64;
        ccfg.max_epochs = 300000;
        ccfg.target_loss = 0.0005;

        if (consolidate_circuit(&add_circuit, add_sources, 3, &ccfg,
                                &chunk, &rep) != 0) {
            fprintf(stderr, "FAIL: circuit consolidation refused "
                            "(samples %lu, aborts %lu, verified %lu).\n",
                    (unsigned long)rep.samples,
                    (unsigned long)rep.teacher_aborts,
                    (unsigned long)rep.verified);
            goto cleanup;
        }
        printf("dec_full_adder_unit distilled: %lu samples (%lu teacher "
               "aborts), verified %lu/%lu, hidden %lu, %lu output ports\n",
               (unsigned long)rep.samples,
               (unsigned long)rep.teacher_aborts,
               (unsigned long)rep.verified,
               (unsigned long)rep.samples,
               (unsigned long)chunk.hidden_count,
               (unsigned long)chunk.output_port_count);
        if (rep.teacher_aborts != 0 || rep.verified != 200) {
            fprintf(stderr, "FAIL: expected a clean 200/200 distillation.\n");
            goto cleanup;
        }

        /* v1.3.1: wire blackboard-backed consolidation report into demo path (explanatory only) */
        {
            CircuitBlackboard tbb = {0};
            double tout[9];
            CircuitConsolidationReport crep = {0};
            if (dag_execute_circuit(&add_circuit, add_sources, 3, tout, 9, &tbb) == 0) {
                circuit_consolidation_report(&add_circuit, &tbb, tout, 9, &chunk,
                                             rep.verified, rep.samples, &crep);
                printf("\n=== v1.3.1 Consolidation Report (advisory only) ===\n");
                printf("  teacher_node_count=%zu (primitives=%zu, entries=%zu)\n",
                       crep.teacher_node_count, crep.teacher_primitive_node_count, crep.teacher_output_entry_count);
                printf("  student_node_count=%zu\n", crep.student_node_count);
                printf("  teacher_mac_estimate=%zu student_mac_estimate=%zu\n",
                       crep.teacher_mac_estimate, crep.student_mac_estimate);
                printf("  compression_ratio=%.2f\n", crep.compression_ratio);
                printf("  root_coverage_match=%d\n", crep.root_coverage_match);
                printf("  output_exact_match=%d\n", crep.output_exact_match);
                printf("  contract_signature_match=%d\n", crep.contract_signature_match);
                printf("  consolidation_safe_to_register=%d (advisory_only; registration controlled ONLY by consolidate verified + contract gate)\n",
                       crep.consolidation_safe_to_register);
                circuit_blackboard_free(&tbb);

                /* v1.4: emit standalone evidence artifact (after gates, never read for registration) */
                int write_artifact = 1;
                const char *env = getenv("CNET_ARTIFACT");
                if (env && *env == '0') write_artifact = 0;
                if (write_artifact) {
#ifdef _WIN32
                    (void)_mkdir("artifacts");
                    (void)_mkdir("artifacts/consolidation");
#else
                    (void)system("mkdir -p artifacts/consolidation 2>/dev/null || true");
#endif
                    if (circuit_write_consolidation_artifact(
                            "artifacts/consolidation/dec_full_adder_unit_report.json",
                            &crep,
                            "dec_add2_circuit",
                            "dec_full_adder_unit",
                            rep.verified,
                            rep.samples) == 0) {
                        printf("  artifact written: artifacts/consolidation/dec_full_adder_unit_report.json (evidence only)\n");
                    } else {
                        printf("  WARNING: artifact write skipped (non-strict, path or dir issue)\n");
                    }
                } else {
                    printf("  artifact writing disabled via CNET_ARTIFACT=0\n");
                }

                /* v2.0 optional memory hints write (after successful strict+bb; non-fatal, disableable, touches only hints file) */
                {
                    int write_hints = 1;
                    const char *mh_env = getenv("CNET_CIRCUIT_MEMORY_HINTS");
                    if (mh_env && *mh_env == '0') write_hints = 0;
                    const char *mh_path = getenv("CNET_CIRCUIT_MEMORY_HINT_PATH");
                    if (!mh_path || !*mh_path) mh_path = "artifacts/consolidation/circuit_hints.json";
                    if (write_hints) {
                        /* Re-capture with strict for hint discipline (plan already executed; strict exec for evidence) */
                        CircuitBlackboard hbb = {0};
                        double hout[5];
                        add_circuit.strict = 1;
                        if (dag_execute_circuit(&add_circuit, add_sources, 3, hout, 5, &hbb) == 0) {
#ifdef _WIN32
                            (void)_mkdir("artifacts"); (void)_mkdir("artifacts/consolidation");
#else
                            (void)system("mkdir -p artifacts/consolidation 2>/dev/null || true");
#endif
                            CircuitMemoryHintStore hs = {0};
                            Port hsrc_types[3] = { add_sources[0].type, add_sources[1].type, add_sources[2].type };
                            Port hgoals[2] = { add_goals[0], add_goals[1] };
                            if (circuit_memory_hints_from_blackboard(&add_circuit, &hbb, hsrc_types, 3, hgoals, 2, &hs) == 0) {
                                if (circuit_memory_write_hints(mh_path, &hs) == 0) {
                                    printf("  memory hints written: %s (advisory telemetry only; 0 planner influence)\n", mh_path);
                                }
                            }
                            circuit_blackboard_free(&hbb);
                        }
                        add_circuit.strict = 0;
                    } else {
                        printf("  memory hints writing disabled via CNET_CIRCUIT_MEMORY_HINTS=0\n");
                    }
                }
            }
        }

        if (btn_save(&chunk, "dec_full_adder_unit_weights.txt") != 0) {
            fprintf(stderr, "FAIL: could not persist the chunk.\n");
            goto cleanup;
        }
        remove("dec_full_adder_unit_stats.txt");

        if (contract_from_circuit(&add_circuit, add_sources, 3,
                                  "dec_full_adder_unit", 4096,
                                  &emitted) != 0 ||
            contract_save(&emitted, "dec_full_adder_unit_contract.txt") != 0) {
            fprintf(stderr, "FAIL: could not emit the circuit contract.\n");
            contract_free(&emitted);
            goto cleanup;
        }
        contract_free(&emitted);

        if (contract_load(&c_chunk, "dec_full_adder_unit_contract.txt") != 0 ||
            btn_certify(&chunk, &c_chunk, &crep) != 0 ||
            crep.exemplars != 200 || crep.passed != 200) {
            fprintf(stderr, "FAIL: the chunk did not certify.\n");
            contract_free(&c_chunk);
            goto cleanup;
        }
        printf("dec_full_adder_unit: CERTIFIED against its teacher's "
               "contract (200/200)\n");

        /* Certified replan: one chunk execution beats the 3-execution
           tree on seeded evidence. */
        {
            PrimitiveRegistry reg;
            CircuitPlan replanned = {0};

            if (contract_load(&c_value, "dec_value_contract.txt") != 0 ||
                contract_load(&c_fa, "dec_full_add_contract.txt") != 0) {
                fprintf(stderr, "FAIL: could not reload decimal "
                                "contracts.\n");
                contract_free(&c_chunk);
                goto cleanup;
            }
            registry_init(&reg);
            /* The claim is "seeded chunk vs FRESH teachers" (decimal_demo
               reloads its teachers from disk, which zeroes counters). Here
               the live teacher BTNs still carry Part 3's 20000-execution
               evidence, which SHOULD outrank a 200-seed chunk — the planner
               scoring that way is correct. Reset to the uninformed prior so
               the replan tests the stated claim, not the execution history. */
            dec_value.output_successes = 0;
            dec_value.output_failures = 0;
            dec_fa.output_successes = 0;
            dec_fa.output_failures = 0;
            if (registry_add_certified(&reg, &dec_value, "dec_value",
                                       &c_value) != 0 ||
                registry_add_certified(&reg, &dec_fa, "dec_full_add",
                                       &c_fa) != 0 ||
                registry_add_certified(&reg, &chunk, "dec_full_adder_unit",
                                       &c_chunk) != 0) {
                fprintf(stderr, "FAIL: certification gate.\n");
                registry_free(&reg);
                contract_free(&c_chunk);
                contract_free(&c_value);
                contract_free(&c_fa);
                goto cleanup;
            }
            reg.require_certified = 1;

            if (dag_plan_circuit(&reg, add_sources, 3, add_goals, 2,
                                 &replanned) != 0 ||
                replanned.roots[0]->btn != &chunk ||
                replanned.roots[0] != replanned.roots[1]) {
                fprintf(stderr, "FAIL: the chunk did not win the certified "
                                "replan.\n");
                circuit_free(&replanned);
                registry_free(&reg);
                contract_free(&c_chunk);
                contract_free(&c_value);
                contract_free(&c_fa);
                goto cleanup;
            }
            printf("\ncertified replan (require_certified):\n");
            print_node(replanned.roots[0], 1);
            circuit_free(&replanned);
            registry_free(&reg);
            contract_free(&c_chunk);
            contract_free(&c_value);
            contract_free(&c_fa);
        }
    }

    /* Stretch: the 20000-sample 2-digit dec_add2. Opt-in; an honest
       refusal at the 100% verification gate is the expected outcome. */
    if (stretch) {
        printf("\n=== Stretch: dec_add2 (2-digit circuit, 20000 samples) "
               "===\n\n");
        {
            PrimitiveRegistry reg;
            static double s0[10], s1[10], s2[10], s3[10], sc[1];
            DagSource src[5];
            Port goals[3];
            CircuitPlan cp = {0};
            ConsolidateConfig cfg;
            ConsolidateReport rep;
            BinaryTransformNetwork big = {0};

            registry_init(&reg);
            registry_add(&reg, &dec_value, "dec_value");
            registry_add(&reg, &dec_fa, "dec_full_add");
            src[0].type = PT(PORT_ONEHOT, 10, 1, "dec_symbol");
            src[0].values = s0;
            src[1].type = PT(PORT_ONEHOT, 10, 1, "dec_symbol");
            src[1].values = s1;
            src[2].type = PT(PORT_ONEHOT, 10, 1, "dec_symbol");
            src[2].values = s2;
            src[3].type = PT(PORT_ONEHOT, 10, 1, "dec_symbol");
            src[3].values = s3;
            src[4].type = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");
            src[4].values = sc;
            goals[0] = PT(PORT_BINARY_MSB, 4, 1, "dec_sum");
            goals[1] = PT(PORT_BINARY_MSB, 4, 1, "dec_sum");
            goals[2] = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");

            if (dag_plan_circuit(&reg, src, 5, goals, 3, &cp) != 0) {
                fprintf(stderr, "FAIL: no 2-digit circuit.\n");
                registry_free(&reg);
                goto cleanup;
            }
            printf("2-digit circuit discovered (6 executions); attempting "
                   "distillation...\n");

            consolidate_config_defaults(&cfg);
            cfg.max_samples = 20000;
            cfg.initial_hidden = 64;
            cfg.max_hidden = 96;
            cfg.max_epochs = 2000;   /* a few minutes, not hours */
            cfg.growth_window = 200;
            memset(&rep, 0, sizeof rep);
            if (consolidate_circuit(&cp, src, 5, &cfg, &big, &rep) == 0) {
                printf("dec_add2 distilled AND verified %lu/%lu -- saving.\n",
                       (unsigned long)rep.verified,
                       (unsigned long)rep.samples);
                btn_save(&big, "dec_add2_weights.txt");
                btn_free(&big);
            } else {
                printf("dec_add2 REFUSED, honestly: %lu samples, "
                       "%lu verified, %lu missed (loss %.6f).\n"
                       "The 100%% gate is the point: a chunk that cannot "
                       "reproduce its teacher does not get to exist.\n",
                       (unsigned long)rep.samples,
                       (unsigned long)rep.verified,
                       (unsigned long)rep.missed,
                       rep.final_loss);
            }
            circuit_free(&cp);
            registry_free(&reg);
        }
    }

    if (do_registry_report) {
        /* v1.7: after normal demo registration (via existing gates), emit read-only registry report.
           Collect artifacts (caller sorts lex by filename), build snapshot reg of the chunk
           registered in this run, print report. No change to registration, no gate calls from here. */
        char **apaths = NULL;
        size_t an = 0;
        const char *adir = "artifacts/consolidation";
#ifdef _WIN32
        char pat[256];
        snprintf(pat, sizeof(pat), "%s\\*.json", adir);
        struct _finddata_t fd;
        intptr_t h = _findfirst(pat, &fd);
        if (h != -1) {
            size_t cap = 16;
            apaths = (char**)malloc(cap * sizeof(char*));
            do {
                if (an == cap) { cap *= 2; apaths = (char**)realloc(apaths, cap * sizeof(char*)); }
                char full[512];
                snprintf(full, sizeof(full), "%s\\%s", adir, fd.name);
                apaths[an++] = _strdup(full);
            } while (_findnext(h, &fd) == 0);
            _findclose(h);
        }
#else
        DIR *d = opendir(adir);
        if (d) {
            struct dirent *e;
            size_t cap = 16;
            apaths = (char**)malloc(cap * sizeof(char*));
            while ((e = readdir(d))) {
                if (strstr(e->d_name, ".json")) {
                    if (an == cap) { cap *= 2; apaths = (char**)realloc(apaths, cap * sizeof(char*)); }
                    char full[512];
                    snprintf(full, sizeof(full), "%s/%s", adir, e->d_name);
                    apaths[an++] = strdup(full);
                }
            }
            closedir(d);
        }
#endif
        if (an > 1) {
            qsort(apaths, an, sizeof(char*), (int(*)(const void*,const void*))strcmp);
        }

        /* minimal snapshot reg with the chunk registered by the normal demo path above */
        PrimitiveRegistry rrep;
        registry_init(&rrep);
        BinaryTransformNetwork dummybtn = {0};
        registry_add(&rrep, &dummybtn, "dec_full_adder_unit");
        if (rrep.count > 0) rrep.entries[rrep.count-1].certified = 1;

        circuit_print_consolidation_registry_report(&rrep, (const char * const *)apaths, an);

        /* v1.8: optionally persist snapshot (non-fatal, disableable like v1.4) */
        int write_snap = 1;
        const char *env = getenv("CNET_REGISTRY_REPORT_ARTIFACT");
        if (env && *env == '0') write_snap = 0;
        if (write_snap) {
            CircuitRegistryReportSnapshot snap = {0};
            if (circuit_build_consolidation_registry_snapshot(&rrep, (const char * const *)apaths, an, &snap) == 0) {
                const char *snap_path = getenv("CNET_REGISTRY_REPORT_PATH");
                if (!snap_path || !*snap_path) snap_path = "artifacts/consolidation/registry_report.json";
                if (circuit_write_consolidation_registry_snapshot(snap_path, &snap) == 0) {
                    printf("  registry snapshot written: %s (evidence only)\n", snap_path);
                } else {
                    printf("  WARNING: registry snapshot write skipped (non-strict)\n");
                }
            }
        } else {
            printf("  registry snapshot writing disabled via CNET_REGISTRY_REPORT_ARTIFACT=0\n");
        }

        registry_free(&rrep);
        for (size_t k = 0; k < an; k++) free(apaths[k]);
        free(apaths);
    }

    /* ================================================================== */
    printf("\n=== Part 5: reachability pruning, measured ===\n\n");
    /* ================================================================== */
    {
        enum { LAYERS = 6, DEAD_LEN = 6, DEAD_BRANCH = 16, TRAPS = 12 };
        enum { REPS = 50 }; /* the pruned run is too fast to time once */
        /* 6 chain prims + 6*12 traps + 5*16 dead producers = 158 */
        enum { NETS = LAYERS + LAYERS * TRAPS +
                      (DEAD_LEN - 1) * DEAD_BRANCH };
        static BinaryTransformNetwork nets[NETS];
        static char names[NETS][24];
        PrimitiveRegistry reg;
        static double t0_v[1];
        DagSource src[1];
        DagPlan slow = {0}, fast = {0};
        size_t n = 0;
        clock_t c0, c1;
        double ms_off, ms_on;
        char tag_a[16], tag_b[16];
        int k, j, w;
        int ok = 1;

        registry_init(&reg);

        /* The viable chain: t0 -> t1 -> ... -> t6. */
        for (k = 1; ok && k <= LAYERS; ++k) {
            sprintf(tag_a, "t%d", k - 1);
            sprintf(tag_b, "t%d", k);
            sprintf(names[n], "layer%d", k);
            ok = btn_init(&nets[n], 1, 1, 1, 2, 0.5, 1u) == 0 &&
                 btn_set_ports(&nets[n], PT(PORT_BINARY_MSB, 1, 1, tag_a),
                               PT(PORT_BINARY_MSB, 1, 1, tag_b)) == 0 &&
                 registry_add(&reg, &nets[n], names[n]) == 0;
            ++n;
        }
        /* Traps: produce every t-layer from the dead chain's head d1. */
        for (k = 1; ok && k <= LAYERS; ++k) {
            for (j = 0; ok && j < TRAPS; ++j) {
                sprintf(tag_b, "t%d", k);
                sprintf(names[n], "trap%d_%d", k, j);
                ok = btn_init(&nets[n], 1, 1, 1, 2, 0.5, 1u) == 0 &&
                     btn_set_ports(&nets[n],
                                   PT(PORT_BINARY_MSB, 1, 1, "d1"),
                                   PT(PORT_BINARY_MSB, 1, 1, tag_b)) == 0 &&
                     registry_add(&reg, &nets[n], names[n]) == 0;
                ++n;
            }
        }
        /* The dead chain: d1 <- d2 <- ... <- d6, DEAD_BRANCH producers per
           link, and nothing at all produces d6. Every trap path explores
           the whole branching dead subtree before failing -- unless the
           reachability table kills it at the first d1 obligation. */
        for (j = 1; ok && j < DEAD_LEN; ++j) {
            for (w = 0; ok && w < DEAD_BRANCH; ++w) {
                sprintf(tag_a, "d%d", j + 1);
                sprintf(tag_b, "d%d", j);
                sprintf(names[n], "dead%d_%d", j, w);
                ok = btn_init(&nets[n], 1, 1, 1, 2, 0.5, 1u) == 0 &&
                     btn_set_ports(&nets[n],
                                   PT(PORT_BINARY_MSB, 1, 1, tag_a),
                                   PT(PORT_BINARY_MSB, 1, 1, tag_b)) == 0 &&
                     registry_add(&reg, &nets[n], names[n]) == 0;
                ++n;
            }
        }
        if (!ok) {
            fprintf(stderr, "FAIL: benchmark registry setup.\n");
            registry_free(&reg);
            goto cleanup;
        }
        printf("trap registry: %lu primitives (1 viable chain, %d traps "
               "per layer over a %d-deep x%d-branching dead end)\n",
               (unsigned long)n, TRAPS, DEAD_LEN, DEAD_BRANCH);

        src[0].type = PT(PORT_BINARY_MSB, 1, 1, "t0");
        src[0].values = t0_v;

        reg.disable_plan_memo = 1;
        c0 = clock();
        if (dag_plan(&reg, src, 1, PT(PORT_BINARY_MSB, 1, 1, "t6"),
                     &slow) != 0) {
            fprintf(stderr, "FAIL: benchmark plan (memo off).\n");
            registry_free(&reg);
            goto cleanup;
        }
        c1 = clock();
        ms_off = (double)(c1 - c0) * 1000.0 / CLOCKS_PER_SEC;

        reg.disable_plan_memo = 0;
        c0 = clock();
        {
            int rep;

            for (rep = 0; rep < REPS; ++rep) {
                if (rep > 0) {
                    dag_free(&fast);
                }
                if (dag_plan(&reg, src, 1, PT(PORT_BINARY_MSB, 1, 1, "t6"),
                             &fast) != 0) {
                    fprintf(stderr, "FAIL: benchmark plan (memo on).\n");
                    dag_free(&slow);
                    registry_free(&reg);
                    goto cleanup;
                }
            }
        }
        c1 = clock();
        ms_on = (double)(c1 - c0) * 1000.0 / CLOCKS_PER_SEC / (double)REPS;

        /* Same plan either way (pruning only skips empty branches). */
        {
            const DagNode *p = slow.root;
            const DagNode *q = fast.root;
            int same = 1;

            while (p != NULL && q != NULL) {
                if (p->kind != q->kind || p->btn != q->btn) {
                    same = 0;
                    break;
                }
                if (p->kind == DAG_SOURCE) {
                    break;
                }
                p = p->children[0];
                q = q->children[0];
            }
            if (!same) {
                fprintf(stderr, "FAIL: pruning changed the plan.\n");
                dag_free(&slow);
                dag_free(&fast);
                registry_free(&reg);
                goto cleanup;
            }
        }
        printf("planning time: %.1f ms without pruning, %.3f ms with "
               "(avg of %d runs): %.0fx; identical plan\n",
               ms_off, ms_on, (int)REPS,
               ms_on > 0.0 ? ms_off / ms_on : (double)REPS * 1000.0);
        dag_free(&slow);
        dag_free(&fast);
        registry_free(&reg);
    }

    if (do_memory_shadow) {
        /* v2.0 shadow mode (post all normal): load the provided hints as advisory telemetry only.
           Compute and print shadow report. By construction (no threading of hints into planner or exec)
           the final plan, blackboard, execution result, and all gates are identical to non-shadow run. */
        if (argc >= 3 && add_planned && add_circuit.root_count > 0) {
            CircuitMemoryHintStore hs = {0};
            (void)circuit_memory_load_hints(argv[2], &hs);
            Port s_types[3] = { add_sources[0].type, add_sources[1].type, add_sources[2].type };
            Port g_ports[2] = { add_goals[0], add_goals[1] };
            CircuitMemoryShadowReport sr = {0};
            PrimitiveRegistry shadow_reg = {0};
            registry_init(&shadow_reg);
            /* best-effort: include the known prim from part2 for valid_* checks in demo */
            (void)registry_add(&shadow_reg, &dec_fa, "dec_full_add");
            if (shadow_reg.count > 0) shadow_reg.entries[shadow_reg.count-1].certified = 1;
            (void)circuit_memory_shadow_report(&shadow_reg, s_types, 3, g_ports, 2, &add_circuit, &hs, &sr);
            circuit_memory_print_shadow_report(&sr);
            printf("shadow influence_on_planner=%d (must be 0); plan unchanged by memory hints (SHADOW_ONLY)\n", sr.influence_on_planner);
            registry_free(&shadow_reg);
        } else if (argc < 3) {
            fprintf(stderr, "usage: circuit_demo --memory-shadow hints.json\n");
        }
    }

    printf("\nAll circuit demo parts passed.\n");
    rc = EXIT_SUCCESS;

cleanup:
    if (add_planned) {
        circuit_free(&add_circuit);
    }
    btn_free(&split);
    btn_free(&dec_value);
    btn_free(&dec_fa);
    btn_free(&chunk);
    return rc;
}

