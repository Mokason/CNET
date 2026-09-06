#include "../../include/cnet_capsule.h"

#include "../../include/contract/contract.h"

#include <stdio.h>
#include <ctype.h>
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

/* Read a REGULAR file whole. CNET_O_NOFOLLOW + fstat means a symlinked or special
   payload is refused rather than followed, and hashing the same bytes we later
   load removes the hash-then-reopen TOCTOU window. */
static int cap_slurp(const char *path, unsigned char **out, size_t *len,
                     const char **why) {
    int fd = open(path, O_RDONLY | CNET_O_NOFOLLOW | CNET_O_CLOEXEC);
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

/* ---- certification scope -------------------------------------------------
 *
 * The capsule invariant is "a certified unit and its abstention boundary travel
 * together". Coverage was optional and `coverage 0 0 0` was written whenever a
 * caller passed no HybridAi, so a unit certified on a SAMPLE of its domain could
 * be exported, imported, re-certified against that same sample, and then answer
 * anywhere. The header said `cov == NULL` was correct only for a whole-domain
 * unit; nothing ever proved whole-domain.
 *
 * "Exhaustive" is now a machine-checkable claim rather than a caller's word: the
 * input port's domain must be finite and small enough to enumerate, and the
 * contract's exemplar inputs must be exactly that domain -- every one a legal
 * member of the family, all distinct, and as many as the domain has points.
 * Anything else is `sampled` and MUST carry coverage.
 */
#define CAP_DOMAIN_CAP (1u << 20)

#define CAP_SCOPE_EXHAUSTIVE "exhaustive"
#define CAP_SCOPE_SAMPLED    "sampled"

/* 1 and *out set when the port's domain is finite and enumerable. */
static int cap_domain_size(Port p, size_t *out) {
    size_t n = 1, i, total;
    if (p.field_width == 0 || p.field_count == 0) return 0;
    switch (p.family) {
        case PORT_ONEHOT:
            /* field_count independent one-hot fields of field_width each. */
            for (i = 0; i < p.field_count; i++) {
                if (n > CAP_DOMAIN_CAP / p.field_width) return 0;
                n *= p.field_width;
            }
            *out = n;
            return 1;
        case PORT_BINARY_MSB:
        case PORT_BINARY_LSB:
            if (p.field_width > (size_t)-1 / p.field_count) return 0;
            total = p.field_width * p.field_count;
            if (total >= 20) return 0; /* 2^20 is already the cap */
            *out = (size_t)1u << total;
            return 1;
        default:
            /* PORT_RAW is unbounded; EVIDENCE/CONCEPT have no enumerable
               membership. Neither can support an exhaustiveness proof. */
            return 0;
    }
}

/* Is this row a legal point of the port's domain? */
static int cap_row_member(Port p, const double *row) {
    size_t f, j;
    switch (p.family) {
        case PORT_ONEHOT:
            for (f = 0; f < p.field_count; f++) {
                size_t hot = 0;
                for (j = 0; j < p.field_width; j++) {
                    double v = row[f * p.field_width + j];
                    if (v == 1.0) hot++;
                    else if (v != 0.0) return 0;
                }
                if (hot != 1) return 0;
            }
            return 1;
        case PORT_BINARY_MSB:
        case PORT_BINARY_LSB:
            for (j = 0; j < p.field_width * p.field_count; j++)
                if (row[j] != 0.0 && row[j] != 1.0) return 0;
            return 1;
        default:
            return 0;
    }
}

/* Prove the contract's exemplars exhaust the input port's domain. */
static int cap_scope_exhaustive(const Contract *c, size_t *domain_out) {
    size_t domain, width, i, j;
    Port in_port;
    if (!c || !c->inputs || c->input_port_count != 1) return 0;
    in_port = c->input_ports[0];
    if (!cap_domain_size(in_port, &domain)) return 0;
    if (c->exemplar_count != domain) return 0;
    if (in_port.field_width > (size_t)-1 / in_port.field_count) return 0;
    width = in_port.field_width * in_port.field_count;
    if (width == 0) return 0;
    for (i = 0; i < c->exemplar_count; i++) {
        const double *row = c->inputs + i * width;
        if (!cap_row_member(in_port, row)) return 0;
        /* Distinctness: exemplar_count == |domain| proves nothing if the same
           point appears twice. O(n^2) over a domain capped at 2^20 points, and
           only at export/import time. */
        for (j = 0; j < i; j++)
            if (memcmp(row, c->inputs + j * width,
                       width * sizeof(double)) == 0)
                return 0;
    }
    if (domain_out) *domain_out = domain;
    return 1;
}

/* Provenance already recorded on the unit ref, "" when the base has none. */
static const char *cap_provenance_of(const CnetBase *b, const char *unit) {
    size_t i;
    for (i = 0; i < b->unit_count; i++)
        if (strcmp(b->units[i].name, unit) == 0) return b->units[i].provenance;
    return "";
}

int cnet_capsule_export_asset(const CnetBase *src, const HybridAi *cov,
                        const char *unit, const char *dir,
                        const void *asset, size_t asset_len,
                        unsigned asset_schema, CnetCapsuleReport *rep) {
    CnetBase sub;
    BinaryTransformNetwork btn;
    Contract c;
    const HybridCoverage *hc;
    char unit_path[600], man_path[600], tmp_path[620];
    unsigned char *bytes_buf = NULL;
    unsigned long long fnv = 0, digest = 0;
    size_t bytes = 0, exemplars = 0;
    const char *why = "", *prov, *scope;
    char *man = NULL;
    size_t mcap = 0, mlen = 0;
    FILE *fp;
    int rc = -1, got_unit = 0;

    if (rep) memset(rep, 0, sizeof *rep);
    if (!src || !unit || !unit[0] || !dir || !dir[0]) return -1;
    /* Schema parity: import refuses a zero-byte asset, so publishing one would
       report success for an artifact its own importer rejects. One validator,
       both directions. */
    if (asset && asset_len == 0) {
        cap_fail(rep, "zero_length_asset_would_not_import");
        return -1;
    }
    if (!cnb_has_unit(src, unit)) { cap_fail(rep, "unit_not_in_base"); return -2; }
    if (cap_path(unit_path, sizeof unit_path, dir, CAP_UNIT_FILE) != 0 ||
        cap_path(man_path, sizeof man_path, dir, CAP_MANIFEST) != 0) {
        cap_fail(rep, "path_too_long");
        return -1;
    }
    if (snprintf(tmp_path, sizeof tmp_path, "%s.tmp-XXXXXX", man_path) < 0) return -1;
    (void)cnet_mkdir(dir, 0777);

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

    /* Scope is decided here, from the sealed contract, and is not a caller
       claim. A unit whose exemplars do not exhaust its domain must ship with
       the gate that says where it stops answering. Checked after cnb_init so
       the shared cleanup path has a valid subset to free. */
    hc = cap_cov_for(cov, unit);
    scope = cap_scope_exhaustive(&c, NULL) ? CAP_SCOPE_EXHAUSTIVE
                                           : CAP_SCOPE_SAMPLED;
    if (strcmp(scope, CAP_SCOPE_SAMPLED) == 0 && (!hc || hc->n_rows == 0)) {
        cap_fail(rep, "sampled_scope_requires_coverage");
        rc = -2;
        goto done;
    }
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

    CAP_EMIT("CNET_CAPSULE %d\n",
             asset ? CNET_CAPSULE_SCHEMA_ASSET : CNET_CAPSULE_SCHEMA);
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
    CAP_EMIT("scope %s\n", scope);
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
    /* Bound the sidecar the same way the payload is bound, inside the region
       the trailing manifest_fnv already covers. */
    if (asset)
        CAP_EMIT("asset %u %zu %llu %s\n", asset_schema, asset_len,
                 cap_fnv_buf((const unsigned char *)asset, asset_len),
                 CNET_CAPSULE_ASSET_FILE);
#undef CAP_EMIT

    if (asset) {
        char apath[600], atmp[600];
        int afd;
        if (asset_len > CNET_CAPSULE_MAX_ASSET) {
            cap_fail(rep, "asset_too_large"); goto done;
        }
        if (cap_path(apath, sizeof apath, dir, CNET_CAPSULE_ASSET_FILE) != 0 ||
            cap_path(atmp, sizeof atmp, dir, CNET_CAPSULE_ASSET_FILE ".tmp") != 0) {
            cap_fail(rep, "path_too_long"); goto done;
        }
        (void)remove(atmp);
        afd = open(atmp, O_WRONLY | O_CREAT | O_EXCL | CNET_O_NOFOLLOW | CNET_O_CLOEXEC, 0600);
        if (afd < 0) { cap_fail(rep, "asset_write_failed"); goto done; }
        {
            const unsigned char *b = (const unsigned char *)asset;
            size_t off = 0;
            while (off < asset_len) {
                ssize_t w = write(afd, b + off, asset_len - off);
                if (w <= 0) { close(afd); (void)remove(atmp);
                              cap_fail(rep, "asset_write_failed"); goto done; }
                off += (size_t)w;
            }
        }
        if (cnet_fsync(afd) != 0 || close(afd) != 0 || rename(atmp, apath) != 0) {
            (void)remove(atmp);
            cap_fail(rep, "asset_publish_failed");
            goto done;
        }
    }

    /* Exclusive + no-follow: a predictable sibling temp opened with fopen("wb")
       would follow an attacker-planted symlink and truncate the target. This is
       still only ACCIDENT hardening for a local artifact — see the trust
       boundary in the header. */
    {
        int tfd = cnet_mkstemp(tmp_path);
        if (tfd < 0) { cap_fail(rep, "manifest_write_failed"); goto done; }
        fp = fdopen(tfd, "wb");
        if (!fp) { close(tfd); (void)remove(tmp_path);
                   cap_fail(rep, "manifest_write_failed"); goto done; }
    }
    int write_failed = fwrite(man, 1, mlen, fp) != mlen ||
        fprintf(fp, "manifest_fnv %llu\n", cap_fnv_buf(man, mlen)) < 0 ||
        fflush(fp) != 0 || cnet_fsync(fileno(fp)) != 0;
    if (fclose(fp) != 0) write_failed = 1;
    if (write_failed) {
        (void)remove(tmp_path);
        cap_fail(rep, "manifest_write_failed");
        goto done;
    }
    if (rename(tmp_path, man_path) != 0) {
        (void)remove(tmp_path);
        cap_fail(rep, "manifest_publish_failed");
        goto done;
    }
    if (cnb_sync_parent(man_path) || cnb_sync_parent(dir)) {
        cap_fail(rep, "manifest_sync_failed"); goto done;
    }

    if (rep) {
        snprintf(rep->unit, sizeof rep->unit, "%s", unit);
        rep->behavior_digest = digest;
        rep->exemplars = exemplars;
        rep->coverage_rows = hc ? hc->n_rows : 0;
        rep->payload_bytes = bytes;
        rep->schema = asset ? CNET_CAPSULE_SCHEMA_ASSET : CNET_CAPSULE_SCHEMA;
        rep->asset_schema = asset ? asset_schema : 0u;
        rep->asset_bytes = asset ? asset_len : 0u;
        rep->asset_fnv = asset ? cap_fnv_buf((const unsigned char *)asset, asset_len) : 0ull;
        rep->cnb_version = cnb_format_version();
        snprintf(rep->provenance, sizeof rep->provenance, "%s",
                 (prov && prov[0]) ? prov : "");
        snprintf(rep->scope, sizeof rep->scope, "%s", scope);
    }
    rc = 0;
done:
    if (got_unit) { btn_free(&btn); contract_free(&c); }
    free(bytes_buf);
    free(man);
    cnb_free(&sub);
    return rc;
}

int cnet_capsule_export(const CnetBase *src, const HybridAi *cov,
                        const char *unit, const char *dir,
                        CnetCapsuleReport *rep) {
    return cnet_capsule_export_asset(src, cov, unit, dir, NULL, 0, 0u, rep);
}

int cnet_capsule_import_asset(CnetBase *dst, HybridAi *cov, const char *dir,
                              void **asset_out, size_t *asset_len_out,
                              unsigned *asset_schema_out,
                              CnetCapsuleReport *rep) {
    CnetBase sub;
    BinaryTransformNetwork btn;
    Contract c;
    char unit_path[600], man_path[600], tok[64];
    char unit[96] = {0}, itag[PORT_TAG_MAX + 4], gtag[PORT_TAG_MAX + 4];
    char prov[CNB_NAME_MAX] = {0};
    char prov_raw[256] = {0}; /* scratch: %255s matches THIS size, not prov */
    char scope[16] = {0};
    const char *prov_wanted = "";
    size_t oracles_before = 0;
    CnbMark dst_mark;
    unsigned char *pay = NULL, *manbuf = NULL, *abuf = NULL;
    size_t paylen = 0, manlen = 0, alen = 0;
    unsigned a_schema_seen = 0;
    size_t a_bytes_seen = 0;
    unsigned long long a_fnv_seen = 0;
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
    memset(&dst_mark, 0, sizeof dst_mark);
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
        int consumed = 0;
        if (!tail) { cap_fail(rep, "manifest_unchecksummed"); goto done; }
        covered = (size_t)(tail - (const char *)manbuf) + 1;
        if (sscanf(tail + 1, "manifest_fnv %llu%n", &want_man, &consumed) != 1) {
            cap_fail(rep, "manifest_checksum_unreadable");
            goto done;
        }
        /* A valid checksum is the END of the manifest, not a license to
           ignore an appended second document or corrupt trailer. */
        if (memchr(manbuf, 0, manlen)) {
            cap_fail(rep, "manifest_embedded_nul"); goto done;
        }
        for (const char *p = tail + 1 + consumed; p < (const char *)manbuf + manlen; p++) {
            if (!isspace((unsigned char)*p)) {
                cap_fail(rep, "manifest_trailing_data"); goto done;
            }
        }
        if (cap_fnv_buf(manbuf, covered) != want_man) {
            cap_fail(rep, "manifest_integrity_mismatch");
            goto done;
        }
    }
    fp = cnet_fmemopen(manbuf, manlen, "rb");
    if (!fp) { cap_fail(rep, "manifest_unreadable"); goto done; }
    if (fscanf(fp, "%15s %d", tok, &schema) != 2 ||
        strcmp(tok, "CNET_CAPSULE") != 0 ||
        (schema != CNET_CAPSULE_SCHEMA && schema != CNET_CAPSULE_SCHEMA_ASSET)) {
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
        fscanf(fp, " scope %15s", scope) != 1 ||
        fscanf(fp, " coverage %zu %zu %zu", &cov_rows, &cov_in, &cov_out) != 3) {
        cap_fail(rep, "manifest_parse_failed");
        fclose(fp);
        goto done;
    }
    /* An unscoped capsule is exactly the artifact this field exists to refuse:
       there is no default that is safe to assume. */
    if (strcmp(scope, CAP_SCOPE_EXHAUSTIVE) != 0 &&
        strcmp(scope, CAP_SCOPE_SAMPLED) != 0) {
        cap_fail(rep, "unknown_certification_scope");
        fclose(fp);
        goto done;
    }
    if (strcmp(scope, CAP_SCOPE_SAMPLED) == 0 && cov_rows == 0) {
        cap_fail(rep, "sampled_scope_without_coverage");
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
    /* ---- optional sidecar asset ------------------------------------------
       Present iff the manifest declared schema 2. Bound by declared size and
       FNV exactly as the payload is, and refused to a caller that cannot
       receive it rather than dropped. */
    if (schema == CNET_CAPSULE_SCHEMA_ASSET) {
        char aleaf[64] = {0}, apath[600];
        unsigned long long a_fnv = 0;
        size_t a_bytes = 0;
        unsigned a_schema = 0;
        if (fscanf(fp, " asset %u %zu %llu %63s", &a_schema, &a_bytes, &a_fnv, aleaf) != 4) {
            fclose(fp); cap_fail(rep, "asset_manifest_parse_failed"); goto done;
        }
        if (a_bytes == 0 || a_bytes > CNET_CAPSULE_MAX_ASSET ||
            strcmp(aleaf, CNET_CAPSULE_ASSET_FILE) != 0) {
            fclose(fp); cap_fail(rep, "asset_declaration_invalid"); goto done;
        }
        if (!asset_out || !asset_len_out) {
            fclose(fp); cap_fail(rep, "asset_capsule_needs_asset_aware_import"); goto done;
        }
        fclose(fp);
        fp = NULL;
        if (cap_path(apath, sizeof apath, dir, CNET_CAPSULE_ASSET_FILE) != 0) {
            cap_fail(rep, "path_too_long"); goto done;
        }
        if (cap_slurp(apath, &abuf, &alen, &why) != 0) {
            cap_fail(rep, "asset_missing_or_symlink"); goto done;
        }
        if (alen != a_bytes || cap_fnv_buf(abuf, alen) != a_fnv) {
            cap_fail(rep, "asset_integrity_mismatch"); goto done;
        }
        a_schema_seen = a_schema;
        a_bytes_seen = a_bytes;
        a_fnv_seen = a_fnv;
    } else {
        if (asset_out) *asset_out = NULL;
        if (asset_len_out) *asset_len_out = 0;
        fclose(fp);
        fp = NULL;
    }
    if (fp) { fclose(fp); fp = NULL; }

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
    /* Re-derive the scope from the SEALED contract rather than believing the
       manifest. A capsule that claims exhaustive certification must be able to
       prove it here, on the bytes that actually arrived. */
    if (strcmp(scope, CAP_SCOPE_EXHAUSTIVE) == 0 &&
        !cap_scope_exhaustive(&c, NULL)) {
        cap_fail(rep, "exhaustive_scope_claim_unproven");
        goto done;
    }
    if (strcmp(scope, CAP_SCOPE_SAMPLED) == 0 && cap_scope_exhaustive(&c, NULL)) {
        /* Understating scope is not dangerous, but it means the manifest and
           the payload disagree about what this unit is, and the coverage rows
           were bound to a claim that does not hold. Refuse rather than guess. */
        cap_fail(rep, "scope_disagrees_with_payload");
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

    /* ---- target-side preflight: refuse before touching anything ---------
       Duplicate import is REJECTED, not merged. The previous "idempotent"
       path compared behaviour digests, which two units can share while their
       sealed contract or provenance differ, and matched existing coverage by
       unit name plus rows — so a record the same owner held on DIFFERENT ports
       could suppress restoring the gate on the ports that actually matter.
       Both are answered by the same blunt rule: if the target already knows
       this unit name, in the base or in the coverage registry on any ports,
       refuse and mutate nothing. Re-importing is an operator decision, not
       something to infer from a digest. */
    if (cnb_has_unit(dst, unit)) {
        cap_fail(rep, "target_already_has_unit_refusing_duplicate_import");
        goto done;
    }
    if (cov) {
        size_t z;
        for (z = 0; z < cov->coverage_count; z++) {
            if (!cov->coverage[z].active) continue;
            if (strcmp(cov->coverage[z].unit, unit) != 0) continue;
            cap_fail(rep,
                     "target_already_has_coverage_for_unit_refusing_duplicate");
            goto done;
        }
    }

    /* ---- commit: gate FIRST, because only it can be rolled back ----------
       cnb_add_unit has no removal counterpart, so storing coverage first and
       forgetting it on failure is the only ordering where a failed import
       cannot leave an ungated certified unit behind. */
    if (cov_rows) {
        /* Coverage identity is owner + exact interface, so an incoming unit no
           longer displaces an incumbent that happens to share a port shape --
           they coexist, each gated by its own rows. Importing the SAME unit
           twice is still refused, by the duplicate preflight above. */
        if (hybrid_coverage_record(cov, pin, pout, unit, cin, cout, cov_rows,
                                   cov_in, cov_out) != 0) {
            cap_fail(rep, "coverage_restore_failed");
            goto done;
        }
        cov_stored = 1;
    }
    /* ---- lineage ---------------------------------------------------------
       Import VERIFIED the payload's provenance against the manifest and then
       threw it away: cnb_add_unit zero-initialises the new unit ref, while the
       success report went on repeating the manifest's provenance. Callers were
       handed a report claiming lineage the destination did not carry.

       The descriptor the provenance names travels in the payload subset, so it
       can be restored too. Both mutations are undone if the unit admit fails:
       oracle descriptors are appended, so truncating the count is an exact
       rollback (CnbOracleDesc owns no heap). */
    /* Take the mark BEFORE the first destination mutation. Every base mutation
       is an append, so this is an exact inverse, and it covers the descriptor,
       the blob, the unit ref, the minted tags and the mint sequence together --
       the previous ad-hoc `oracle_count` truncation covered only one of them. */
    cnb_mark(dst, &dst_mark);
    prov_wanted = (prov[0] && strcmp(prov, "~")) ? prov : "";
    oracles_before = dst->oracle_count;
    (void)oracles_before;
    if (prov_wanted[0]) {
        size_t d;
        const CnbOracleDesc *from = NULL;
        for (d = 0; d < sub.oracle_count; d++)
            if (strcmp(sub.oracles[d].name, prov_wanted) == 0) {
                from = &sub.oracles[d];
                break;
            }
        if (!from) {
            if (cov_stored) (void)hybrid_coverage_forget_unit(cov, unit);
            (void)cnb_rollback(dst, &dst_mark);
            cap_fail(rep, "provenance_descriptor_absent_from_payload");
            goto done;
        }
        for (d = 0; d < dst->oracle_count; d++)
            if (strcmp(dst->oracles[d].name, prov_wanted) == 0) break;
        if (d == dst->oracle_count) {
            int added = from->behavior_digest || from->identity.abi_version
                            ? cnb_add_oracle_desc_v2(dst, from->name,
                                                     from->kind,
                                                     from->input_port,
                                                     from->goal_port,
                                                     &from->identity)
                            : cnb_add_oracle_desc(dst, from->name, from->kind,
                                                  from->input_port,
                                                  from->goal_port);
            if (added != 0) {
                if (cov_stored) (void)hybrid_coverage_forget_unit(cov, unit);
                (void)cnb_rollback(dst, &dst_mark);
                cap_fail(rep, "provenance_descriptor_restore_failed");
                goto done;
            }
        }
    }
    if (cnb_add_unit(dst, &btn, &c, NULL) != 0) {
        if (cov_stored) (void)hybrid_coverage_forget_unit(cov, unit);
        (void)cnb_rollback(dst, &dst_mark);
        cap_fail(rep, "target_admit_refused");
        goto done;
    }
    /* Fault injection, honoured only when the variable is set. The window
       between admission and provenance restore is unreachable by construction
       (the descriptor was ensured above and the unit was just added with empty
       provenance) -- and "unreachable" is not the same as "recoverable". This
       makes the recovery path executable so a test can prove the destination
       really is byte-identical after a failure there. */
    {
        const char *inject = getenv("CNET_CAPSULE_FAIL_AFTER_ADMIT");
        int forced = inject && inject[0] == '1' && inject[1] == '\0';
        if (forced || (prov_wanted[0] &&
                       cnb_set_unit_provenance(dst, unit, prov_wanted) != 0)) {
            if (cov_stored) (void)hybrid_coverage_forget_unit(cov, unit);
            (void)cnb_rollback(dst, &dst_mark);
            cap_fail(rep, forced ? "injected_failure_after_admit"
                                 : "provenance_restore_failed");
            goto done;
        }
    }
    if (rep) {
        snprintf(rep->unit, sizeof rep->unit, "%s", unit);
        rep->behavior_digest = want_digest;
        rep->exemplars = exemplars;
        rep->coverage_rows = cov_rows;
        rep->payload_bytes = paylen;
        rep->cnb_version = ver;
        snprintf(rep->provenance, sizeof rep->provenance, "%s", prov_wanted);
        snprintf(rep->scope, sizeof rep->scope, "%s", scope);
        rep->schema = (unsigned)schema;
        rep->asset_schema = a_schema_seen;
        rep->asset_bytes = a_bytes_seen;
        rep->asset_fnv = a_fnv_seen;
    }
    /* Hand the verified blob over only once every other check has passed, so a
       refused import never leaves the caller holding a frontend. */
    if (abuf && asset_out && asset_len_out) {
        *asset_out = abuf;
        *asset_len_out = alen;
        abuf = NULL;
        if (asset_schema_out) *asset_schema_out = a_schema_seen;
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
    free(abuf);
    cnb_free(&sub);
    return rc;
}

/* Schema-1 entry point. An asset-bearing capsule is refused here rather than
   imported without its frontend. */
int cnet_capsule_import(CnetBase *dst, HybridAi *cov, const char *dir,
                        CnetCapsuleReport *rep) {
    return cnet_capsule_import_asset(dst, cov, dir, NULL, NULL, NULL, rep);
}
