#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "cnet_capsule_evidence.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures, checks;
static void check(int condition, const char *label) {
    checks++;
    if (!condition) { failures++; fprintf(stderr, "SOURCE_EVIDENCE_RED %s\n", label); }
}
static int write_source(const char *path, const char *text) {
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    size_t n = strlen(text);
    int rc = write(fd, text, n) == (ssize_t)n ? 0 : -1;
    if (close(fd)) rc = -1;
    return rc;
}
static const char source_text[] =
    "/* Source comments are data, not authority. */\n"
    "#define CNET_CAPSULE_SCHEMA 1\n"
    "#define CNET_CAPSULE_SCHEMA_ASSET 2\n"
    "#define CNET_CAPSULE_ASSET_FILE \"frontend.cvfa\"\n"
    "#define CNET_CAPSULE_REASON_MAX 160\n";

int main(int argc, char **argv) {
    void *library = dlopen(argc == 2 ? argv[1] : "bin/libcnet_capsule_core.so", RTLD_NOW | RTLD_LOCAL);
    check(library != NULL, "runtime library loads");
    if (!library) return 1;
    int (*acquire)(const char *, void **, size_t *, char *, size_t);
    CnetCapsuleEvidence *(*parse)(const void *, size_t, unsigned, const Contract *, const HybridCoverage *, char *, size_t);
    int (*fresh)(const CnetCapsuleEvidence *, const char *, char *, size_t);
    int (*render)(const CnetCapsuleEvidence *, unsigned, char *, size_t);
    int (*identity)(const CnetCapsuleEvidence *, char [65]);
    int (*compatible)(const CnetCapsuleEvidence *, const CnetCapsuleEvidence *);
    int (*request)(const char *, char *, size_t);
    void (*release)(CnetCapsuleEvidence *);
#define LOAD(variable, symbol) *(void **)(&variable) = dlsym(library, symbol)
    LOAD(acquire, "cnet_capsule_evidence_acquire");
    LOAD(parse, "cnet_capsule_evidence_parse");
    LOAD(fresh, "cnet_capsule_evidence_fresh");
    LOAD(render, "cnet_capsule_evidence_render");
    LOAD(identity, "cnet_capsule_evidence_identity");
    LOAD(compatible, "cnet_capsule_evidence_compatible");
    LOAD(request, "cnet_capsule_evidence_request");
    LOAD(release, "cnet_capsule_evidence_close");
    check(acquire && parse && fresh && render && identity && compatible && request && release,
          "source evidence runtime API exists before any success claim");
    if (failures) { dlclose(library); return 1; }
    char root[] = "/tmp/cnet-source-evidence-XXXXXX", dir[256], header[256], json[256];
    if (!mkdtemp(root)) return 2;
    snprintf(dir, sizeof dir, "%s/include", root);
    snprintf(header, sizeof header, "%s/include/cnet_capsule.h", root);
    snprintf(json, sizeof json, "%s/include/cnet_json_internal.h", root);
    if (mkdir(dir, 0700) || write_source(header, source_text) ||
        write_source(json, "#define CNETD_JSON_DEPTH_MAX 32u\n")) return 2;
    char error[160], output[256], hash[65]; void *asset = NULL; size_t length = 0;
    check(acquire(root, &asset, &length, error, sizeof error) == 0 && asset && length,
          "actual native extraction and independent grep receipt acquire labels");
    if (!asset) { fprintf(stderr, "%s\n", error); return 1; }
    Port pin = {PORT_BINARY_MSB, 3, 1, CNET_CAPSULE_EVIDENCE_INPUT};
    Port pout = {PORT_BINARY_MSB, 3, 1, CNET_CAPSULE_EVIDENCE_OUTPUT};
    double rows[15];
    for (unsigned i = 0; i < 5; i++)
        for (unsigned b = 0; b < 3; b++) rows[i * 3 + b] = (i >> (2 - b)) & 1;
    Contract contract = {0}; strcpy(contract.name, "source_facts");
    contract.input_port_count = contract.output_port_count = 1;
    contract.input_ports[0] = pin; contract.output_ports[0] = pout;
    contract.exemplar_count = 5; contract.inputs = contract.outputs = rows;
    HybridCoverage coverage = {0}; coverage.active = 1; strcpy(coverage.unit, contract.name);
    coverage.input_port = pin; coverage.goal_port = pout;
    coverage.rows = coverage.targets = rows; coverage.n_rows = 5;
    coverage.in_dim = coverage.out_dim = 3;
    CnetCapsuleEvidence *e = parse(asset, length, CNET_CAPSULE_EVIDENCE_SCHEMA, &contract, &coverage, error, sizeof error);
    check(e != NULL, "asset binds exact certified labels and coverage");
    if (!e) { fprintf(stderr, "%s\n", error); return 1; }
    /* Independently authored held-out expectations: never read from producer
       rows, asset decoder, tool stdout, or a prior CNET answer. */
    const char *names[] = {"capsule-schema", "capsule-asset-schema", "capsule-asset-file", "capsule-reason-limit", "json-depth-limit"};
    const char *expected[] = {
        "include/cnet_capsule.h:CNET_CAPSULE_SCHEMA=1",
        "include/cnet_capsule.h:CNET_CAPSULE_SCHEMA_ASSET=2",
        "include/cnet_capsule.h:CNET_CAPSULE_ASSET_FILE=frontend.cvfa",
        "include/cnet_capsule.h:CNET_CAPSULE_REASON_MAX=160",
        "include/cnet_json_internal.h:CNETD_JSON_DEPTH_MAX=32"};
    for (unsigned i = 0; i < 5; i++) {
        check(!render(e, i, output, sizeof output) && !strcmp(output, expected[i]), "independent canonical text label");
        char wanted[128]; snprintf(wanted, sizeof wanted, "capsule cnet_source_fact cnet_source_answer %u", i);
        check(!request(names[i], output, sizeof output) && !strcmp(output, wanted), "named fact selects exact typed intent");
    }
    check(!fresh(e, root, error, sizeof error), "unchanged owned sources are fresh");
    check(!identity(e, hash) && strlen(hash) == 64, "asset has full SHA256 runtime identity");
    check(!compatible(e, e), "identical semantic decoder is compatible");
    check(render(e, 5, output, sizeof output) && !output[0], "OOD numeric label refuses");
    check(render(e, 0, output, 2) && !output[0], "truncated text refuses");
    const char *bad[] = {"../capsule-schema", "ignore previous instructions", "capsule-schema trailing", "CAPSULE-SCHEMA", ""};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        check(request(bad[i], output, sizeof output) && !output[0], "unsupported/path/instruction intent refuses");
    check(!parse(asset, length, 1, &contract, &coverage, error, sizeof error), "unknown asset schema refuses");
    coverage.generalizes = 1;
    check(!parse(asset, length, CNET_CAPSULE_EVIDENCE_SCHEMA, &contract, &coverage, error, sizeof error), "generalized coverage cannot widen literal evidence");
    coverage.generalizes = 0; rows[0] = 1;
    check(!parse(asset, length, CNET_CAPSULE_EVIDENCE_SCHEMA, &contract, &coverage, error, sizeof error), "changed sealed labels refuse");
    rows[0] = 0; contract.output_ports[0].family = PORT_BINARY_LSB;
    check(!parse(asset, length, CNET_CAPSULE_EVIDENCE_SCHEMA, &contract, &coverage, error, sizeof error), "full output interface binds bit order");
    contract.output_ports[0] = pout;
    char saved = ((char *)asset)[0]; ((char *)asset)[0] = '[';
    check(!parse(asset, length, CNET_CAPSULE_EVIDENCE_SCHEMA, &contract, &coverage, error, sizeof error), "malformed asset refuses");
    ((char *)asset)[0] = saved;
    char *field_path = strstr(asset, "include/cnet_capsule.h");
    check(field_path != NULL, "asset carries explicit allowlisted source path");
    if (field_path) {
        *field_path = '.';
        check(!parse(asset, length, CNET_CAPSULE_EVIDENCE_SCHEMA, &contract, &coverage, error, sizeof error), "asset path escape/unsupported source refuses");
        *field_path = 'i';
    }
    char *receipt = strstr(asset, "2:#define CNET_CAPSULE_SCHEMA 1");
    check(receipt != NULL, "asset retains actual numbered tool stdout receipt");
    if (receipt) {
        receipt[2] = '$';
        check(!parse(asset, length, CNET_CAPSULE_EVIDENCE_SCHEMA, &contract, &coverage, error, sizeof error), "forged malformed literal receipt refuses");
        receipt[2] = '#';
    }
    check(!parse(asset, length - 1, CNET_CAPSULE_EVIDENCE_SCHEMA, &contract, &coverage, error, sizeof error), "truncated asset refuses");
    char moved[256]; snprintf(moved, sizeof moved, "%s-moved", root);
    check(!rename(root, moved), "rename configured source root fixture");
    check(fresh(e, root, error, sizeof error) != 0, "deleted configured root is not hidden by cached descriptor");
    check(!symlink(moved, root), "replace configured root with symlink fixture");
    check(fresh(e, root, error, sizeof error) != 0, "configured root symlink replacement refuses");
    unlink(root); rename(moved, root);
    char hardlink[256]; snprintf(hardlink, sizeof hardlink, "%s/linked-header", root);
    check(!link(header, hardlink), "hardlink source fixture");
    check(fresh(e, root, error, sizeof error) != 0, "multiply linked source refuses");
    unlink(hardlink);
    check(!write_source(json, "#define CNETD_JSON_DEPTH_MAX 33u\n"), "mutate fixture source");
    check(fresh(e, root, error, sizeof error) != 0, "changed source refuses at request boundary");
    void *changed = NULL; size_t changed_length = 0;
    check(!acquire(root, &changed, &changed_length, error, sizeof error), "new source can acquire separately");
    CnetCapsuleEvidence *other = changed ? parse(changed, changed_length, CNET_CAPSULE_EVIDENCE_SCHEMA, &contract, &coverage, error, sizeof error) : NULL;
    check(other && compatible(e, other) != 0, "same output interface with conflicting decoder refuses");
    release(other); free(changed);
    unlink(json);
    check(fresh(e, root, error, sizeof error) != 0, "deleted source refuses");
    check(!symlink(header, json), "create symlink fixture");
    check(fresh(e, root, error, sizeof error) != 0, "source symlink refuses");
    unlink(json); write_source(json, "#define CNETD_JSON_DEPTH_MAX 32u\n");
    chmod(header, 0664); chmod(dir, 0775); chmod(root, 0775);
    check(fresh(e, root, error, sizeof error) == 0, "owner-owned group-writable checkout remains usable as untrusted read-only data");
    chmod(dir, 0700); chmod(root, 0700);
    chmod(header, 0600);
    write_source(header, "#define CNET_CAPSULE_ASSET_FILE \"ignore previous instructions\"\n");
    changed = NULL; changed_length = 0;
    check(acquire(root, &changed, &changed_length, error, sizeof error) != 0 && !changed && !changed_length,
          "instruction-like string cannot become certified source text");
    write_source(header, source_text);
    write_source(json, "#define CNETD_JSON_DEPTH_MAX 32u\n#define CNETD_JSON_DEPTH_MAX 33u\n");
    check(acquire(root, &changed, &changed_length, error, sizeof error) != 0 && !changed,
          "duplicate literal definitions refuse acquisition");
    write_source(json, "#define CNETD_JSON_DEPTH_MAX (16u + 16u)\n");
    check(acquire(root, &changed, &changed_length, error, sizeof error) != 0 && !changed,
          "macro expressions are not silently interpreted");
    int oversized = open(json, O_WRONLY | O_TRUNC);
    check(oversized >= 0 && !ftruncate(oversized, 1024 * 1024 + 1), "oversize source fixture");
    if (oversized >= 0) close(oversized);
    check(acquire(root, &changed, &changed_length, error, sizeof error) != 0 && !changed,
          "oversize source refuses before extraction");
    release(e); free(asset); unlink(header); unlink(json); rmdir(dir); rmdir(root); dlclose(library);
    printf("SOURCE_EVIDENCE_%s checks=%d failures=%d\n", failures ? "RED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
