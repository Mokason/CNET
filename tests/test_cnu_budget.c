/* test_cnu_budget — a tiny CNU must not be able to demand a huge allocation.
 *
 * WHY THIS EXISTS. `unit_load_mem` bounded each dimension individually against
 * UNIT_MAX_DIM (2^20) but never bounded their PRODUCTS, and called `btn_init`
 * before anything proved the sealed payload carried the arrays those products
 * imply. A correctly resealed ~200-byte CNU declaring
 * `ic = oc = max_hidden = 2^20` therefore asked `btn_init` for input x hidden
 * plus hidden x output doubles — terabytes — and initialised them before the
 * bounded reads could discover the payload was empty. Capsule payload size caps
 * do not cap expansion implied by header dimensions.
 *
 * The property proved here: an amplified header is refused FAST, under a hard
 * address-space limit small enough that succeeding to allocate is impossible.
 * The child process is the assertion — if the parser tries to allocate its way
 * out, it dies instead of returning.
 *
 * Fixtures are built in memory from a real saved unit and re-sealed with the
 * same FNV-1a the format uses, so every mutant differs from a loading unit in
 * exactly the declared dimensions. Nothing is written to disk.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../include/contract/contract.h"
#include "../include/contract/unit.h"
#include "../include/nn.h"

#define SYM 4
/* Big enough that a moderately amplified header CAN allocate successfully --
   otherwise the address limit, not the parser, would be doing the refusing and
   the test would pass against the unfixed code. Small enough that a genuinely
   absurd header still cannot. */
#define ADDRESS_LIMIT_BYTES ((rlim_t)2048u * 1024u * 1024u)
#define TIME_LIMIT_SECONDS  20

static int failures;
static int checks;

static void check(int condition, const char *message) {
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

/* The format's own seal, so a mutated image is a VALID image. */
static unsigned long long cnu_fnv(const unsigned char *p, size_t n) {
    unsigned long long h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static Port make_port(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = SYM;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static void one_hot(double *row, int hot) {
    int i;
    for (i = 0; i < SYM; i++) row[i] = (i == hot) ? 1.0 : 0.0;
}

/* A real, loadable CNU image. */
static unsigned char *make_unit(size_t *len_out) {
    BinaryTransformNetwork btn;
    Contract c;
    double in[SYM][SYM], target[SYM][SYM];
    unsigned char *buf = NULL;
    size_t len = 0;
    int i;

    for (i = 0; i < SYM; i++) {
        one_hot(in[i], i);
        one_hot(target[i], (i + 1) % SYM);
    }
    memset(&btn, 0, sizeof btn);
    memset(&c, 0, sizeof c);
    if (btn_init(&btn, SYM, SYM, 8, 16, 0.5, 20260730u) != 0) return NULL;
    btn_set_ports(&btn, make_port("cnubudget_in"), make_port("cnubudget_out"));
    btn_train(&btn, (const double *)in, (const double *)target, SYM, 500);
    if (contract_init_borrowed(&c, "cnubudget_unit", &btn, (const double *)in,
                               (const double *)target, SYM) != 0) {
        btn_free(&btn);
        return NULL;
    }
    if (unit_save_mem(&btn, &c, &buf, &len) != 0) buf = NULL;
    contract_free(&c);
    btn_free(&btn);
    if (buf) *len_out = len;
    return buf;
}

/* The header layout, from unit_save_mem: magic[4] version[4] name_len[2]
   name[name_len] then input_count/output_count/hidden_count/max_hidden as
   little-endian u64. Locate the first u64 and rewrite the four dimensions. */
static unsigned long long get_u64(const unsigned char *at) {
    unsigned long long v = 0;
    size_t i;
    for (i = 0; i < 8; i++) v |= (unsigned long long)at[i] << (8 * i);
    return v;
}

static void put_u64(unsigned char *at, unsigned long long v) {
    size_t i;
    for (i = 0; i < 8; i++) at[i] = (unsigned char)(v >> (8 * i));
}

static int amplify(unsigned char *buf, size_t len, unsigned long long ic,
                   unsigned long long oc, unsigned long long hc,
                   unsigned long long mhc) {
    size_t off = 4 + 4, ports;
    unsigned name_len;
    unsigned long long seal;
    if (len < off + 2) return -1;
    name_len = (unsigned)buf[off] | ((unsigned)buf[off + 1] << 8);
    off += 2 + name_len;
    /* ic oc hc mhc, then learning_rate (f64), ternary flag (u8),
       ternary_threshold (f64), then the port tables. */
    if (len < off + 32 + 8 + 1 + 8 + 8) return -1;
    put_u64(buf + off, ic);
    put_u64(buf + off + 8, oc);
    put_u64(buf + off + 16, hc);
    put_u64(buf + off + 24, mhc);
    ports = off + 32 + 8 + 1 + 8;

    /* The parser cross-checks ic/oc against the port totals, so a fixture that
       only rewrote the dimensions would be refused for the wrong reason. Widen
       the single input and output port to match. Layout per port:
       family(u8) width(u64) count(u64) taglen(u8) tag[taglen]. */
    {
        size_t at = ports;
        unsigned n_in, n_out, tl;
        if (at >= len) return -1;
        n_in = buf[at++];
        if (n_in != 1) return -1;
        if (at + 1 + 8 + 8 + 1 > len) return -1;
        at += 1;                        /* family */
        put_u64(buf + at, ic);          /* field_width */
        put_u64(buf + at + 8, 1);       /* field_count */
        at += 16;
        tl = buf[at++];
        at += tl;
        if (at >= len) return -1;
        n_out = buf[at++];
        if (n_out != 1) return -1;
        if (at + 1 + 8 + 8 + 1 > len) return -1;
        at += 1;
        put_u64(buf + at, oc);
        put_u64(buf + at + 8, 1);
    }

    seal = cnu_fnv(buf, len - 8);
    put_u64(buf + len - 8, seal);
    return 0;
}

/* Walk the header exactly as unit_load_mem does and return the offset of the
   exemplar_count u64. Returns (size_t)-1 if the layout does not match, which
   would mean the fixture is not the file this test thinks it is. */
static size_t exemplar_count_offset(const unsigned char *buf, size_t len,
                                    unsigned long long *count_out) {
    size_t off = 4 + 4;
    unsigned name_len, n_ports, i, tl;
    unsigned long long ic, oc, hc, mhc, cells;

    if (len < off + 2) return (size_t)-1;
    name_len = (unsigned)buf[off] | ((unsigned)buf[off + 1] << 8);
    off += 2 + name_len;
    if (len < off + 32 + 8 + 1 + 8) return (size_t)-1;
    ic = get_u64(buf + off);
    oc = get_u64(buf + off + 8);
    hc = get_u64(buf + off + 16);
    mhc = get_u64(buf + off + 24);
    (void)mhc;
    off += 32 + 8 + 1 + 8;          /* dims, learning rate, ternary, threshold */

    for (n_ports = 0; n_ports < 2; n_ports++) {
        unsigned count;
        if (off >= len) return (size_t)-1;
        count = buf[off++];
        for (i = 0; i < count; i++) {
            if (off + 1 + 8 + 8 + 1 > len) return (size_t)-1;
            off += 1 + 8 + 8;       /* family, field_width, field_count */
            tl = buf[off++];
            off += tl;
        }
    }
    /* output_bias, hidden_bias, input x hidden, hidden x output */
    cells = oc + hc + ic * hc + hc * oc;
    if (cells > (unsigned long long)(len / sizeof(double))) return (size_t)-1;
    off += (size_t)cells * sizeof(double);
    if (off + 8 > len) return (size_t)-1;
    if (count_out) *count_out = get_u64(buf + off);
    return off;
}

/* Rewrite exemplar_count and reseal. The packed exemplar body is left exactly
   as it was: that is the point -- the header claims far more rows than the
   payload can possibly carry. */
static int amplify_exemplars(unsigned char *buf, size_t len,
                             unsigned long long n_ex) {
    unsigned long long seal, was = 0;
    size_t at = exemplar_count_offset(buf, len, &was);
    if (at == (size_t)-1) return -1;
    if (was != SYM) return -1;      /* the fixture is not what we assumed */
    put_u64(buf + at, n_ex);
    seal = cnu_fnv(buf, len - 8);
    put_u64(buf + len - 8, seal);
    return 0;
}

/* Load in a child under a hard RLIMIT_AS and RLIMIT_CPU, and report what it
   COST. The verdict alone is not enough: a parser that allocates gigabytes and
   only then discovers the payload is empty still "refuses", and that is exactly
   the denial of service. `peak_kb` and `cpu_seconds` are what distinguish
   "refused before allocating" from "refused because the allocation failed".
   Returns:
     0  the parser refused
     1  the parser ACCEPTED the image
    -1  the child died (OOM kill, CPU limit, signal) */
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

static int load_in_child(const unsigned char *image, size_t len,
                         long *peak_kb, double *cpu_seconds, long *vm_peak_kb) {
    pid_t pid;
    int status = 0, fds[2];
    struct rusage usage;
    if (peak_kb) *peak_kb = 0;
    if (cpu_seconds) *cpu_seconds = 0.0;
    if (vm_peak_kb) *vm_peak_kb = -1;
    if (pipe(fds) != 0) return -1;
    pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return -1; }
    if (pid == 0) {
        close(fds[0]);
        struct rlimit as, cpu;
        BinaryTransformNetwork btn;
        Contract c;
        int rc;
        /* ASan reserves an enormous shadow mapping, so RLIMIT_AS is not
           meaningful under it; the RSS and CPU assertions still are. */
        if (!getenv("CNU_BUDGET_NO_RLIMIT")) {
            as.rlim_cur = as.rlim_max = ADDRESS_LIMIT_BYTES;
            (void)setrlimit(RLIMIT_AS, &as);
        }
        cpu.rlim_cur = cpu.rlim_max = TIME_LIMIT_SECONDS;
        (void)setrlimit(RLIMIT_CPU, &cpu);
        memset(&btn, 0, sizeof btn);
        memset(&c, 0, sizeof c);
        rc = unit_load_mem(&btn, &c, image, len);
        if (rc == 0) {
            btn_free(&btn);
            contract_free(&c);
        }
        /* Report peak ADDRESS SPACE, not just resident pages: malloc of half a
           gigabyte that is never written keeps RSS flat while still being the
           amplification. Read before _exit so the mapping is still counted. */
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
    if (peak_kb) *peak_kb = usage.ru_maxrss;
    if (cpu_seconds)
        *cpu_seconds = (double)usage.ru_utime.tv_sec +
                       (double)usage.ru_utime.tv_usec / 1e6 +
                       (double)usage.ru_stime.tv_sec +
                       (double)usage.ru_stime.tv_usec / 1e6;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status) == 1 ? 1 : 0;
}

/* Headroom over the honest control before a refusal counts as "cheap". */
#define PEAK_SLACK_KB   (64L * 1024L)
#define VM_SLACK_KB     (64L * 1024L)
#define CPU_BUDGET_SECS 2.0

static long baseline_peak_kb;
static long baseline_vm_peak_kb;

static void amplified_case(const unsigned char *good, size_t len,
                           unsigned long long ic, unsigned long long oc,
                           unsigned long long hc, unsigned long long mhc,
                           const char *description) {
    unsigned char *image = (unsigned char *)malloc(len);
    char message[320];
    long peak = 0, vm = -1;
    double cpu = 0.0;
    int verdict;

    if (!image) {
        check(0, "fixture allocation");
        return;
    }
    memcpy(image, good, len);
    if (amplify(image, len, ic, oc, hc, mhc) != 0) {
        free(image);
        check(0, "fixture header could not be rewritten");
        return;
    }
    verdict = load_in_child(image, len, &peak, &cpu, &vm);
    free(image);

    printf("CNU_BUDGET_CASE verdict=%d peak_kb=%ld vm_peak_kb=%ld cpu=%.3f %s\n",
           verdict, peak, vm, cpu, description);
    snprintf(message, sizeof message, "%s is refused (verdict=%d)", description,
             verdict);
    check(verdict == 0, message);
    snprintf(message, sizeof message,
             "%s is refused BEFORE allocating (peak %ld kB vs control %ld kB)",
             description, peak, baseline_peak_kb);
    check(peak <= baseline_peak_kb + PEAK_SLACK_KB, message);
    snprintf(message, sizeof message,
             "%s reserves no extra address space (VmPeak %ld kB vs control %ld kB)",
             description, vm, baseline_vm_peak_kb);
    check(vm >= 0 && baseline_vm_peak_kb >= 0 &&
              vm <= baseline_vm_peak_kb + VM_SLACK_KB,
          message);
    snprintf(message, sizeof message,
             "%s is refused within the CPU budget (%.3fs)", description, cpu);
    check(cpu < CPU_BUDGET_SECS, message);
}

static void amplified_exemplar_case(const unsigned char *good, size_t len,
                                    unsigned long long n_ex,
                                    const char *description) {
    unsigned char *image = (unsigned char *)malloc(len);
    char message[320];
    long peak = 0, vm = -1;
    double cpu = 0.0;
    int verdict;

    if (!image) {
        check(0, "fixture allocation");
        return;
    }
    memcpy(image, good, len);
    if (amplify_exemplars(image, len, n_ex) != 0) {
        free(image);
        check(0, "exemplar fixture header could not be rewritten");
        return;
    }
    verdict = load_in_child(image, len, &peak, &cpu, &vm);
    free(image);

    printf("CNU_BUDGET_CASE verdict=%d peak_kb=%ld vm_peak_kb=%ld cpu=%.3f %s\n",
           verdict, peak, vm, cpu, description);
    snprintf(message, sizeof message, "%s is refused (verdict=%d)", description,
             verdict);
    check(verdict == 0, message);
    snprintf(message, sizeof message,
             "%s is refused BEFORE allocating (peak %ld kB vs control %ld kB)",
             description, peak, baseline_peak_kb);
    check(peak <= baseline_peak_kb + PEAK_SLACK_KB, message);
    snprintf(message, sizeof message,
             "%s reserves no extra address space (VmPeak %ld kB vs control %ld kB)",
             description, vm, baseline_vm_peak_kb);
    check(vm >= 0 && baseline_vm_peak_kb >= 0 &&
              vm <= baseline_vm_peak_kb + VM_SLACK_KB,
          message);
    snprintf(message, sizeof message,
             "%s is refused within the CPU budget (%.3fs)", description, cpu);
    check(cpu < CPU_BUDGET_SECS, message);
}

int main(void) {
    unsigned char *good;
    size_t len = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    good = make_unit(&len);
    if (!good) {
        fprintf(stderr, "FAIL: cannot build a control unit\n");
        return 1;
    }
    printf("CNU_BUDGET_CONTROL bytes=%zu\n", len);

    /* Control: the honest unit still loads, in the child, under the same
       limits. A budget that rejects everything proves nothing, and its cost is
       the baseline every refusal below is measured against. */
    {
        double cpu = 0.0;
        check(load_in_child(good, len, &baseline_peak_kb, &cpu,
                            &baseline_vm_peak_kb) == 1,
              "the honest unit still loads under the address limit");
        printf("CNU_BUDGET_BASELINE peak_kb=%ld vm_peak_kb=%ld cpu=%.3f\n",
               baseline_peak_kb, baseline_vm_peak_kb, cpu);
        check(baseline_vm_peak_kb > 0,
              "the child reports its peak address space");
    }

    /* The finding's fixture: every dimension at the old individual maximum. */
    amplified_case(good, len, 1u << 20, 1u << 20, 1u << 20, 1u << 20,
                   "a tiny CNU declaring 2^20 in every dimension");
    /* Amplification via max_hidden_count alone: the read budget cannot see it,
       because only `hidden_count` doubles are ever read. */
    amplified_case(good, len, SYM, SYM, 8, 1u << 20,
                   "a tiny CNU whose max_hidden_count alone is amplified");
    /* Amplification via the input-hidden product. */
    amplified_case(good, len, 1u << 20, SYM, 1u << 16, 1u << 16,
                   "a tiny CNU with an amplified input x hidden matrix");
    /* Amplification via the hidden-output product. */
    amplified_case(good, len, SYM, 1u << 20, 1u << 16, 1u << 16,
                   "a tiny CNU with an amplified hidden x output matrix");
    /* Products chosen to wrap size_t if multiplied unchecked. */
    amplified_case(good, len, 1u << 20, 1u << 20, 1u << 19, 1u << 20,
                   "a tiny CNU whose products would wrap unchecked arithmetic");
    /* Just over the absolute cell ceiling. */
    amplified_case(good, len, 1u << 14, 1u << 14, 1u << 12, 1u << 13,
                   "a tiny CNU just past the absolute cell ceiling");
    /* The nastiest shape, and the one that actually costs. calloc alone is
       cheap (untouched zero pages), but btn_init then runs
       btn_add_hidden_neuron `hidden_count` times, and each pass WRITES
       `input_count` doubles. hidden_count was never bounded by what the payload
       could possibly contain, so a 764-byte file could make the parser touch a
       gigabyte and burn seconds of CPU before the bounded reads discovered
       there were no weights to read. */
    amplified_case(good, len, 512, SYM, 1u << 18, 1u << 18,
                   "a tiny CNU that makes the parser touch a gigabyte");

    /* --- exemplar amplification -----------------------------------------
       Exemplars are packed one BIT per value, so a declared row count implies a
       payload length exactly. exemplar_count was bounded only by
       UNIT_MAX_EXEMPLARS (2^24) and its product with the port totals was
       checked only for overflow, not against the bytes that remain, so a tiny
       file could ask for two 500 MB arrays before a single packed byte was
       read. */
    amplified_exemplar_case(good, len, 1u << 24,
                            "a tiny CNU declaring 2^24 exemplars");
    amplified_exemplar_case(good, len, (1u << 24) - 1,
                            "a tiny CNU one under the exemplar ceiling");
    amplified_exemplar_case(good, len, 1u << 20,
                            "a tiny CNU declaring 2^20 exemplars");
    amplified_exemplar_case(good, len, 4096,
                            "a tiny CNU declaring 4096 exemplars it does not carry");
    amplified_exemplar_case(good, len, SYM + 1,
                            "a tiny CNU declaring one exemplar too many");
    amplified_exemplar_case(good, len, (1ull << 24) + 1,
                            "a tiny CNU past the exemplar ceiling");

    free(good);
    if (failures) {
        printf("CNU_BUDGET_FAIL checks=%d failures=%d\n", checks, failures);
        return 1;
    }
    printf("CNU_BUDGET_PASS checks=%d amplified=7 exemplar_amplified=6 "
           "address_limit=%luMB\n", checks,
           (unsigned long)(ADDRESS_LIMIT_BYTES / (1024u * 1024u)));
    return 0;
}
