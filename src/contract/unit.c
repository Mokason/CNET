/* CNET unit file ("CNU1"): weights + contract in one sealed binary artifact.
 * See include/contract/unit.h.
 *
 * Layout (little-endian, single-toolchain project):
 *   "CNU1" | u32 version
 *   u16 name_len | name bytes
 *   u64 input_count, output_count, hidden_count, max_hidden_count
 *   f64 learning_rate | u8 ternary_inference | f64 ternary_threshold
 *   u8 n_in_ports  | per port: u8 family, u64 width, u64 count, u8 taglen, tag
 *   u8 n_out_ports | same
 *   f64 weights: output_bias[out], hidden_bias[hidden],
 *                input_hidden live (hidden-major, input_count stride),
 *                hidden_output live (output-major, max_hidden stride)
 *   u64 exemplar_count
 *   packed exemplars: per exemplar ceil((in_total+out_total)/8) bytes,
 *                     MSB-first (canonical 0/1 values as bits)
 *   u64 SEAL = FNV-1a over every preceding byte
 */

#include "../../include/contract/unit.h"
#include "../../include/plan_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNIT_MAGIC "CNU1"
#define UNIT_VERSION 1u

/* sanity caps: refuse hostile headers before any allocation */
#define UNIT_MAX_DIM       (1u << 20)
#define UNIT_MAX_EXEMPLARS (1u << 24)

/* ---- FNV-1a (same scheme as the contract seal) ---- */

static unsigned long long unit_fnv(const unsigned char *p, size_t n) {
    unsigned long long h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* ---- growable blob writer ---- */

typedef struct { unsigned char *buf; size_t len, cap; } UnitBlob;

static int blob_put(UnitBlob *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        unsigned char *nb;
        while (nc < b->len + n) nc *= 2;
        nb = (unsigned char *)realloc(b->buf, nc);
        if (nb == NULL) return -1;
        b->buf = nb;
        b->cap = nc;
    }
    memcpy(b->buf + b->len, p, n);
    b->len += n;
    return 0;
}

static int blob_u8(UnitBlob *b, unsigned v)  { unsigned char x = (unsigned char)v; return blob_put(b, &x, 1); }
static int blob_u16(UnitBlob *b, unsigned v) { unsigned short x = (unsigned short)v; return blob_put(b, &x, 2); }
static int blob_u32(UnitBlob *b, unsigned v) { unsigned int x = v; return blob_put(b, &x, 4); }
static int blob_u64(UnitBlob *b, unsigned long long v) { return blob_put(b, &v, 8); }
static int blob_f64(UnitBlob *b, double v)   { return blob_put(b, &v, 8); }

/* ---- bounded blob reader ---- */

typedef struct { const unsigned char *buf; size_t len, off; } UnitReader;

static int rd(UnitReader *r, void *out, size_t n) {
    if (r->off + n > r->len) return -1;
    memcpy(out, r->buf + r->off, n);
    r->off += n;
    return 0;
}

static int rd_u8(UnitReader *r, unsigned *v)  { unsigned char x; if (rd(r, &x, 1)) return -1; *v = x; return 0; }
static int rd_u16(UnitReader *r, unsigned *v) { unsigned short x; if (rd(r, &x, 2)) return -1; *v = x; return 0; }
static int rd_u32(UnitReader *r, unsigned *v) { unsigned int x; if (rd(r, &x, 4)) return -1; *v = x; return 0; }
static int rd_u64(UnitReader *r, unsigned long long *v) { return rd(r, v, 8); }
static int rd_f64(UnitReader *r, double *v)   { return rd(r, v, 8); }

/* ---- shared helpers (local copies of contract.c's statics) ---- */

static int unit_name_valid(const char *name) {
    const char *p;
    if (name == NULL || name[0] == '\0' || strlen(name) >= CONTRACT_NAME_MAX) {
        return 0;
    }
    for (p = name; *p != '\0'; ++p) {
        char ch = *p;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '_')) {
            return 0;
        }
    }
    return 1;
}

static int unit_ports_equal(const Port *a, size_t na, const Port *b, size_t nb) {
    size_t i;
    if (na != nb) return 0;
    for (i = 0; i < na; ++i) {
        if (a[i].family != b[i].family ||
            a[i].field_width != b[i].field_width ||
            a[i].field_count != b[i].field_count ||
            strcmp(a[i].tag, b[i].tag) != 0) {
            return 0;
        }
    }
    return 1;
}

static size_t unit_ports_total(const Port *ports, size_t n) {
    size_t total = 0, i;
    for (i = 0; i < n; ++i) {
        size_t pw = ports[i].field_width, pc = ports[i].field_count;
        if (pc != 0 && pw > (size_t)-1 / pc) return (size_t)-1;
        if (total > (size_t)-1 - pw * pc) return (size_t)-1;
        total += pw * pc;
    }
    return total;
}

static int put_ports(UnitBlob *b, const Port *ports, size_t n) {
    size_t i;
    if (blob_u8(b, (unsigned)n)) return -1;
    for (i = 0; i < n; ++i) {
        size_t tl = strlen(ports[i].tag);
        if (blob_u8(b, (unsigned)ports[i].family) ||
            blob_u64(b, ports[i].field_width) ||
            blob_u64(b, ports[i].field_count) ||
            blob_u8(b, (unsigned)tl) ||
            blob_put(b, ports[i].tag, tl)) {
            return -1;
        }
    }
    return 0;
}

static int get_ports(UnitReader *r, Port *ports, size_t max_ports, size_t *n_out) {
    unsigned n, i;
    if (rd_u8(r, &n) || n == 0 || n > max_ports) return -1;
    for (i = 0; i < n; ++i) {
        unsigned fam, tl;
        unsigned long long w, c;
        char tag[PORT_TAG_MAX + 1];
        if (rd_u8(r, &fam) || rd_u64(r, &w) || rd_u64(r, &c) || rd_u8(r, &tl)) {
            return -1;
        }
        if (w == 0 || c == 0 || w > UNIT_MAX_DIM || c > UNIT_MAX_DIM) return -1;
        if (tl > PORT_TAG_MAX) return -1;
        if (rd(r, tag, tl)) return -1;
        tag[tl] = '\0';
        memset(&ports[i], 0, sizeof ports[i]);
        ports[i].family = (PortFamily)fam;
        ports[i].field_width = (size_t)w;
        ports[i].field_count = (size_t)c;
        if (tl > 0 && port_set_tag(&ports[i], tag) != 0) return -1;
    }
    *n_out = (size_t)n;
    return 0;
}

/* ---- unit_save ---- */

int unit_save_mem(const BinaryTransformNetwork *btn, const Contract *c,
                  unsigned char **buf_out, size_t *len_out) {
    UnitBlob b = {0};
    size_t i, j, in_total, out_total, row_bits, row_bytes;
    unsigned long long seal;

    if (btn == NULL || c == NULL || buf_out == NULL || len_out == NULL) return -1;
    if (!unit_name_valid(c->name)) return -1;
    if (c->exemplar_count == 0 || c->exemplar_count > UNIT_MAX_EXEMPLARS) return -1;

    /* coherence: the unit's contract IS the btn's signature */
    if (!unit_ports_equal(btn->input_ports, btn->input_port_count,
                          c->input_ports, c->input_port_count) ||
        !unit_ports_equal(btn->output_ports, btn->output_port_count,
                          c->output_ports, c->output_port_count)) {
        return -1;
    }

    in_total  = unit_ports_total(c->input_ports,  c->input_port_count);
    out_total = unit_ports_total(c->output_ports, c->output_port_count);
    if (in_total == (size_t)-1 || out_total == (size_t)-1 ||
        in_total == 0 || out_total == 0) {
        return -1;
    }

    /* bit-packing is only sound for canonical 0/1 values: verify FIRST */
    for (i = 0; i < c->exemplar_count; ++i) {
        for (j = 0; j < in_total; ++j) {
            double v = c->inputs[i * in_total + j];
            if (v != 0.0 && v != 1.0) return -1;
        }
        for (j = 0; j < out_total; ++j) {
            double v = c->outputs[i * out_total + j];
            if (v != 0.0 && v != 1.0) return -1;
        }
    }

    if (blob_put(&b, UNIT_MAGIC, 4) || blob_u32(&b, UNIT_VERSION)) goto done;
    if (blob_u16(&b, (unsigned)strlen(c->name)) ||
        blob_put(&b, c->name, strlen(c->name))) goto done;

    if (blob_u64(&b, btn->input_count) || blob_u64(&b, btn->output_count) ||
        blob_u64(&b, btn->hidden_count) || blob_u64(&b, btn->max_hidden_count) ||
        blob_f64(&b, btn->learning_rate) ||
        blob_u8(&b, (unsigned)(btn->ternary_inference ? 1 : 0)) ||
        blob_f64(&b, btn->ternary_threshold)) {
        goto done;
    }
    if (put_ports(&b, btn->input_ports, btn->input_port_count) ||
        put_ports(&b, btn->output_ports, btn->output_port_count)) {
        goto done;
    }

    /* live weight slots, btn_save order + stride convention */
    for (i = 0; i < btn->output_count; ++i) {
        if (blob_f64(&b, btn->output_bias[i])) goto done;
    }
    for (i = 0; i < btn->hidden_count; ++i) {
        if (blob_f64(&b, btn->hidden_bias[i])) goto done;
    }
    for (i = 0; i < btn->input_count; ++i) {
        for (j = 0; j < btn->hidden_count; ++j) {
            if (blob_f64(&b, btn->input_hidden[j * btn->input_count + i])) goto done;
        }
    }
    for (i = 0; i < btn->hidden_count; ++i) {
        for (j = 0; j < btn->output_count; ++j) {
            if (blob_f64(&b, btn->hidden_output_weights[j * btn->max_hidden_count + i])) goto done;
        }
    }

    /* bit-packed exemplars: canonical 0/1 -> 1 bit each, MSB-first, each
       exemplar padded to a byte boundary */
    if (blob_u64(&b, c->exemplar_count)) goto done;
    row_bits = in_total + out_total;
    row_bytes = (row_bits + 7) / 8;
    for (i = 0; i < c->exemplar_count; ++i) {
        unsigned char byte = 0;
        size_t bit = 0;
        for (j = 0; j < row_bits; ++j) {
            double v = (j < in_total)
                     ? c->inputs[i * in_total + j]
                     : c->outputs[i * out_total + (j - in_total)];
            byte = (unsigned char)((byte << 1) | (v == 1.0 ? 1 : 0));
            if (++bit == 8) {
                if (blob_u8(&b, byte)) goto done;
                byte = 0;
                bit = 0;
            }
        }
        if (bit != 0) {
            byte = (unsigned char)(byte << (8 - bit));
            if (blob_u8(&b, byte)) goto done;
        }
        (void)row_bytes;
    }

    seal = unit_fnv(b.buf, b.len);
    if (blob_u64(&b, seal)) goto done;

    *buf_out = b.buf;
    *len_out = b.len;
    return 0;

done:
    free(b.buf);
    return -1;
}

int unit_save(const BinaryTransformNetwork *btn, const Contract *c,
              const char *path) {
    unsigned char *buf;
    size_t len;
    FILE *f;
    int ok = -1;

    if (path == NULL) return -1;
    if (unit_save_mem(btn, c, &buf, &len) != 0) return -1;
    f = fopen(path, "wb");
    if (f != NULL) {
        ok = (fwrite(buf, 1, len, f) == len) ? 0 : -1;
        if (fclose(f) != 0) ok = -1;
        if (ok != 0) remove(path);
    }
    free(buf);
    return ok;
}

/* ---- unit_load ---- */

int unit_load_mem(BinaryTransformNetwork *btn, Contract *c,
                  const unsigned char *buf, size_t len) {
    UnitReader r;
    unsigned version, name_len, tern;
    unsigned long long ic, oc, hc, mhc, n_ex, seal_want;
    double lr, tth;
    Contract local;
    size_t i, j, in_total, out_total, row_bits;
    int btn_ready = 0;

    if (btn == NULL || c == NULL || buf == NULL) return -1;
    memset(&local, 0, sizeof local);
    if (len < 4 + 4 + 8) return -1;

    /* seal FIRST: never parse bytes that fail integrity */
    {
        unsigned long long have;
        memcpy(&seal_want, buf + len - 8, 8);
        have = unit_fnv(buf, len - 8);
        if (have != seal_want) return -1;
    }

    r.buf = buf;
    r.len = len - 8; /* payload only */
    r.off = 0;

    if (r.len < 8 || memcmp(buf, UNIT_MAGIC, 4) != 0) goto fail;
    r.off = 4;
    if (rd_u32(&r, &version) || version != UNIT_VERSION) goto fail;

    if (rd_u16(&r, &name_len) || name_len == 0 || name_len >= CONTRACT_NAME_MAX) goto fail;
    if (rd(&r, local.name, name_len)) goto fail;
    local.name[name_len] = '\0';
    if (!unit_name_valid(local.name)) goto fail;

    if (rd_u64(&r, &ic) || rd_u64(&r, &oc) || rd_u64(&r, &hc) || rd_u64(&r, &mhc) ||
        rd_f64(&r, &lr) || rd_u8(&r, &tern) || rd_f64(&r, &tth)) {
        goto fail;
    }
    if (ic == 0 || oc == 0 || hc == 0 || mhc < hc ||
        ic > UNIT_MAX_DIM || oc > UNIT_MAX_DIM || mhc > UNIT_MAX_DIM) {
        goto fail;
    }

    {
        Port in_ports[BTN_MAX_INPUT_PORTS], out_ports[BTN_MAX_OUTPUT_PORTS];
        size_t n_in = 0, n_out = 0;
        if (get_ports(&r, in_ports, BTN_MAX_INPUT_PORTS, &n_in) ||
            get_ports(&r, out_ports, BTN_MAX_OUTPUT_PORTS, &n_out)) {
            goto fail;
        }
        in_total  = unit_ports_total(in_ports, n_in);
        out_total = unit_ports_total(out_ports, n_out);
        if (in_total != (size_t)ic || out_total != (size_t)oc) goto fail;

        if (btn_init(btn, (size_t)ic, (size_t)oc, (size_t)hc, (size_t)mhc,
                     lr, 1u) != 0) {
            goto fail;
        }
        btn_ready = 1;
        btn->ternary_inference = (int)tern;
        btn->ternary_threshold = tth;
        if (btn_set_io_ports(btn, in_ports, n_in, out_ports, n_out) != 0) goto fail;
    }

    for (i = 0; i < btn->output_count; ++i) {
        if (rd_f64(&r, &btn->output_bias[i])) goto fail;
    }
    for (i = 0; i < btn->hidden_count; ++i) {
        if (rd_f64(&r, &btn->hidden_bias[i])) goto fail;
    }
    for (i = 0; i < btn->input_count; ++i) {
        for (j = 0; j < btn->hidden_count; ++j) {
            if (rd_f64(&r, &btn->input_hidden[j * btn->input_count + i])) goto fail;
        }
    }
    for (i = 0; i < btn->hidden_count; ++i) {
        for (j = 0; j < btn->output_count; ++j) {
            if (rd_f64(&r, &btn->hidden_output_weights[j * btn->max_hidden_count + i])) goto fail;
        }
    }

    /* contract: signature copied from the btn (coherent by construction) */
    memcpy(local.input_ports, btn->input_ports,
           btn->input_port_count * sizeof(Port));
    local.input_port_count = btn->input_port_count;
    memcpy(local.output_ports, btn->output_ports,
           btn->output_port_count * sizeof(Port));
    local.output_port_count = btn->output_port_count;

    if (rd_u64(&r, &n_ex) || n_ex == 0 || n_ex > UNIT_MAX_EXEMPLARS) goto fail;
    local.exemplar_count = (size_t)n_ex;
    if (local.exemplar_count > (size_t)-1 / sizeof(double) / in_total) goto fail;
    if (local.exemplar_count > (size_t)-1 / sizeof(double) / out_total) goto fail;
    local.inputs  = (double *)malloc(local.exemplar_count * in_total * sizeof(double));
    local.outputs = (double *)malloc(local.exemplar_count * out_total * sizeof(double));
    if (local.inputs == NULL || local.outputs == NULL) goto fail;

    row_bits = in_total + out_total;
    for (i = 0; i < local.exemplar_count; ++i) {
        unsigned byte = 0;
        size_t bit = 8; /* force a fetch on the first bit */
        for (j = 0; j < row_bits; ++j) {
            double v;
            if (bit == 8) {
                if (rd_u8(&r, &byte)) goto fail;
                bit = 0;
            }
            v = ((byte >> (7 - bit)) & 1u) ? 1.0 : 0.0;
            ++bit;
            if (j < in_total) {
                local.inputs[i * in_total + j] = v;
            } else {
                local.outputs[i * out_total + (j - in_total)] = v;
            }
        }
        /* validate every port slice, exactly like contract_load */
        {
            size_t off = 0;
            for (j = 0; j < local.input_port_count; ++j) {
                size_t tot = plan_port_total(local.input_ports[j]);
                if (!port_validate(local.input_ports[j],
                                   local.inputs + i * in_total + off)) {
                    goto fail;
                }
                off += tot;
            }
            off = 0;
            for (j = 0; j < local.output_port_count; ++j) {
                size_t tot = plan_port_total(local.output_ports[j]);
                if (!port_validate(local.output_ports[j],
                                   local.outputs + i * out_total + off)) {
                    goto fail;
                }
                off += tot;
            }
        }
    }

    if (r.off != r.len) goto fail; /* trailing junk inside the sealed payload */

    local.owns_data = 1;
    local.seal_verified = 1;
    *c = local;
    return 0;

fail:
    if (btn_ready) btn_free(btn);
    free(local.inputs);
    free(local.outputs);
    return -1;
}

int unit_load(BinaryTransformNetwork *btn, Contract *c, const char *path) {
    unsigned char *buf = NULL;
    long fsize;
    FILE *f;
    int rc;

    if (btn == NULL || c == NULL || path == NULL) return -1;
    f = fopen(path, "rb");
    if (f == NULL) return -1;
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 4 + 4 + 8) { fclose(f); return -1; }
    buf = (unsigned char *)malloc((size_t)fsize);
    if (buf == NULL || fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    rc = unit_load_mem(btn, c, buf, (size_t)fsize);
    free(buf);
    return rc;
}
