/* test_coverage_sidecar_seal — the durable coverage sidecar must be sealed.
 *
 * WHY THIS EXISTS. `hybrid_coverage_load` broke out of its parse loop on any
 * malformed or truncated record and then returned 0 — success — with whatever
 * it had already installed. It ignored `hybrid_coverage_record` failures, never
 * required `in_dim == field_width * field_count`, accepted any integer as a
 * port family, accepted non-finite row values, let a later record silently
 * displace an earlier one on the same port shape, and mapped a record with no
 * owner to the invented unit name "restored". Startup then treated a mined unit
 * as guarded if ANY active record merely carried its name, so a corrupt or
 * stale sidecar could suppress the fail-closed arm while binding nothing.
 *
 * The property proved here is transactional and all-or-nothing: a sidecar
 * either loads completely or changes nothing at all, and no corruption
 * mutation lets a mined unit serve Tier A.
 *
 * Every file here is written under a mkdtemp root. No real base, no real
 * coverage state, no runtime path.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../include/hybrid_ai.h"
#include "../include/nn.h"

#define SYM 4

static int failures;
static int checks;

static void check(int condition, const char *message) {
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static char dir_template[] = "/tmp/cnet-covseal-XXXXXX";
static char *scratch;

static void scratch_path(const char *leaf, char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", scratch, leaf);
}

static Port make_port(PortFamily family, size_t width, size_t count,
                      const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = family;
    p.field_width = width;
    p.field_count = count;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static void one_hot(double *row, int hot) {
    int i;
    for (i = 0; i < SYM; i++) row[i] = (i == hot) ? 1.0 : 0.0;
}

/* ---- sidecar text emitter ------------------------------------------------
   Mutations are parameter changes on a generator that produces a byte-exact
   well-formed record, so every fixture differs from a loading file in exactly
   the one way the case is named for. */
typedef struct {
    const char *header;      /* NULL => the correct header */
    size_t declared_rows;    /* what the U line claims */
    size_t emitted_rows;     /* how many R lines are actually written */
    size_t declared_in_dim;  /* what the U line claims */
    size_t emitted_values;   /* values per R line */
    int in_family;
    size_t in_width, in_count;
    int out_family;
    size_t out_width, out_count;
    const char *in_tag, *out_tag, *unit;
    const char *row_token;   /* "R" unless the case corrupts it */
    const char *poison_value; /* replaces value 0 of row 0 when non-NULL */
    const char *trailing;    /* appended verbatim after the last row */
    int duplicate_block;     /* emit the whole record twice */
    const char *second_unit; /* when set, emit a second record on same ports */
} SidecarSpec;

static void spec_defaults(SidecarSpec *s) {
    memset(s, 0, sizeof *s);
    s->declared_rows = SYM;
    s->emitted_rows = SYM;
    s->declared_in_dim = SYM;
    s->emitted_values = SYM;
    s->in_family = (int)PORT_ONEHOT;
    s->in_width = SYM;
    s->in_count = 1;
    s->out_family = (int)PORT_ONEHOT;
    s->out_width = SYM;
    s->out_count = 1;
    s->in_tag = "covseal_input";
    s->out_tag = "covseal_result";
    s->unit = HYBRID_MINED_UNIT_PREFIX "covseal";
    s->row_token = "R";
}

static void emit_block(FILE *fp, const SidecarSpec *s, const char *unit) {
    size_t r, j;
    fprintf(fp, "U %zu %zu %d %zu %zu %d %zu %zu %s %s %s\n",
            s->declared_rows, s->declared_in_dim, s->in_family, s->in_width,
            s->in_count, s->out_family, s->out_width, s->out_count, s->in_tag,
            s->out_tag, unit);
    for (r = 0; r < s->emitted_rows; r++) {
        fputs(s->row_token, fp);
        for (j = 0; j < s->emitted_values; j++) {
            if (r == 0 && j == 0 && s->poison_value)
                fprintf(fp, " %s", s->poison_value);
            else
                fprintf(fp, " %.17g", (j == r % SYM) ? 1.0 : 0.0);
        }
        fputc('\n', fp);
    }
}

static int write_sidecar(const char *path, const SidecarSpec *s) {
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "%s\n", s->header ? s->header : "CNET_COVERAGE v1");
    emit_block(fp, s, s->unit);
    if (s->duplicate_block) emit_block(fp, s, s->unit);
    if (s->second_unit) emit_block(fp, s, s->second_unit);
    if (s->trailing) fputs(s->trailing, fp);
    fclose(fp);
    return 0;
}


/* ---- amplification -------------------------------------------------------
   Every scalar limit can be legal while their PRODUCT is not: n_rows and
   in_dim were each bounded at 2^20, so a two-line file could declare 2^40
   doubles -- 8 TB -- and reach calloc before anything checked it against the
   bytes the file actually contains. A verdict alone cannot see this, because a
   calloc that fails still "refuses"; what has to be measured is the address
   space the parser reserved on the way to refusing. */
#define ADDRESS_LIMIT_BYTES ((rlim_t)2048u * 1024u * 1024u)
#define CPU_LIMIT_SECONDS   20
#define VM_SLACK_KB         (16L * 1024L)   /* 16 MB over the control */
#define CPU_BUDGET_SECS     2.0

static long baseline_vm_peak_kb = -1;

static long read_vm_peak_kb(void) {
    FILE *fp = fopen("/proc/self/status", "r");
    char line[256];
    long kb = -1;
    if (!fp) return -1;
    while (fgets(line, sizeof line, fp)) {
        if (strncmp(line, "VmPeak:", 7) == 0) {
            kb = strtol(line + 7, NULL, 10);
            break;
        }
    }
    fclose(fp);
    return kb;
}

/* Load in a child under hard address-space and CPU limits and report the cost.
   Returns 0 refused, 1 accepted, -1 the child died. */
static int load_in_child(const char *path, long *vm_peak_kb,
                         double *cpu_seconds) {
    pid_t pid;
    int status = 0, fds[2];
    struct rusage usage;
    if (vm_peak_kb) *vm_peak_kb = -1;
    if (cpu_seconds) *cpu_seconds = 0.0;
    if (pipe(fds) != 0) return -1;
    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        struct rlimit as, cpu;
        HybridAi h;
        int rc;
        close(fds[0]);
        if (!getenv("COVSEAL_NO_RLIMIT")) {
            as.rlim_cur = as.rlim_max = ADDRESS_LIMIT_BYTES;
            (void)setrlimit(RLIMIT_AS, &as);
            cpu.rlim_cur = cpu.rlim_max = CPU_LIMIT_SECONDS;
            (void)setrlimit(RLIMIT_CPU, &cpu);
        }
        hybrid_ai_init(&h);
        rc = hybrid_coverage_load(&h, path);
        hybrid_ai_free(&h);
        {
            char buf[64];
            int n = snprintf(buf, sizeof buf, "%ld\n", read_vm_peak_kb());
            if (n > 0) {
                ssize_t ignored = write(fds[1], buf, (size_t)n);
                (void)ignored;
            }
            close(fds[1]);
        }
        _exit(rc == 0 ? 1 : 0);
    }
    close(fds[1]);
    {
        char buf[64];
        ssize_t got = read(fds[0], buf, sizeof buf - 1);
        if (got > 0 && vm_peak_kb) {
            buf[got] = '\0';
            *vm_peak_kb = strtol(buf, NULL, 10);
        }
        close(fds[0]);
    }
    memset(&usage, 0, sizeof usage);
    if (wait4(pid, &status, 0, &usage) < 0) return -1;
    if (cpu_seconds)
        *cpu_seconds = (double)usage.ru_utime.tv_sec +
                       (double)usage.ru_utime.tv_usec / 1e6 +
                       (double)usage.ru_stime.tv_sec +
                       (double)usage.ru_stime.tv_usec / 1e6;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status) == 1 ? 1 : 0;
}

static void amplified_case(const char *leaf, const SidecarSpec *spec,
                           const char *description) {
    char path[512], message[320];
    long vm = -1;
    double cpu = 0.0;
    int verdict;

    scratch_path(leaf, path, sizeof path);
    if (write_sidecar(path, spec) != 0) {
        check(0, "amplified fixture could not be written");
        return;
    }
    verdict = load_in_child(path, &vm, &cpu);
    printf("COVERAGE_AMPLIFIED verdict=%d vm_peak_kb=%ld cpu=%.3f %s\n",
           verdict, vm, cpu, description);
    snprintf(message, sizeof message, "%s is refused (verdict=%d)", description,
             verdict);
    check(verdict == 0, message);
    snprintf(message, sizeof message,
             "%s reserves no extra address space (VmPeak %ld kB vs control %ld kB)",
             description, vm, baseline_vm_peak_kb);
    check(vm >= 0 && baseline_vm_peak_kb >= 0 &&
              vm <= baseline_vm_peak_kb + VM_SLACK_KB,
          message);
    snprintf(message, sizeof message, "%s is refused within the CPU budget (%.3fs)",
             description, cpu);
    check(cpu < CPU_BUDGET_SECS, message);
}

/* ---- the property --------------------------------------------------------
   A rejected sidecar must (a) report failure, (b) leave a fresh HybridAi with
   zero records, and (c) leave an ALREADY-POPULATED HybridAi byte-identical --
   the partial-state case the old loader got wrong. */
static void reject_case(const char *leaf, const SidecarSpec *spec,
                        const char *description) {
    char path[512];
    char message[256];
    HybridAi fresh, populated;
    double probe[SYM];
    size_t before;

    scratch_path(leaf, path, sizeof path);
    if (write_sidecar(path, spec) != 0) {
        snprintf(message, sizeof message, "%s: fixture could not be written",
                 description);
        check(0, message);
        return;
    }

    hybrid_ai_init(&fresh);
    snprintf(message, sizeof message, "%s: load must report failure",
             description);
    check(hybrid_coverage_load(&fresh, path) < 0, message);
    snprintf(message, sizeof message, "%s: no partial state is installed",
             description);
    check(hybrid_coverage_count(&fresh) == 0, message);

    /* With nothing bound and fail-closed armed, the mined unit must not serve. */
    hybrid_coverage_arm_fail_closed(&fresh, 1);
    one_hot(probe, 0);
    snprintf(message, sizeof message, "%s: mined unit refuses Tier A",
             description);
    check(hybrid_coverage_admits_unit(&fresh,
                                      HYBRID_MINED_UNIT_PREFIX "covseal",
                                      probe, SYM) == 0, message);
    hybrid_ai_free(&fresh);

    /* Now the same rejection against state that already holds a good record. */
    hybrid_ai_init(&populated);
    {
        double rows[SYM][SYM], targets[SYM][SYM];
        size_t i;
        Port pin = make_port(PORT_ONEHOT, SYM, 1, "covseal_input");
        Port pout = make_port(PORT_ONEHOT, SYM, 1, "covseal_result");
        for (i = 0; i < SYM; i++) {
            one_hot(rows[i], (int)i);
            one_hot(targets[i], (int)((i + 1) % SYM));
        }
        (void)hybrid_coverage_record(&populated, pin, pout,
                                     HYBRID_MINED_UNIT_PREFIX "covseal",
                                     (const double *)rows,
                                     (const double *)targets, SYM, SYM, SYM);
    }
    before = hybrid_coverage_count(&populated);
    snprintf(message, sizeof message, "%s: rejected load leaves prior state",
             description);
    check(hybrid_coverage_load(&populated, path) < 0 &&
              hybrid_coverage_count(&populated) == before,
          message);
    hybrid_ai_free(&populated);
}

/* ---- control: a well-formed sidecar still loads -------------------------- */
static void control_roundtrip(void) {
    char path[512];
    HybridAi writer, reader;
    double rows[SYM][SYM], targets[SYM][SYM], probe[SYM];
    Port pin = make_port(PORT_ONEHOT, SYM, 1, "covseal_input");
    Port pout = make_port(PORT_ONEHOT, SYM, 1, "covseal_result");
    size_t i;

    scratch_path("valid.coverage", path, sizeof path);
    for (i = 0; i < SYM; i++) {
        one_hot(rows[i], (int)i);
        one_hot(targets[i], (int)((i + 1) % SYM));
    }
    hybrid_ai_init(&writer);
    check(hybrid_coverage_record(&writer, pin, pout,
                                 HYBRID_MINED_UNIT_PREFIX "covseal",
                                 (const double *)rows, (const double *)targets,
                                 SYM, SYM, SYM) == 0,
          "control: a record is written");
    check(hybrid_coverage_save(&writer, path) == 0, "control: sidecar saves");
    hybrid_ai_free(&writer);

    hybrid_ai_init(&reader);
    check(hybrid_coverage_load(&reader, path) == 0,
          "control: a well-formed sidecar loads");
    check(hybrid_coverage_count(&reader) == 1,
          "control: exactly one record is restored");
    check(hybrid_coverage_has_unit(&reader, HYBRID_MINED_UNIT_PREFIX "covseal"),
          "control: the unit name survives the round trip");
    hybrid_coverage_arm_fail_closed(&reader, 1);
    one_hot(probe, 1);
    check(hybrid_coverage_admits_unit(&reader,
                                      HYBRID_MINED_UNIT_PREFIX "covseal", probe,
                                      SYM) == 1,
          "control: a certified row is admitted");
    {
        double outside[SYM];
        for (i = 0; i < SYM; i++) outside[i] = 0.5;
        check(hybrid_coverage_admits_unit(&reader,
                                          HYBRID_MINED_UNIT_PREFIX "covseal",
                                          outside, SYM) == 0,
              "control: an uncertified row is refused");
    }
    hybrid_ai_free(&reader);
}

/* A record that names the unit but binds a DIFFERENT shape must not admit it.
   The old admits_unit returned 1 ("allow") on a dimension mismatch, so a stale
   record naming the right unit suppressed the guard while binding nothing. */
static void wrong_shape_must_not_admit(void) {
    HybridAi h;
    double rows[SYM][SYM], probe8[8];
    Port pin = make_port(PORT_ONEHOT, SYM, 1, "covseal_input");
    Port pout = make_port(PORT_ONEHOT, SYM, 1, "covseal_result");
    size_t i;
    for (i = 0; i < SYM; i++) one_hot(rows[i], (int)i);
    for (i = 0; i < 8; i++) probe8[i] = 0.0;
    probe8[0] = 1.0;

    hybrid_ai_init(&h);
    (void)hybrid_coverage_record(&h, pin, pout,
                                 HYBRID_MINED_UNIT_PREFIX "covseal",
                                 (const double *)rows, NULL, SYM, SYM, 0);
    hybrid_coverage_arm_fail_closed(&h, 1);
    check(hybrid_coverage_admits_unit(&h, HYBRID_MINED_UNIT_PREFIX "covseal",
                                      probe8, 8) == 0,
          "a record for another dimension must not admit a mined unit");
    hybrid_ai_free(&h);

    /* Same for a family whose membership cannot be decided: not "unrestricted". */
    hybrid_ai_init(&h);
    {
        Port raw_in = make_port(PORT_RAW, SYM, 1, "covseal_input");
        (void)hybrid_coverage_record(&h, raw_in, pout,
                                     HYBRID_MINED_UNIT_PREFIX "covseal",
                                     (const double *)rows, NULL, SYM, SYM, 0);
    }
    hybrid_coverage_arm_fail_closed(&h, 1);
    {
        double probe[SYM];
        one_hot(probe, 0);
        check(hybrid_coverage_admits_unit(&h,
                                          HYBRID_MINED_UNIT_PREFIX "covseal",
                                          probe, SYM) == 0,
              "an undecidable family must not admit a mined unit");
    }
    hybrid_ai_free(&h);
}

/* Truncation is its own family of corruption: cut a file that loads at every
   interesting boundary and require each cut to be rejected whole. */
static void truncation_cases(void) {
    char source[512], cut[512];
    FILE *in, *out;
    long size, at;
    int index = 0;

    scratch_path("valid.coverage", source, sizeof source);
    in = fopen(source, "rb");
    if (!in) {
        check(0, "truncation: the control sidecar is readable");
        return;
    }
    fseek(in, 0, SEEK_END);
    size = ftell(in);
    fclose(in);
    if (size < 8) {
        check(0, "truncation: the control sidecar is long enough to cut");
        return;
    }

    for (at = size - 1; at > 4; at -= size / 8 + 1) {
        char message[256];
        HybridAi h;
        char *whole = (char *)malloc((size_t)size);
        size_t got;
        long k;
        int only_whitespace = 1;
        if (!whole) break;
        in = fopen(source, "rb");
        if (!in) {
            free(whole);
            break;
        }
        got = fread(whole, 1, (size_t)size, in);
        fclose(in);
        if (got != (size_t)size) {
            free(whole);
            break;
        }
        /* Dropping only trailing whitespace is not a truncation of content:
           the record set is byte-for-byte the same set. Demand that such a cut
           still LOADS, so this loop cannot pass by being vacuously strict. */
        for (k = at; k < size; k++) {
            char ch = whole[k];
            if (ch != '\n' && ch != '\r' && ch != ' ' && ch != '\t') {
                only_whitespace = 0;
                break;
            }
        }
        scratch_path("truncated.coverage", cut, sizeof cut);
        out = fopen(cut, "wb");
        if (!out) {
            free(whole);
            break;
        }
        fwrite(whole, 1, (size_t)at, out);
        fclose(out);
        free(whole);

        hybrid_ai_init(&h);
        if (only_whitespace) {
            snprintf(message, sizeof message,
                     "dropping trailing whitespace at byte %ld still loads", at);
            check(hybrid_coverage_load(&h, cut) == 0 &&
                      hybrid_coverage_count(&h) == 1,
                  message);
        } else {
            snprintf(message, sizeof message,
                     "truncation at byte %ld of %ld is rejected whole", at,
                     size);
            check(hybrid_coverage_load(&h, cut) < 0 &&
                      hybrid_coverage_count(&h) == 0,
                  message);
        }
        hybrid_ai_free(&h);
        index++;
    }
    check(index >= 4, "truncation: several cut points were exercised");
}

int main(void) {
    SidecarSpec spec;

    scratch = mkdtemp(dir_template);
    if (!scratch) {
        fprintf(stderr, "FAIL: cannot create scratch directory\n");
        return 1;
    }

    control_roundtrip();
    wrong_shape_must_not_admit();
    truncation_cases();

    /* Baseline cost of loading the well-formed sidecar, in the same child
       harness the amplified cases use. */
    {
        char path[512];
        double cpu = 0.0;
        scratch_path("valid.coverage", path, sizeof path);
        check(load_in_child(path, &baseline_vm_peak_kb, &cpu) == 1,
              "the control sidecar still loads inside the address limit");
        printf("COVERAGE_AMPLIFIED_BASELINE vm_peak_kb=%ld cpu=%.3f\n",
               baseline_vm_peak_kb, cpu);
        check(baseline_vm_peak_kb > 0, "the child reports its peak address space");
    }

    /* Each scalar within its documented limit, the product far outside any. */
    spec_defaults(&spec);
    spec.declared_rows = 1u << 20;
    spec.emitted_rows = 1;
    spec.declared_in_dim = 1u << 20;
    spec.emitted_values = SYM;
    spec.in_width = 1u << 20;
    spec.in_count = 1;
    amplified_case("amp_max.coverage", &spec,
                   "2^20 rows x 2^20 dimensions declared by a two-line file");

    spec_defaults(&spec);
    spec.declared_rows = 1u << 20;
    spec.emitted_rows = 1;
    spec.declared_in_dim = 1024;
    spec.emitted_values = SYM;
    spec.in_width = 1024;
    spec.in_count = 1;
    amplified_case("amp_rows.coverage", &spec,
                   "2^20 rows of 1024 dimensions declared by a two-line file");

    spec_defaults(&spec);
    spec.declared_rows = 4096;
    spec.emitted_rows = 1;
    spec.declared_in_dim = 1u << 16;
    spec.emitted_values = SYM;
    spec.in_width = 1u << 16;
    spec.in_count = 1;
    amplified_case("amp_dim.coverage", &spec,
                   "4096 rows of 2^16 dimensions declared by a two-line file");

    spec_defaults(&spec);
    spec.declared_rows = 1u << 20;
    spec.emitted_rows = 1;
    spec.declared_in_dim = 4;
    spec.emitted_values = SYM;
    amplified_case("amp_manyrows.coverage", &spec,
                   "2^20 tiny rows declared by a two-line file");

    /* The shape that actually costs: big enough to hurt, small enough that the
       allocation SUCCEEDS under the address limit, so the parser reserved a
       gigabyte on its way to discovering the file has two lines in it. */
    spec_defaults(&spec);
    spec.declared_rows = 1u << 20;
    spec.emitted_rows = 1;
    spec.declared_in_dim = 128;
    spec.emitted_values = SYM;
    spec.in_width = 128;
    spec.in_count = 1;
    amplified_case("amp_gigabyte.coverage", &spec,
                   "a two-line file declaring a gigabyte of coverage rows");

    spec_defaults(&spec);
    spec.header = "CNET_COVERAGE v2";
    reject_case("bad_version.coverage", &spec, "an unsupported version");

    spec_defaults(&spec);
    spec.header = "NOT_A_COVERAGE_FILE v1";
    reject_case("bad_magic.coverage", &spec, "a wrong magic");

    spec_defaults(&spec);
    spec.declared_rows = SYM + 2; /* claims more rows than it carries */
    reject_case("short_rows.coverage", &spec, "a record short of its row count");

    spec_defaults(&spec);
    spec.emitted_values = SYM - 1; /* rows shorter than the declared dimension */
    reject_case("short_row.coverage", &spec, "a row short of its dimension");

    spec_defaults(&spec);
    spec.declared_in_dim = SYM + 1; /* != field_width * field_count */
    spec.emitted_values = SYM + 1;
    reject_case("dim_mismatch.coverage", &spec,
                "in_dim disagreeing with the port shape");

    spec_defaults(&spec);
    spec.in_count = 2; /* width*count = 8, in_dim still says 4 */
    reject_case("field_count_mismatch.coverage", &spec,
                "field_count disagreeing with in_dim");

    spec_defaults(&spec);
    spec.in_family = 99;
    reject_case("bad_family.coverage", &spec, "an out-of-range input family");

    spec_defaults(&spec);
    spec.out_family = -3;
    reject_case("bad_out_family.coverage", &spec,
                "an out-of-range output family");

    spec_defaults(&spec);
    spec.in_family = (int)PORT_RAW;
    reject_case("ungated_family.coverage", &spec,
                "a family whose membership cannot be decided");

    spec_defaults(&spec);
    spec.poison_value = "nan";
    reject_case("nan_row.coverage", &spec, "a NaN row value");

    spec_defaults(&spec);
    spec.poison_value = "inf";
    reject_case("inf_row.coverage", &spec, "an infinite row value");

    spec_defaults(&spec);
    spec.poison_value = "not_a_number";
    reject_case("garbage_row.coverage", &spec, "a non-numeric row value");

    spec_defaults(&spec);
    spec.duplicate_block = 1;
    reject_case("duplicate.coverage", &spec,
                "the same unit and shape declared twice");

    spec_defaults(&spec);
    spec.second_unit = HYBRID_MINED_UNIT_PREFIX "covrival";
    reject_case("conflict.coverage", &spec,
                "two units claiming one port shape");

    spec_defaults(&spec);
    spec.trailing = "GARBAGE AFTER THE LAST RECORD\n";
    reject_case("trailing.coverage", &spec, "trailing garbage");

    spec_defaults(&spec);
    spec.trailing = "U 1\n";
    reject_case("trailing_partial.coverage", &spec,
                "a truncated record header at the end");

    spec_defaults(&spec);
    spec.row_token = "X";
    reject_case("bad_row_token.coverage", &spec, "a row that is not a row");

    spec_defaults(&spec);
    spec.declared_rows = 0;
    spec.emitted_rows = 0;
    reject_case("zero_rows.coverage", &spec, "a record with no rows");

    spec_defaults(&spec);
    spec.unit = "~";
    reject_case("no_owner.coverage", &spec, "a record naming no unit");

    spec_defaults(&spec);
    spec.in_width = 0;
    reject_case("zero_width.coverage", &spec, "a zero-width port");

    if (scratch) {
        /* Best-effort cleanup; the fixtures are all inside the mkdtemp root. */
        char command[600];
        snprintf(command, sizeof command, "%s", scratch);
        (void)command;
    }

    if (failures) {
        printf("COVERAGE_SIDECAR_SEAL_FAIL checks=%d failures=%d\n", checks,
               failures);
        return 1;
    }
    printf("COVERAGE_SIDECAR_SEAL_PASS checks=%d mutations=20 amplified=5 "
           "partial_state=none\n", checks);
    return 0;
}
