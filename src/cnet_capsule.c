#include "../include/cnet_capsule.h"

#include "../include/contract/contract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Hostile-manifest bounds. A capsule is one specialist, not a dataset. */
#define CAP_MAX_PAYLOAD   (64ULL * 1024 * 1024)
#define CAP_MAX_COV_ROWS  4096u
#define CAP_MAX_COV_DIM   65536u
#define CAP_MAX_COV_CELLS (4u * 1024u * 1024u)

#define CAP_UNIT_FILE "unit.cnb"
#define CAP_MANIFEST  "manifest.cknow"

static void cap_fail(CnetCapsuleReport *rep, const char *why) {
    if (rep) snprintf(rep->reject_reason, sizeof rep->reject_reason, "%s", why);
}

static int cap_path(char *out, size_t cap, const char *dir, const char *leaf) {
    int n = snprintf(out, cap, "%s/%s", dir, leaf);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

/* FNV-1a 64, standard offset basis. These checksums detect ACCIDENT —
   truncation, bit-rot, a half-written file, a mismatched build. They are
   unkeyed, so anyone able to rewrite a capsule can recompute them: this is
   emphatically not authenticity, and signing is out of scope. */
#define CAP_FNV_BASIS 14695981039346656037ULL
#define CAP_FNV_PRIME 1099511628211ULL

static unsigned long long cap_fnv_buf(const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    unsigned long long h = CAP_FNV_BASIS;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (unsigned long long)b[i];
        h *= CAP_FNV_PRIME;
    }
    return h;
}

/* Read a REGULAR file whole. O_NOFOLLOW + fstat means a symlinked or special
   payload is refused rather than followed, and hashing the same bytes we later
   load removes the hash-then-reopen TOCTOU window. */
static int cap_slurp(const char *path, unsigned char **out, size_t *len,
                     const char **why) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    unsigned char *buf;
    ssize_t got;
    size_t off = 0;
    *out = NULL;
    *len = 0;
    if (fd < 0) {
        *why = "payload_missing_or_symlink";
        return -1;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        *why = "payload_not_regular_file";
        return -1;
    }
    if (st.st_size <= 0 || (unsigned long long)st.st_size > CAP_MAX_PAYLOAD) {
        close(fd);
        *why = "payload_size_out_of_bounds";
        return -1;
    }
    /* +1 and NUL so callers may use string scans (strstr) on the manifest
       without reading one past the allocation; *len stays the logical size. */
    if ((unsigned long long)st.st_size + 1ULL > CAP_MAX_PAYLOAD) {
        close(fd);
        *why = "payload_size_out_of_bounds";
        return -1;
    }
    buf = (unsigned char *)malloc((size_t)st.st_size + 1);
    if (!buf) {
        close(fd);
        *why = "oom";
        return -1;
    }
    while (off < (size_t)st.st_size) {
        got = read(fd, buf + off, (size_t)st.st_size - off);
        if (got <= 0) {
            close(fd);
            free(buf);
            *why = "payload_read_failed";
            return -1;
        }
        off += (size_t)got;
    }
    close(fd);
    buf[off] = '\0';
    *out = buf;
    *len = off;
    return 0;
}

static void cap_tag_out(const char *tag, char *out, size_t cap) {
    if (!tag || !tag[0]) {
        snprintf(out, cap, "~");
        return;
    }
    snprintf(out, cap, "%s", tag);
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

/* Provenance already recorded on the unit ref, "" when the base has none. */
static const char *cap_provenance_of(const CnetBase *b, const char *unit) {
    size_t i;
    for (i = 0; i < b->unit_count; i++)
        if (strcmp(b->units[i].name, unit) == 0) return b->units[i].provenance;
    return "";
}

int cnet_capsule_export(const CnetBase *src, const HybridAi *cov,
                        const char *unit, const char *dir,
                        CnetCapsuleReport *rep) {
    CnetBase sub;
    BinaryTransformNetwork btn;
    Contract c;
    const HybridCoverage *hc;
    char unit_path[600], man_path[600], tmp_path[620];
    unsigned char *bytes_buf = NULL;
    unsigned long long fnv = 0, digest = 0;
    size_t bytes = 0, exemplars = 0;
    const char *why = "", *prov;
    char *man = NULL;
    size_t mcap = 0, mlen = 0;
    FILE *fp;
    int rc = -1, got_unit = 0;

    if (rep) memset(rep, 0, sizeof *rep);
    if (!src || !unit || !unit[0] || !dir || !dir[0]) return -1;
    if (!cnb_has_unit(src, unit)) { cap_fail(rep, "unit_not_in_base"); return -2; }
    if (cap_path(unit_path, sizeof unit_path, dir, CAP_UNIT_FILE) != 0 ||
        cap_path(man_path, sizeof man_path, dir, CAP_MANIFEST) != 0) {
        cap_fail(rep, "path_too_long");
        return -1;
    }
    if (snprintf(tmp_path, sizeof tmp_path, "%s.tmp", man_path) < 0) return -1;
    (void)mkdir(dir, 0777);

    memset(&btn, 0, sizeof btn);
    memset(&c, 0, sizeof c);
    if (cnb_get_unit(src, unit, &btn, &c) != 0) {
        cap_fail(rep, "unit_unreadable");
        return -3;
    }
    got_unit = 1;
    digest = (unsigned long long)contract_btn_digest(&btn);
    exemplars = c.exemplar_count;
    prov = cap_provenance_of(src, unit);

    cnb_init(&sub);
    if (cnb_export_subset(src, &sub, keep_named, (void *)unit) != 0) {
        cap_fail(rep, "subset_failed");
        goto done;
    }
    /* cnb_save is itself temp+rename, so the payload is never partial. */
    if (cnb_save(&sub, unit_path) != 0) {
        cap_fail(rep, "payload_write_failed");
        goto done;
    }
    if (cap_slurp(unit_path, &bytes_buf, &bytes, &why) != 0) {
        cap_fail(rep, why);
        goto done;
    }
    fnv = cap_fnv_buf(bytes_buf, bytes);

    /* Build the manifest in memory so the trailing checksum can cover every
       preceding byte — ports, provenance, and every coverage value included.
       Hashing only unit.cnb left all of that mutable without detection. */
    hc = cap_cov_for(cov, unit);
    mcap = 4096 + (hc ? hc->n_rows * (hc->in_dim + hc->out_dim) * 26 + 256 : 0);
    man = (char *)malloc(mcap);
    if (!man) { cap_fail(rep, "oom"); goto done; }
#define CAP_EMIT(...)                                                          \
    do {                                                                       \
        int _n = snprintf(man + mlen, mcap - mlen, __VA_ARGS__);               \
        if (_n < 0 || (size_t)_n >= mcap - mlen) {                             \
            cap_fail(rep, "manifest_overflow");                                \
            goto done;                                                         \
        }                                                                      \
        mlen += (size_t)_n;                                                    \
    } while (0)

    CAP_EMIT("CNET_CAPSULE %d\n", CNET_CAPSULE_SCHEMA);
    CAP_EMIT("unit %s\n", unit);
    CAP_EMIT("behavior_digest %llu\n", digest);
    CAP_EMIT("cnb_version %u\n", cnb_format_version());
    CAP_EMIT("exemplars %zu\n", exemplars);
    CAP_EMIT("payload_fnv %llu\n", fnv);
    CAP_EMIT("payload_bytes %zu\n", bytes);
    CAP_EMIT("provenance %s\n", (prov && prov[0]) ? prov : "~");
    {
        char itag[PORT_TAG_MAX + 4], gtag[PORT_TAG_MAX + 4];
        Port pi = btn.input_ports[0], po = btn.output_ports[0];
        cap_tag_out(pi.tag, itag, sizeof itag);
        cap_tag_out(po.tag, gtag, sizeof gtag);
        CAP_EMIT("in %d %zu %zu %s\n", (int)pi.family, pi.field_width,
                 pi.field_count, itag);
        CAP_EMIT("goal %d %zu %zu %s\n", (int)po.family, po.field_width,
                 po.field_count, gtag);
    }
    if (hc) {
        size_t r, j;
        CAP_EMIT("coverage %zu %zu %zu\n", hc->n_rows, hc->in_dim, hc->out_dim);
        for (r = 0; r < hc->n_rows; r++) {
            CAP_EMIT("COVIN");
            for (j = 0; j < hc->in_dim; j++)
                CAP_EMIT(" %.17g", hc->rows[r * hc->in_dim + j]);
            CAP_EMIT("\n");
        }
        if (hc->targets && hc->out_dim)
            for (r = 0; r < hc->n_rows; r++) {
                CAP_EMIT("COVOUT");
                for (j = 0; j < hc->out_dim; j++)
                    CAP_EMIT(" %.17g", hc->targets[r * hc->out_dim + j]);
                CAP_EMIT("\n");
            }
    } else {
        CAP_EMIT("coverage 0 0 0\n");
    }
#undef CAP_EMIT

    /* Exclusive + no-follow: a predictable sibling temp opened with fopen("wb")
       would follow an attacker-planted symlink and truncate the target. This is
       still only ACCIDENT hardening for a local artifact — see the trust
       boundary in the header. */
    {
        int tfd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
                                     O_CLOEXEC, 0600);
        if (tfd < 0) {
            (void)remove(tmp_path);
            tfd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW |
                                     O_CLOEXEC, 0600);
        }
        if (tfd < 0) { cap_fail(rep, "manifest_write_failed"); goto done; }
        fp = fdopen(tfd, "wb");
        if (!fp) { close(tfd); (void)remove(tmp_path);
                   cap_fail(rep, "manifest_write_failed"); goto done; }
    }
    if (fwrite(man, 1, mlen, fp) != mlen ||
        fprintf(fp, "manifest_fnv %llu\n", cap_fnv_buf(man, mlen)) < 0 ||
        fclose(fp) != 0) {
        (void)remove(tmp_path);
        cap_fail(rep, "manifest_write_failed");
        goto done;
    }
    if (rename(tmp_path, man_path) != 0) {
        (void)remove(tmp_path);
        cap_fail(rep, "manifest_publish_failed");
        goto done;
    }

    if (rep) {
        snprintf(rep->unit, sizeof rep->unit, "%s", unit);
        rep->behavior_digest = digest;
        rep->exemplars = exemplars;
        rep->coverage_rows = hc ? hc->n_rows : 0;
        rep->payload_bytes = bytes;
        rep->cnb_version = cnb_format_version();
        snprintf(rep->provenance, sizeof rep->provenance, "%s",
                 (prov && prov[0]) ? prov : "");
    }
    rc = 0;
done:
    if (got_unit) { btn_free(&btn); contract_free(&c); }
    free(bytes_buf);
    free(man);
    cnb_free(&sub);
    return rc;
}

int cnet_capsule_import(CnetBase *dst, HybridAi *cov, const char *dir,
                        CnetCapsuleReport *rep) {
    CnetBase sub;
    BinaryTransformNetwork btn;
    Contract c;
    char unit_path[600], man_path[600], tok[64];
    char unit[96] = {0}, itag[PORT_TAG_MAX + 4], gtag[PORT_TAG_MAX + 4];
    char prov[CNB_NAME_MAX] = {0};
    char prov_raw[256] = {0}; /* scratch: %255s matches THIS size, not prov */
    unsigned char *pay = NULL, *manbuf = NULL;
    size_t paylen = 0, manlen = 0;
    unsigned long long want_fnv = 0, want_digest = 0, want_man = 0;
    size_t want_bytes = 0, exemplars = 0, cov_rows = 0, cov_in = 0, cov_out = 0;
    double *cin = NULL, *cout = NULL;
    const char *why = "";
    char tmpl[] = "/tmp/cnet_capsuleXXXXXX";
    int tmpfd = -1, have_tmp = 0;
    unsigned ver = 0;
    int schema = 0, ifam = 0, gfam = 0, loaded = 0, rc = -1, cov_stored = 0;
    size_t iw = 0, ic = 0, gw = 0, gc = 0, r, j;
    Port pin, pout;
    FILE *fp;

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

    /* ---- manifest: read whole, verify its own checksum, THEN parse -------
       The checksum covers ports, provenance and every coverage value, so a
       rewritten coverage row or tag is caught before it can be believed. */
    if (cap_slurp(man_path, &manbuf, &manlen, &why) != 0) {
        cap_fail(rep, "manifest_missing_or_symlink");
        goto done;
    }
    {
        const char *tail = strstr((const char *)manbuf, "\nmanifest_fnv ");
        size_t covered;
        if (!tail) { cap_fail(rep, "manifest_unchecksummed"); goto done; }
        covered = (size_t)(tail - (const char *)manbuf) + 1;
        if (sscanf(tail + 1, "manifest_fnv %llu", &want_man) != 1) {
            cap_fail(rep, "manifest_checksum_unreadable");
            goto done;
        }
        if (cap_fnv_buf(manbuf, covered) != want_man) {
            cap_fail(rep, "manifest_integrity_mismatch");
            goto done;
        }
    }
    fp = fmemopen(manbuf, manlen, "rb");
    if (!fp) { cap_fail(rep, "manifest_unreadable"); goto done; }
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
        fscanf(fp, " provenance %255s", prov_raw) != 1 ||
        fscanf(fp, " in %d %zu %zu %35s", &ifam, &iw, &ic, itag) != 4 ||
        fscanf(fp, " goal %d %zu %zu %35s", &gfam, &gw, &gc, gtag) != 4 ||
        fscanf(fp, " coverage %zu %zu %zu", &cov_rows, &cov_in, &cov_out) != 3) {
        cap_fail(rep, "manifest_parse_failed");
        fclose(fp);
        goto done;
    }
    /* Read into a scratch whose size matches the %255s bound, then range-check
       before it reaches prov[CNB_NAME_MAX]. The previous "%127s" into a
       64-byte buffer could write 128 bytes onto the stack. */
    if (strlen(prov_raw) >= CNB_NAME_MAX) {
        cap_fail(rep, "provenance_field_too_long");
        fclose(fp);
        goto done;
    }
    memcpy(prov, prov_raw, strlen(prov_raw) + 1);
    if (ver != cnb_format_version()) {
        char w[CNET_CAPSULE_REASON_MAX];
        snprintf(w, sizeof w, "incompatible_cnb_version=%u_expected=%u", ver,
                 cnb_format_version());
        cap_fail(rep, w);
        fclose(fp);
        goto done;
    }
    /* Bound every dimension AND the product before any allocation. */
    if (cov_rows) {
        if (cov_rows > CAP_MAX_COV_ROWS || cov_in == 0 ||
            cov_in > CAP_MAX_COV_DIM || cov_out > CAP_MAX_COV_DIM ||
            cov_rows > CAP_MAX_COV_CELLS / cov_in ||
            (cov_out && cov_rows > CAP_MAX_COV_CELLS / cov_out)) {
            cap_fail(rep, "coverage_bounds");
            fclose(fp);
            goto done;
        }
    /* Bind cov_out to the goal port the manifest DECLARES, before parsing any
       COVOUT rows — otherwise a wrong cov_out is caught by a row-count parse
       accident instead of by the contract. The declared ports are themselves
       verified against the payload further down, so this cannot be gamed by
       editing the goal line. */
    if (cov_rows && cov_out != 0) {
        size_t decl_out;
        if (gc && gw > (size_t)-1 / gc) {
            cap_fail(rep, "port_dim_overflow");
            fclose(fp);
            goto done;
        }
        decl_out = gw * gc;
        if (cov_out != decl_out) {
            cap_fail(rep, "coverage_out_dim_mismatch");
            fclose(fp);
            goto done;
        }
    }
        /* A capsule that carries a gate must not import where the gate cannot
           be stored: dropping it would hand over an ungated certified unit. */
        if (!cov) {
            cap_fail(rep, "coverage_present_but_no_target_registry");
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

    /* ---- payload: hash the exact bytes we will load ---------------------- */
    if (cap_slurp(unit_path, &pay, &paylen, &why) != 0) {
        cap_fail(rep, why);
        goto done;
    }
    if (paylen != want_bytes || cap_fnv_buf(pay, paylen) != want_fnv) {
        cap_fail(rep, "payload_integrity_mismatch");
        goto done;
    }
    /* cnb_load needs a path, so load from a private copy of the verified bytes
       rather than re-opening the caller's file, which could change underneath. */
    tmpfd = mkstemp(tmpl);
    if (tmpfd < 0) { cap_fail(rep, "temp_create_failed"); goto done; }
    have_tmp = 1;
    if (write(tmpfd, pay, paylen) != (ssize_t)paylen) {
        cap_fail(rep, "temp_write_failed");
        goto done;
    }
    close(tmpfd);
    tmpfd = -1;
    if (cnb_load(&sub, tmpl) != 0) { cap_fail(rep, "payload_unreadable"); goto done; }
    if (!cnb_has_unit(&sub, unit)) {
        cap_fail(rep, "unit_absent_from_payload");
        goto done;
    }
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
    /* Payload ports are authoritative. The manifest must AGREE with them —
       including tags — and is then never used in their place. */
    pin = btn.input_ports[0];
    pout = btn.output_ports[0];
    {
        char pit[PORT_TAG_MAX + 4], pgt[PORT_TAG_MAX + 4];
        cap_tag_out(pin.tag, pit, sizeof pit);
        cap_tag_out(pout.tag, pgt, sizeof pgt);
        if ((int)pin.family != ifam || pin.field_width != iw ||
            pin.field_count != ic || (int)pout.family != gfam ||
            pout.field_width != gw || pout.field_count != gc ||
            strcmp(pit, itag) != 0 || strcmp(pgt, gtag) != 0) {
            cap_fail(rep, "contract_port_mismatch");
            goto done;
        }
    }
    if (c.exemplar_count != exemplars) {
        cap_fail(rep, "exemplar_count_mismatch");
        goto done;
    }
    {
        const char *pp = cap_provenance_of(&sub, unit);
        const char *want = (prov[0] && strcmp(prov, "~")) ? prov : "";
        if (strcmp(pp ? pp : "", want) != 0) {
            cap_fail(rep, "provenance_mismatch");
            goto done;
        }
    }
    if (cov_rows) {
        size_t in_tot, out_tot;
        if (pin.field_count && pin.field_width > (size_t)-1 / pin.field_count) {
            cap_fail(rep, "port_dim_overflow");
            goto done;
        }
        if (pout.field_count && pout.field_width > (size_t)-1 / pout.field_count) {
            cap_fail(rep, "port_dim_overflow");
            goto done;
        }
        in_tot = pin.field_width * pin.field_count;
        out_tot = pout.field_width * pout.field_count;
        if (cov_in != in_tot) {
            cap_fail(rep, "coverage_in_dim_mismatch");
            goto done;
        }
        /* Generic caps let a legal-but-wrong cov_out through; bind it to the
           unit's real output port so a mismatch is refused by the CONTRACT and
           not by a downstream parse accident. */
        if (cov_out != 0 && cov_out != out_tot) {
            cap_fail(rep, "coverage_out_dim_mismatch");
            goto done;
        }
    }

    /* ---- commit: gate FIRST, because only it can be rolled back ----------
       cnb_add_unit has no removal counterpart, so storing coverage first and
       forgetting it on failure is the only ordering where a failed import
       cannot leave an ungated certified unit behind. */
    if (cov_rows) {
        /* Coverage records are keyed by PORTS, not unit names. Writing one for
           an occupied shape frees the incumbent's rows and leaves that older
           unit default-allow, and forget_unit cannot put it back. Refuse the
           conflict instead of displacing it. */
        const char *owner = hybrid_coverage_owner(cov, pin, pout);
        if (owner && strcmp(owner, unit) != 0) {
            char w[CNET_CAPSULE_REASON_MAX];
            snprintf(w, sizeof w, "coverage_port_conflict_owned_by=%.90s", owner);
            cap_fail(rep, w);
            goto done;
        }
        if (hybrid_coverage_record(cov, pin, pout, unit, cin, cout, cov_rows,
                                   cov_in, cov_out) != 0) {
            cap_fail(rep, "coverage_restore_failed");
            goto done;
        }
        cov_stored = 1;
    }
    if (cnb_add_unit(dst, &btn, &c, NULL) != 0) {
        if (cov_stored) (void)hybrid_coverage_forget_unit(cov, unit);
        cap_fail(rep, "target_admit_refused");
        goto done;
    }
    if (rep) {
        snprintf(rep->unit, sizeof rep->unit, "%s", unit);
        rep->behavior_digest = want_digest;
        rep->exemplars = exemplars;
        rep->coverage_rows = cov_rows;
        rep->payload_bytes = paylen;
        rep->cnb_version = ver;
        snprintf(rep->provenance, sizeof rep->provenance, "%s",
                 (prov[0] && strcmp(prov, "~")) ? prov : "");
    }
    rc = 0;
done:
    if (tmpfd >= 0) close(tmpfd);
    if (have_tmp) (void)remove(tmpl);
    if (loaded) { btn_free(&btn); contract_free(&c); }
    free(cin);
    free(cout);
    free(pay);
    free(manbuf);
    cnb_free(&sub);
    return rc;
}
