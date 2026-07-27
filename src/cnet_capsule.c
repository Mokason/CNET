#include "../include/cnet_capsule.h"

#include "../include/contract/contract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define CAP_UNIT_FILE "unit.cnb"
#define CAP_MANIFEST  "manifest.cknow"

static void cap_fail(CnetCapsuleReport *rep, const char *why) {
    if (rep) snprintf(rep->reject_reason, sizeof rep->reject_reason, "%s", why);
}

static int cap_path(char *out, size_t cap, const char *dir, const char *leaf) {
    int n = snprintf(out, cap, "%s/%s", dir, leaf);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

/* FNV-1a 64 over the payload file. Integrity here is a corruption check, not a
   signature: a capsule is a local transfer object, and pretending otherwise
   would imply authenticity guarantees no PKI is present to back. */
static int cap_file_fnv(const char *path, unsigned long long *out,
                        size_t *size_out) {
    FILE *fp = fopen(path, "rb");
    unsigned long long h = 1469598103934665603ULL;
    unsigned char buf[8192];
    size_t n, total = 0;
    if (!fp) return -1;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) {
        size_t i;
        for (i = 0; i < n; i++) {
            h ^= (unsigned long long)buf[i];
            h *= 1099511628211ULL;
        }
        total += n;
    }
    fclose(fp);
    *out = h;
    if (size_out) *size_out = total;
    return 0;
}

static void cap_tag_out(const char *tag, char *out, size_t cap) {
    if (!tag || !tag[0]) {
        snprintf(out, cap, "~");
        return;
    }
    snprintf(out, cap, "%s", tag);
}

static void cap_tag_in(const char *tok, char *out, size_t cap) {
    if (!strcmp(tok, "~")) {
        out[0] = '\0';
        return;
    }
    snprintf(out, cap, "%s", tok);
}

static int keep_named(const char *name, void *ctx) {
    return strcmp(name, (const char *)ctx) == 0;
}

/* Locate the coverage record for a unit. Public struct, so no accessor churn. */
static const HybridCoverage *cap_cov_for(const HybridAi *cov,
                                         const char *unit) {
    size_t i;
    if (!cov || !unit) return NULL;
    for (i = 0; i < cov->coverage_count; i++) {
        const HybridCoverage *c = &cov->coverage[i];
        if (c->active && c->rows && strcmp(c->unit, unit) == 0) return c;
    }
    return NULL;
}

int cnet_capsule_export(const CnetBase *src, const HybridAi *cov,
                        const char *unit, const char *dir,
                        CnetCapsuleReport *rep) {
    CnetBase sub;
    BinaryTransformNetwork btn;
    Contract c;
    const HybridCoverage *hc;
    char unit_path[600], man_path[600];
    unsigned long long fnv = 0, digest = 0;
    size_t bytes = 0, exemplars = 0;
    FILE *fp;
    int rc = -1;

    if (rep) memset(rep, 0, sizeof *rep);
    if (!src || !unit || !unit[0] || !dir || !dir[0]) return -1;
    if (!cnb_has_unit(src, unit)) {
        cap_fail(rep, "unit_not_in_base");
        return -2;
    }
    if (cap_path(unit_path, sizeof unit_path, dir, CAP_UNIT_FILE) != 0 ||
        cap_path(man_path, sizeof man_path, dir, CAP_MANIFEST) != 0) {
        cap_fail(rep, "path_too_long");
        return -1;
    }
    (void)mkdir(dir, 0777);

    /* Identity + typed contract come from the sealed unit itself, never from
       the caller: a manifest that could disagree with its payload is not a
       compatibility check, it is a second source of truth. */
    memset(&btn, 0, sizeof btn);
    memset(&c, 0, sizeof c);
    if (cnb_get_unit(src, unit, &btn, &c) != 0) {
        cap_fail(rep, "unit_unreadable");
        return -3;
    }
    digest = (unsigned long long)contract_btn_digest(&btn);
    exemplars = c.exemplar_count;

    cnb_init(&sub);
    if (cnb_export_subset(src, &sub, keep_named, (void *)unit) != 0) {
        cap_fail(rep, "subset_failed");
        goto done;
    }
    if (cnb_save(&sub, unit_path) != 0) {
        cap_fail(rep, "payload_write_failed");
        goto done;
    }
    if (cap_file_fnv(unit_path, &fnv, &bytes) != 0) {
        cap_fail(rep, "payload_unreadable");
        goto done;
    }

    hc = cap_cov_for(cov, unit);
    fp = fopen(man_path, "w");
    if (!fp) {
        cap_fail(rep, "manifest_write_failed");
        goto done;
    }
    fprintf(fp, "CNET_CAPSULE %d\n", CNET_CAPSULE_SCHEMA);
    fprintf(fp, "unit %s\n", unit);
    fprintf(fp, "behavior_digest %llu\n", digest);
    fprintf(fp, "cnb_version %u\n", cnb_format_version());
    fprintf(fp, "exemplars %zu\n", exemplars);
    fprintf(fp, "payload_fnv %llu\n", fnv);
    fprintf(fp, "payload_bytes %zu\n", bytes);
    {
        char itag[PORT_TAG_MAX + 4], gtag[PORT_TAG_MAX + 4];
        Port pin = btn.input_ports[0], pout = btn.output_ports[0];
        cap_tag_out(pin.tag, itag, sizeof itag);
        cap_tag_out(pout.tag, gtag, sizeof gtag);
        fprintf(fp, "in %d %zu %zu %s\n", (int)pin.family, pin.field_width,
                pin.field_count, itag);
        fprintf(fp, "goal %d %zu %zu %s\n", (int)pout.family, pout.field_width,
                pout.field_count, gtag);
    }
    if (hc) {
        size_t r, j;
        fprintf(fp, "coverage %zu %zu %zu\n", hc->n_rows, hc->in_dim,
                hc->out_dim);
        for (r = 0; r < hc->n_rows; r++) {
            fputs("COVIN", fp);
            for (j = 0; j < hc->in_dim; j++)
                fprintf(fp, " %.17g", hc->rows[r * hc->in_dim + j]);
            fputc('\n', fp);
        }
        if (hc->targets && hc->out_dim) {
            for (r = 0; r < hc->n_rows; r++) {
                fputs("COVOUT", fp);
                for (j = 0; j < hc->out_dim; j++)
                    fprintf(fp, " %.17g", hc->targets[r * hc->out_dim + j]);
                fputc('\n', fp);
            }
        }
    } else {
        fprintf(fp, "coverage 0 0 0\n");
    }
    if (fclose(fp) != 0) {
        cap_fail(rep, "manifest_flush_failed");
        goto done;
    }

    if (rep) {
        snprintf(rep->unit, sizeof rep->unit, "%s", unit);
        rep->behavior_digest = digest;
        rep->exemplars = exemplars;
        rep->coverage_rows = hc ? hc->n_rows : 0;
        rep->payload_bytes = bytes;
        rep->cnb_version = cnb_format_version();
    }
    rc = 0;
done:
    btn_free(&btn);
    contract_free(&c);
    cnb_free(&sub);
    return rc;
}

int cnet_capsule_import(CnetBase *dst, HybridAi *cov, const char *dir,
                        CnetCapsuleReport *rep) {
    CnetBase sub;
    BinaryTransformNetwork btn;
    Contract c;
    FILE *fp;
    char unit_path[600], man_path[600], tok[64];
    char unit[96] = {0}, itag[PORT_TAG_MAX + 4], gtag[PORT_TAG_MAX + 4];
    unsigned long long want_fnv = 0, have_fnv = 0, want_digest = 0;
    size_t want_bytes = 0, have_bytes = 0, exemplars = 0;
    size_t cov_rows = 0, cov_in = 0, cov_out = 0, r, j;
    double *cin = NULL, *cout = NULL;
    unsigned ver = 0;
    int schema = 0, ifam = 0, gfam = 0, loaded = 0, rc = -1;
    size_t iw = 0, ic = 0, gw = 0, gc = 0;
    Port pin, pout;

    if (rep) memset(rep, 0, sizeof *rep);
    if (!dst || !dir || !dir[0]) return -1;
    if (cap_path(unit_path, sizeof unit_path, dir, CAP_UNIT_FILE) != 0 ||
        cap_path(man_path, sizeof man_path, dir, CAP_MANIFEST) != 0) {
        cap_fail(rep, "path_too_long");
        return -1;
    }
    memset(&btn, 0, sizeof btn);
    memset(&c, 0, sizeof c);
    cnb_init(&sub);

    fp = fopen(man_path, "r");
    if (!fp) {
        cap_fail(rep, "manifest_missing");
        goto done;
    }
    if (fscanf(fp, "%15s %d", tok, &schema) != 2 ||
        strcmp(tok, "CNET_CAPSULE") != 0 || schema != CNET_CAPSULE_SCHEMA) {
        cap_fail(rep, "bad_manifest_header");
        fclose(fp);
        goto done;
    }
    if (fscanf(fp, " unit %95s", unit) != 1 ||
        fscanf(fp, " behavior_digest %llu", &want_digest) != 1 ||
        fscanf(fp, " cnb_version %u", &ver) != 1 ||
        fscanf(fp, " exemplars %zu", &exemplars) != 1 ||
        fscanf(fp, " payload_fnv %llu", &want_fnv) != 1 ||
        fscanf(fp, " payload_bytes %zu", &want_bytes) != 1 ||
        fscanf(fp, " in %d %zu %zu %35s", &ifam, &iw, &ic, itag) != 4 ||
        fscanf(fp, " goal %d %zu %zu %35s", &gfam, &gw, &gc, gtag) != 4 ||
        fscanf(fp, " coverage %zu %zu %zu", &cov_rows, &cov_in, &cov_out) != 3) {
        cap_fail(rep, "manifest_parse_failed");
        fclose(fp);
        goto done;
    }
    /* Compatibility BEFORE touching the payload: a container this build cannot
       read must not be half-parsed into the target. */
    if (ver != cnb_format_version()) {
        char why[CNET_CAPSULE_REASON_MAX];
        snprintf(why, sizeof why, "incompatible_cnb_version=%u_expected=%u", ver,
                 cnb_format_version());
        cap_fail(rep, why);
        fclose(fp);
        goto done;
    }
    if (cov_rows) {
        if (cov_in == 0 || cov_rows > (size_t)1 << 20 ||
            cov_in > (size_t)1 << 20) {
            cap_fail(rep, "coverage_bounds");
            fclose(fp);
            goto done;
        }
        cin = (double *)calloc(cov_rows * cov_in, sizeof(double));
        if (cov_out) cout = (double *)calloc(cov_rows * cov_out, sizeof(double));
        if (!cin || (cov_out && !cout)) {
            cap_fail(rep, "oom");
            fclose(fp);
            goto done;
        }
        for (r = 0; r < cov_rows; r++) {
            if (fscanf(fp, " %15s", tok) != 1 || strcmp(tok, "COVIN") != 0) {
                cap_fail(rep, "coverage_rows_truncated");
                fclose(fp);
                goto done;
            }
            for (j = 0; j < cov_in; j++)
                if (fscanf(fp, " %lf", &cin[r * cov_in + j]) != 1) {
                    cap_fail(rep, "coverage_rows_truncated");
                    fclose(fp);
                    goto done;
                }
        }
        for (r = 0; cout && r < cov_rows; r++) {
            if (fscanf(fp, " %15s", tok) != 1 || strcmp(tok, "COVOUT") != 0) {
                cap_fail(rep, "coverage_targets_truncated");
                fclose(fp);
                goto done;
            }
            for (j = 0; j < cov_out; j++)
                if (fscanf(fp, " %lf", &cout[r * cov_out + j]) != 1) {
                    cap_fail(rep, "coverage_targets_truncated");
                    fclose(fp);
                    goto done;
                }
        }
    }
    fclose(fp);

    if (cap_file_fnv(unit_path, &have_fnv, &have_bytes) != 0) {
        cap_fail(rep, "payload_missing");
        goto done;
    }
    if (have_bytes != want_bytes || have_fnv != want_fnv) {
        cap_fail(rep, "payload_integrity_mismatch");
        goto done;
    }
    if (cnb_load(&sub, unit_path) != 0) {
        cap_fail(rep, "payload_unreadable");
        goto done;
    }
    if (!cnb_has_unit(&sub, unit)) {
        cap_fail(rep, "unit_absent_from_payload");
        goto done;
    }
    /* Materialising re-verifies the per-blob CNU1 seal. */
    if (cnb_get_unit(&sub, unit, &btn, &c) != 0) {
        cap_fail(rep, "unit_seal_failed");
        goto done;
    }
    loaded = 1;
    if ((unsigned long long)contract_btn_digest(&btn) != want_digest) {
        cap_fail(rep, "behavior_digest_mismatch");
        goto done;
    }
    if (btn.input_port_count < 1 || btn.output_port_count < 1) {
        cap_fail(rep, "unit_has_no_ports");
        goto done;
    }
    pin = btn.input_ports[0];
    pout = btn.output_ports[0];
    if ((int)pin.family != ifam || pin.field_width != iw ||
        pin.field_count != ic || (int)pout.family != gfam ||
        pout.field_width != gw || pout.field_count != gc) {
        cap_fail(rep, "contract_port_mismatch");
        goto done;
    }
    if (c.exemplar_count != exemplars) {
        cap_fail(rep, "exemplar_count_mismatch");
        goto done;
    }
    if (cov_rows && cov_in != pin.field_width * pin.field_count) {
        cap_fail(rep, "coverage_dim_mismatch");
        goto done;
    }

    /* Every check passed — only now mutate the target. */
    if (cnb_add_unit(dst, &btn, &c, NULL) != 0) {
        cap_fail(rep, "target_admit_refused");
        goto done;
    }
    if (cov && cov_rows) {
        Port rin = pin, rout = pout;
        cap_tag_in(itag, rin.tag, sizeof rin.tag);
        cap_tag_in(gtag, rout.tag, sizeof rout.tag);
        if (hybrid_coverage_record(cov, rin, rout, unit, cin,
                                   cout, cov_rows, cov_in, cov_out) != 0) {
            cap_fail(rep, "coverage_restore_failed");
            goto done;
        }
    }
    if (rep) {
        snprintf(rep->unit, sizeof rep->unit, "%s", unit);
        rep->behavior_digest = want_digest;
        rep->exemplars = exemplars;
        rep->coverage_rows = cov_rows;
        rep->payload_bytes = have_bytes;
        rep->cnb_version = ver;
    }
    rc = 0;
done:
    if (loaded) {
        btn_free(&btn);
        contract_free(&c);
    }
    free(cin);
    free(cout);
    cnb_free(&sub);
    return rc;
}
