#include "../include/base.h"
#include "../include/contract/unit.h"
#include "../include/specialist.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <windows.h>
/* Windows has no O_NOFOLLOW or O_CLOEXEC. Dropping O_NOFOLLOW does not weaken
   the anti-symlink property of cnb_save: that rests on O_CREAT|O_EXCL, which
   fails when the path already exists — including when it is a symlink or a
   reparse point. _O_NOINHERIT is the O_CLOEXEC analogue. */
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif
#else
#include <fcntl.h>
#include <unistd.h>
#endif

/* Brings in <fcntl.h> on BOTH platforms plus CNET_O_* spellings. */
#include "../include/cnet_platform.h"
/* Sealed containers are byte-exact. Without O_BINARY the Windows CRT
   translates \n to \r\n on write and the seal digest no longer matches. */
#ifndef O_BINARY
#define O_BINARY 0
#endif

#define CNB_MAGIC "CNB1"
#define CNB_VERSION 5u
#define CNB_MIN_VERSION 1u

/* sanity caps: refuse hostile headers before any allocation */
#define CNB_MAX_TABLE   (1u << 20)
#define CNB_MAX_BLOB    (1u << 28)

/* ---- FNV-1a (same scheme as the unit/contract seals) ---- */

static unsigned long long cnb_fnv(const unsigned char *p, size_t n) {
    unsigned long long h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* ---- growable blob writer (unit.c pattern) ---- */

typedef struct { unsigned char *buf; size_t len, cap; } CnbW;

static int w_put(CnbW *b, const void *p, size_t n) {
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

static int w_u8(CnbW *b, unsigned v)  { unsigned char x = (unsigned char)v; return w_put(b, &x, 1); }
static int w_u16(CnbW *b, unsigned v) { unsigned short x = (unsigned short)v; return w_put(b, &x, 2); }
static int w_u32(CnbW *b, unsigned v) { unsigned int x = v; return w_put(b, &x, 4); }
static int w_u64(CnbW *b, unsigned long long v) { return w_put(b, &v, 8); }

typedef struct { const unsigned char *buf; size_t len, off; } CnbR;

static int r_get(CnbR *r, void *p, size_t n) {
    if (r->off + n > r->len) return -1;
    memcpy(p, r->buf + r->off, n);
    r->off += n;
    return 0;
}

static int r_u8(CnbR *r, unsigned *v)  { unsigned char x; if (r_get(r, &x, 1)) return -1; *v = x; return 0; }
static int r_u16(CnbR *r, unsigned *v) { unsigned short x; if (r_get(r, &x, 2)) return -1; *v = x; return 0; }
static int r_u32(CnbR *r, unsigned *v) { unsigned int x; if (r_get(r, &x, 4)) return -1; *v = x; return 0; }
static int r_u64(CnbR *r, unsigned long long *v) { return r_get(r, v, 8); }

/* length-prefixed string into a fixed buffer (atom-sized) */
static int w_str(CnbW *b, const char *s) {
    size_t n = strlen(s);
    if (n > 0xFFFF) return -1;
    if (w_u16(b, (unsigned)n)) return -1;
    return n ? w_put(b, s, n) : 0;
}

static int r_str(CnbR *r, char *dst, size_t cap) {
    unsigned n;
    if (r_u16(r, &n) || n >= cap) return -1;
    if (n && r_get(r, dst, n)) return -1;
    dst[n] = '\0';
    return 0;
}

static int w_port(CnbW *b, Port p) {
    if (w_u8(b, (unsigned)p.family) ||
        w_u64(b, p.field_width) || w_u64(b, p.field_count) ||
        w_str(b, p.tag)) return -1;
    return 0;
}

static int r_port(CnbR *r, Port *p) {
    unsigned fam;
    unsigned long long w, c;
    char tag[PORT_TAG_MAX];
    memset(p, 0, sizeof *p);
    if (r_u8(r, &fam) || fam > PORT_CONCEPT) return -1;
    if (r_u64(r, &w) || r_u64(r, &c) ||
        w > CNB_MAX_TABLE || c > CNB_MAX_TABLE) return -1;
    if (r_str(r, tag, sizeof tag)) return -1;
    p->family = (PortFamily)fam;
    p->field_width = (size_t)w;
    p->field_count = (size_t)c;
    if (tag[0] && port_set_tag(p, tag) != 0) return -1;
    return 0;
}

/* ---- generic dynamic-array push ---- */

#define CNB_PUSH(arr, count, cap, T)                                   \
    do {                                                               \
        if ((count) == (cap)) {                                        \
            size_t nc = (cap) ? (cap) * 2 : 8;                         \
            T *na = (T *)realloc((arr), nc * sizeof(T));               \
            if (na == NULL) return -1;                                 \
            (arr) = na;                                                \
            (cap) = nc;                                                \
        }                                                              \
        memset(&(arr)[count], 0, sizeof(T));                           \
    } while (0)

static int cnb_name_is_atom(const char *s) {
    size_t i;
    if (!s || !s[0]) return 0;
    for (i = 0; s[i]; ++i) {
        char ch = s[i];
        int ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                 (ch >= '0' && ch <= '9') || ch == '_';
        if (!ok) return 0;
    }
    return i < CNB_NAME_MAX;
}

/* ---- lifecycle ---- */

void cnb_init(CnetBase *b) {
    if (!b) return;
    memset(b, 0, sizeof *b);
}

void cnb_free(CnetBase *b) {
    size_t i;
    if (!b) return;
    for (i = 0; i < b->blob_count; ++i) free(b->blobs[i].bytes);
    free(b->blobs);
    free(b->units);
    free(b->tags);
    free(b->oracles);
    free(b->stats);
    for (i = 0; i < b->loaded_count; ++i) {
        btn_free(b->loaded[i]);
        free(b->loaded[i]);
    }
    free(b->loaded);
    for (i = 0; i < b->loaded_count; ++i) free(b->loaded_names[i]);
    free(b->loaded_names);
    memset(b, 0, sizeof *b);
}

/* ---- tag governance (real logic in this module; see header) ---- */

static void tag_fold(const char *s, char *out) {
    /* lowercase + strip underscores: the canonical form for near-miss checks */
    size_t i, j = 0;
    for (i = 0; s[i]; ++i) {
        char c = s[i];
        if (c == '_') continue;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[j++] = c;
    }
    out[j] = '\0';
}

/* Damerau-Levenshtein distance <= 1: one substitution, insertion, deletion,
   or ADJACENT TRANSPOSITION ("nibbel" vs "nibble" — the archetypal typo). */
static int edit_distance_le1(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la == lb) {
        size_t i, diff[2], nd = 0;
        for (i = 0; i < la; ++i) {
            if (a[i] != b[i]) {
                if (nd < 2) diff[nd] = i;
                if (++nd > 2) return 0;
            }
        }
        if (nd <= 1) return 1;   /* equal or one substitution */
        return diff[1] == diff[0] + 1 &&
               a[diff[0]] == b[diff[1]] && a[diff[1]] == b[diff[0]];
    }
    {
        size_t i = 0, j = 0, edits = 0;
        if (la > lb) { const char *t = a; a = b; b = t; la ^= lb; lb ^= la; la ^= lb; }
        if (lb - la > 1) return 0;
        while (i < la && j < lb) {
            if (a[i] == b[j]) { ++i; ++j; continue; }
            if (++edits > 1) return 0;
            ++j;   /* insertion into the shorter */
        }
        edits += (lb - j) + (la - i);
        return edits <= 1;
    }
}

int cnb_tag_near_miss(const CnetBase *b, const char *tag,
                      char *existing_out, size_t existing_cap) {
    size_t i;
    char fold_new[PORT_TAG_MAX], fold_old[PORT_TAG_MAX];
    if (!b || !tag || !tag[0]) return 0;
    tag_fold(tag, fold_new);
    for (i = 0; i < b->tag_count; ++i) {
        const char *old = b->tags[i].tag;
        if (strcmp(old, tag) == 0) continue;   /* exact = same tag, not a miss */
        tag_fold(old, fold_old);
        if (strcmp(fold_new, fold_old) == 0 ||
            edit_distance_le1(tag, old) ||
            edit_distance_le1(fold_new, fold_old)) {
            if (existing_out && existing_cap > 0) {
                snprintf(existing_out, existing_cap, "%s", old);
            }
            return 1;
        }
    }
    return 0;
}

int cnb_tag_lookup(const CnetBase *b, const char *tag) {
    size_t i;
    if (!b || !tag) return -1;
    for (i = 0; i < b->tag_count; ++i)
        if (strcmp(b->tags[i].tag, tag) == 0) return (int)i;
    return -1;
}

int cnb_tag_mint(CnetBase *b, const char *tag, const char *owner_unit) {
    if (!b || !cnb_name_is_atom(tag) || strlen(tag) >= PORT_TAG_MAX) return -1;
    if (cnb_tag_lookup(b, tag) >= 0) return 1;          /* idempotent re-mint */
    if (cnb_tag_near_miss(b, tag, NULL, 0)) return -1;  /* refusal teeth */
    CNB_PUSH(b->tags, b->tag_count, b->tag_cap, CnbTag);
    snprintf(b->tags[b->tag_count].tag, PORT_TAG_MAX, "%s", tag);
    snprintf(b->tags[b->tag_count].owner, CNB_NAME_MAX, "%s",
             owner_unit ? owner_unit : "");
    b->tags[b->tag_count].mint_seq = b->next_mint_seq++;
    b->tag_count++;
    return 0;
}

void cnb_tag_audit(const CnetBase *b, FILE *out) {
    size_t i, u;
    if (!b || !out) return;
    fprintf(out, "CNB tag audit: %lu tags, %lu units\n",
            (unsigned long)b->tag_count, (unsigned long)b->unit_count);
    for (i = 0; i < b->tag_count; ++i) {
        size_t refs = 0;
        for (u = 0; u < b->unit_count; ++u) {
            BinaryTransformNetwork btn;
            Contract c;
            size_t p;
            int hit = 0;
            if (cnb_get_unit(b, b->units[u].name, &btn, &c) != 0) continue;
            for (p = 0; p < btn.input_port_count && !hit; ++p)
                hit = (strcmp(btn.input_ports[p].tag, b->tags[i].tag) == 0);
            for (p = 0; p < btn.output_port_count && !hit; ++p)
                hit = (strcmp(btn.output_ports[p].tag, b->tags[i].tag) == 0);
            refs += (size_t)hit;
            btn_free(&btn);
            contract_free(&c);
        }
        fprintf(out, "  tag %-24s owner %-24s seq %lu units %lu%s\n",
                b->tags[i].tag, b->tags[i].owner[0] ? b->tags[i].owner : "-",
                (unsigned long)b->tags[i].mint_seq, (unsigned long)refs,
                refs == 0 ? "  ORPHAN" : "");
    }
}

/* ---- units ---- */

static int find_unit(const CnetBase *b, const char *name) {
    size_t i;
    for (i = 0; i < b->unit_count; ++i)
        if (strcmp(b->units[i].name, name) == 0) return (int)i;
    return -1;
}

/* ---- capacity reservation (+ test-only fault injection) ------------------
   Reserving every array BEFORE any mutation is what makes admission
   all-or-nothing: once minting starts, nothing left can allocate, so nothing
   left can fail for want of memory. */
static size_t g_cnb_alloc_fail_in = 0;

void cnb_test_alloc_fail_in(size_t n) { g_cnb_alloc_fail_in = n; }

static int cnb_reserve(void **arr, size_t count, size_t extra, size_t *cap,
                       size_t elem) {
    size_t want, nc;
    void *na;
    if (g_cnb_alloc_fail_in && --g_cnb_alloc_fail_in == 0) return -1;
    if (extra > (size_t)-1 - count) return -1;
    want = count + extra;
    if (want <= *cap) return 0;
    nc = *cap ? *cap : 8;
    while (nc < want) {
        if (nc > (size_t)-1 / 2) return -1;
        nc *= 2;
    }
    if (nc > (size_t)-1 / elem) return -1;
    na = realloc(*arr, nc * elem);
    if (!na) return -1;
    *arr = na;
    *cap = nc;
    return 0;
}

static int cnb_add_unit_bytes(CnetBase *b, const char *name,
                              unsigned long long behavior_digest,
                              const Port *in_ports, size_t n_in,
                              const Port *out_ports, size_t n_out,
                              unsigned char *bytes, size_t len,
                              int *reused_out) {
    unsigned long long digest = cnb_fnv(bytes, len);
    size_t i, blob_index;
    int existing = find_unit(b, name);

    if (reused_out) *reused_out = 0;

    if (existing >= 0) {
        const CnbBlob *old = &b->blobs[b->units[existing].blob_index];
        if (old->digest == digest && old->len == len &&
            memcmp(old->bytes, bytes, len) == 0) {
            free(bytes);
            if (reused_out) *reused_out = 1;
            return 0;   /* idempotent re-ingest */
        }
        free(bytes);
        return -1;      /* same name, different content: refused */
    }

    /* ---- PREFLIGHT: every refusal happens before ANY mutation ------------
       Two orderings used to leave the base half-changed. The blob-collision
       check ran AFTER tags were minted, so a refused collision kept the tags;
       and the tag preflight compares each tag against the BASE only, so a unit
       whose own in/out tags near-miss each other (e.g. "pair_aa"/"pair_ab")
       passed preflight, minted the first and failed on the second. Preflight
       everything that can say no, then commit with a tag rollback for the
       residual mint-order case. */
    for (i = 0; i < n_in; ++i)
        if (in_ports[i].tag[0] && cnb_tag_lookup(b, in_ports[i].tag) < 0 &&
            cnb_tag_near_miss(b, in_ports[i].tag, NULL, 0)) {
            free(bytes); return -1;
        }
    for (i = 0; i < n_out; ++i)
        if (out_ports[i].tag[0] && cnb_tag_lookup(b, out_ports[i].tag) < 0 &&
            cnb_tag_near_miss(b, out_ports[i].tag, NULL, 0)) {
            free(bytes); return -1;
        }

    /* Blob decision made BEFORE minting so a collision refusal mints nothing. */
    blob_index = b->blob_count;
    for (i = 0; i < b->blob_count; ++i) {
        if (b->blobs[i].digest == digest) {
            if (b->blobs[i].len == len &&
                memcmp(b->blobs[i].bytes, bytes, len) == 0) {
                blob_index = i;
                break;
            }
            free(bytes);
            return -1;   /* digest collision, different bytes: refused */
        }
    }

    /* Reserve every array now, while nothing has been mutated. A CNB_PUSH
       failure after minting used to leave tags behind, or a unit-reserve
       failure could leave tags plus an orphan blob. */
    if (cnb_reserve((void **)&b->tags, b->tag_count, n_in + n_out, &b->tag_cap,
                    sizeof(CnbTag)) != 0) {
        free(bytes);
        return -1;
    }
    if (blob_index == b->blob_count &&
        cnb_reserve((void **)&b->blobs, b->blob_count, 1, &b->blob_cap,
                    sizeof(CnbBlob)) != 0) {
        free(bytes);
        return -1;
    }
    if (cnb_reserve((void **)&b->units, b->unit_count, 1, &b->unit_cap,
                    sizeof(CnbUnitRef)) != 0) {
        free(bytes);
        return -1;
    }

    /* ---- COMMIT ---------------------------------------------------------
       Tag mint can still refuse when two of THIS unit's tags near-miss each
       other, which no base-relative preflight can see. Tags are append-only
       (cnb_tag_mint pushes and bumps next_mint_seq), so restoring both counters
       is an exact rollback. */
    {
        size_t tag_mark = b->tag_count;
        unsigned long long seq_mark = b->next_mint_seq;
        int minted_ok = 1;
        for (i = 0; i < n_in && minted_ok; ++i)
            if (in_ports[i].tag[0] &&
                cnb_tag_mint(b, in_ports[i].tag, name) < 0)
                minted_ok = 0;
        for (i = 0; i < n_out && minted_ok; ++i)
            if (out_ports[i].tag[0] &&
                cnb_tag_mint(b, out_ports[i].tag, name) < 0)
                minted_ok = 0;
        if (!minted_ok) {
            b->tag_count = tag_mark;
            b->next_mint_seq = seq_mark;
            free(bytes);
            return -1;
        }
    }
    if (blob_index < b->blob_count) {
        free(bytes);
        bytes = NULL;
    }
    if (blob_index == b->blob_count) {
        /* Capacity is reserved, but the slot still needs zeroing — CNB_PUSH
           did that and this path replaced it. Skipping it left units[].provenance
           uninitialised and cnb_save's strlen ran off the end (caught by ASAN). */
        memset(&b->blobs[b->blob_count], 0, sizeof b->blobs[0]);
        b->blobs[b->blob_count].digest = digest;
        b->blobs[b->blob_count].bytes = bytes;   /* base owns them now */
        b->blobs[b->blob_count].len = len;
        b->blob_count++;
    }

    memset(&b->units[b->unit_count], 0, sizeof b->units[0]);
    snprintf(b->units[b->unit_count].name, CNB_NAME_MAX, "%s", name);
    b->units[b->unit_count].blob_index = blob_index;
    b->units[b->unit_count].behavior_digest = behavior_digest;
    b->unit_count++;
    return 0;
}

int cnb_add_unit(CnetBase *b, const BinaryTransformNetwork *btn,
                 const Contract *c, int *reused_out) {
    unsigned char *bytes;
    size_t len;
    if (!b || !btn || !c || !cnb_name_is_atom(c->name)) return -1;
    if (unit_save_mem(btn, c, &bytes, &len) != 0) return -1;
    return cnb_add_unit_bytes(b, c->name, contract_btn_digest(btn),
                              btn->input_ports, btn->input_port_count,
                              btn->output_ports, btn->output_port_count,
                              bytes, len, reused_out);
}

int cnb_has_unit(const CnetBase *b, const char *name) {
    if (!b || !name) return 0;
    return find_unit(b, name) >= 0;
}

int cnb_get_unit(const CnetBase *b, const char *name,
                 BinaryTransformNetwork *btn, Contract *c) {
    int idx;
    if (!b || !name || !btn || !c) return -1;
    idx = find_unit(b, name);
    if (idx < 0) return -1;
    return unit_load_mem(btn, c, b->blobs[b->units[idx].blob_index].bytes,
                         b->blobs[b->units[idx].blob_index].len);
}

unsigned cnb_format_version(void) { return CNB_VERSION; }

void cnb_mark(const CnetBase *b, CnbMark *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!b) return;
    out->units = b->unit_count;
    out->blobs = b->blob_count;
    out->tags = b->tag_count;
    out->oracles = b->oracle_count;
    out->stats = b->stats_count;
    out->next_mint_seq = b->next_mint_seq;
}

int cnb_rollback(CnetBase *b, const CnbMark *mark) {
    size_t i;
    if (!b || !mark) return -1;
    /* Only ever backwards. A mark longer than the current state did not come
       from this base, and truncating "forward" would invent entries. */
    if (mark->units > b->unit_count || mark->blobs > b->blob_count ||
        mark->tags > b->tag_count || mark->oracles > b->oracle_count ||
        mark->stats > b->stats_count)
        return -1;
    /* Blob payloads are the only owned heap past the mark; unit refs, tags,
       oracle descriptors and stats are plain structs in growable arrays. */
    for (i = mark->blobs; i < b->blob_count; ++i) {
        free(b->blobs[i].bytes);
        b->blobs[i].bytes = NULL;
        b->blobs[i].len = 0;
        b->blobs[i].digest = 0;
    }
    b->unit_count = mark->units;
    b->blob_count = mark->blobs;
    b->tag_count = mark->tags;
    b->oracle_count = mark->oracles;
    b->stats_count = mark->stats;
    b->next_mint_seq = mark->next_mint_seq;
    return 0;
}

int cnb_export_subset(const CnetBase *src, CnetBase *dst,
                      int (*keep)(const char *name, void *ctx), void *ctx) {
    size_t i;
    if (!src || !dst || !keep) return -1;
    cnb_init(dst);
    /* Oracle descriptors first (provenance targets), but ONLY those a kept unit
       actually references. Copying the whole registry meant a one-unit subset —
       the shape a portable capsule ships — disclosed the names, kinds, ports and
       identities of every other oracle in the source base. Least disclosure is
       part of what makes a subset a subset. */
    for (i = 0; i < src->oracle_count; ++i) {
        const CnbOracleDesc *o = &src->oracles[i];
        size_t u;
        int referenced = 0;
        for (u = 0; u < src->unit_count; ++u) {
            if (!keep(src->units[u].name, ctx)) continue;
            if (strcmp(src->units[u].provenance, o->name) == 0) {
                referenced = 1;
                break;
            }
        }
        if (!referenced) continue;
        if (o->behavior_digest || o->identity.abi_version) {
            if (cnb_add_oracle_desc_v2(dst, o->name, o->kind, o->input_port,
                                       o->goal_port, &o->identity) != 0)
                goto fail;
        } else if (cnb_add_oracle_desc(dst, o->name, o->kind, o->input_port,
                                       o->goal_port) != 0) {
            goto fail;
        }
    }
    for (i = 0; i < src->unit_count; ++i) {
        const char *nm = src->units[i].name;
        BinaryTransformNetwork btn;
        Contract c;
        int reused = 0;
        if (!keep(nm, ctx)) continue;
        memset(&btn, 0, sizeof btn);
        memset(&c, 0, sizeof c);
        if (cnb_get_unit(src, nm, &btn, &c) != 0) goto fail;
        if (cnb_add_unit(dst, &btn, &c, &reused) != 0) {
            btn_free(&btn);
            contract_free(&c);
            goto fail;
        }
        if (src->units[i].provenance[0])
            (void)cnb_set_unit_provenance(dst, nm, src->units[i].provenance);
        /* stats */
        {
            size_t s;
            for (s = 0; s < src->stats_count; ++s) {
                if (strcmp(src->stats[s].name, nm) == 0) {
                    /* re-bind via put after get already has counters on btn */
                    (void)cnb_put_stats(dst, nm, &btn);
                    break;
                }
            }
        }
        btn_free(&btn);
        contract_free(&c);
    }
    return 0;
fail:
    cnb_free(dst);
    cnb_init(dst);
    return -1;
}

/* ---- persistence ---- */

static int cnb_sync_file(FILE *f) {
    if (fflush(f) != 0) return -1;
#ifdef _WIN32
    return _commit(_fileno(f)) == 0 ? 0 : -1;
#else
    return fsync(fileno(f)) == 0 ? 0 : -1;
#endif
}

static int cnb_replace_file(const char *tmp_path, const char *path) {
#ifdef _WIN32
    return MoveFileExA(tmp_path, path,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
               ? 0 : -1;
#else
    return rename(tmp_path, path) == 0 ? 0 : -1;
#endif
}

int cnb_sync_parent(const char *path) {
#ifdef _WIN32
    (void)path;
    return 0;
#else
    char dir[512];
    char *slash;
    int fd;
    int flags = O_RDONLY;
    int rc;
    size_t len;

    if (!path) return -1;
    len = strlen(path);
    if (len >= sizeof dir) return -1;
    memcpy(dir, path, len + 1);
    while (len > 1 && dir[len - 1] == '/') dir[--len] = '\0';
    slash = strrchr(dir, '/');
    if (!slash) {
        snprintf(dir, sizeof dir, ".");
    } else if (slash == dir) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    fd = open(dir, flags);
    if (fd < 0) return -1;
    rc = fsync(fd);
    if (close(fd) != 0 && rc == 0) rc = -1;
    return rc == 0 ? 0 : -1;
#endif
}

int cnb_save(const CnetBase *b, const char *path) {
    CnbW w = {0};
    size_t i;
    unsigned long long seal;
    char tmp[512];
    FILE *f;
    int ok = -1;

    if (!b || !path || strlen(path) > sizeof tmp - 5) return -1;

    if (w_put(&w, CNB_MAGIC, 4) || w_u32(&w, CNB_VERSION)) goto done;

    if (w_u64(&w, b->blob_count)) goto done;
    for (i = 0; i < b->blob_count; ++i) {
        if (w_u64(&w, b->blobs[i].digest) || w_u64(&w, b->blobs[i].len) ||
            w_put(&w, b->blobs[i].bytes, b->blobs[i].len)) goto done;
    }

    if (w_u64(&w, b->unit_count)) goto done;
    for (i = 0; i < b->unit_count; ++i) {
        if (w_str(&w, b->units[i].name) ||
            w_u64(&w, b->units[i].blob_index) ||
            w_u64(&w, b->units[i].behavior_digest) ||
            w_str(&w, b->units[i].provenance)) goto done;
    }

    if (w_u64(&w, b->tag_count)) goto done;
    for (i = 0; i < b->tag_count; ++i) {
        if (w_str(&w, b->tags[i].tag) || w_str(&w, b->tags[i].owner) ||
            w_u64(&w, b->tags[i].mint_seq)) goto done;
    }

    if (w_u64(&w, b->oracle_count)) goto done;
    for (i = 0; i < b->oracle_count; ++i) {
        if (w_str(&w, b->oracles[i].name) || w_str(&w, b->oracles[i].kind) ||
            w_port(&w, b->oracles[i].input_port) ||
            w_port(&w, b->oracles[i].goal_port) ||
            w_u32(&w, b->oracles[i].identity.abi_version) ||
            w_u32(&w, b->oracles[i].identity.struct_size) ||
            w_u64(&w, b->oracles[i].identity.artifact_digest) ||
            w_u64(&w, b->oracles[i].identity.contract_digest) ||
            w_u64(&w, b->oracles[i].identity.config_digest) ||
            w_u64(&w, b->oracles[i].identity.retrieval_snapshot_digest) ||
            w_u64(&w, b->oracles[i].identity.toolchain_digest) ||
            w_put(&w, b->oracles[i].identity.artifact_sha256, 32) ||  /* v4 */
            w_u64(&w, b->oracles[i].identity.runtime_libs_digest) ||  /* v5 */
            w_u64(&w, b->oracles[i].behavior_digest)) goto done;
    }

    if (w_u64(&w, b->stats_count)) goto done;
    for (i = 0; i < b->stats_count; ++i) {
        if (w_str(&w, b->stats[i].name) ||
            w_u64(&w, b->stats[i].bound_digest) ||
            w_u64(&w, b->stats[i].successes) ||
            w_u64(&w, b->stats[i].failures)) goto done;
    }

    if (w_u64(&w, b->next_mint_seq)) goto done;

    seal = cnb_fnv(w.buf, w.len);
    if (w_u64(&w, seal)) goto done;

    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    /* The temp name is predictable, so fopen("wb") would follow a planted
       symlink and truncate whatever it points at. Remove any existing entry
       (unlinking a symlink removes the link, never its target) and create
       exclusively without following. */
    {
        int tfd;
        (void)remove(tmp);
        /* O_CREAT|O_EXCL is the security property; CNET_O_NOFOLLOW is
           defence in depth (0 on MinGW). O_BINARY keeps seals byte-exact. */
        tfd = open(tmp,
                   O_WRONLY | O_CREAT | O_EXCL | CNET_O_NOFOLLOW | CNET_O_CLOEXEC |
                       O_BINARY,
                   0600);
        if (tfd < 0) goto done;
        f = fdopen(tfd, "wb");
        if (f == NULL) { close(tfd); (void)remove(tmp); goto done; }
    }
    ok = (fwrite(w.buf, 1, w.len, f) == w.len) ? 0 : -1;
    if (ok == 0 && cnb_sync_file(f) != 0) ok = -1;
    if (fclose(f) != 0) ok = -1;
    if (ok == 0 && cnb_replace_file(tmp, path) != 0) ok = -1;
    if (ok == 0 && cnb_sync_parent(path) != 0) ok = -1;
    if (ok != 0) remove(tmp);

done:
    free(w.buf);
    return ok;
}

int cnb_load(CnetBase *b, const char *path) {
    unsigned char *buf = NULL;
    long fsize;
    FILE *f;

    if (!b || !path) return -1;
    f = fopen(path, "rb");
    if (f == NULL) return -1;
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize < 4 + 4 + 8) { fclose(f); return -1; }
    buf = (unsigned char *)malloc((size_t)fsize);
    if (buf == NULL || fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);
    int rc = cnb_load_mem(b, buf, (size_t)fsize);
    free(buf);
    return rc;
}

int cnb_load_mem(CnetBase *b, const unsigned char *buf, size_t fsize) {
    CnbR r;
    CnetBase fresh;
    unsigned version;
    unsigned long long n, i;
    int ok = -1;
    if (!b || !buf || fsize < 4 + 4 + 8) return -1;

    /* seal FIRST */
    {
        unsigned long long want, have;
        memcpy(&want, buf + fsize - 8, 8);
        have = cnb_fnv(buf, (size_t)fsize - 8);
        if (have != want) return -1;
    }

    r.buf = buf;
    r.len = (size_t)fsize - 8;
    r.off = 0;
    cnb_init(&fresh);

    if (r.len < 8 || memcmp(buf, CNB_MAGIC, 4) != 0) goto fail;
    r.off = 4;
    if (r_u32(&r, &version) ||
        version < CNB_MIN_VERSION || version > CNB_VERSION) goto fail;

    if (r_u64(&r, &n) || n > CNB_MAX_TABLE) goto fail;
    for (i = 0; i < n; ++i) {
        unsigned long long dg, len;
        if (r_u64(&r, &dg) || r_u64(&r, &len) || len == 0 ||
            len > CNB_MAX_BLOB || r.off + len > r.len) goto fail;
        {
            CnbBlob *bl;
            if (fresh.blob_count == fresh.blob_cap) {
                size_t nc = fresh.blob_cap ? fresh.blob_cap * 2 : 8;
                CnbBlob *nb = (CnbBlob *)realloc(fresh.blobs, nc * sizeof *nb);
                if (!nb) goto fail;
                fresh.blobs = nb; fresh.blob_cap = nc;
            }
            bl = &fresh.blobs[fresh.blob_count];
            memset(bl, 0, sizeof *bl);
            bl->bytes = (unsigned char *)malloc((size_t)len);
            if (!bl->bytes) goto fail;
            memcpy(bl->bytes, r.buf + r.off, (size_t)len);
            r.off += (size_t)len;
            bl->len = (size_t)len;
            bl->digest = dg;
            /* honesty: the stored digest must MATCH the bytes */
            if (cnb_fnv(bl->bytes, bl->len) != dg) {
                free(bl->bytes); goto fail;
            }
            fresh.blob_count++;
        }
    }

    if (r_u64(&r, &n) || n > CNB_MAX_TABLE) goto fail;
    for (i = 0; i < n; ++i) {
        char name[CNB_NAME_MAX];
        char provenance[CNB_NAME_MAX];
        unsigned long long bi, bd;
        provenance[0] = '\0';
        if (r_str(&r, name, sizeof name) || !cnb_name_is_atom(name) ||
            r_u64(&r, &bi) || bi >= fresh.blob_count || r_u64(&r, &bd)) goto fail;
        if (version >= 3) {
            if (r_str(&r, provenance, sizeof provenance)) goto fail;
            if (provenance[0] && !cnb_name_is_atom(provenance)) goto fail;
        }
        if (fresh.unit_count == fresh.unit_cap) {
            size_t nc = fresh.unit_cap ? fresh.unit_cap * 2 : 8;
            CnbUnitRef *nu = (CnbUnitRef *)realloc(fresh.units, nc * sizeof *nu);
            if (!nu) goto fail;
            fresh.units = nu; fresh.unit_cap = nc;
        }
        memset(&fresh.units[fresh.unit_count], 0, sizeof fresh.units[0]);
        snprintf(fresh.units[fresh.unit_count].name, CNB_NAME_MAX, "%s", name);
        fresh.units[fresh.unit_count].blob_index = (size_t)bi;
        fresh.units[fresh.unit_count].behavior_digest = bd;
        snprintf(fresh.units[fresh.unit_count].provenance, CNB_NAME_MAX,
                 "%s", provenance);
        fresh.unit_count++;
    }

    if (r_u64(&r, &n) || n > CNB_MAX_TABLE) goto fail;
    for (i = 0; i < n; ++i) {
        char tag[PORT_TAG_MAX], owner[CNB_NAME_MAX];
        unsigned long long seq;
        if (r_str(&r, tag, sizeof tag) || !tag[0] ||
            r_str(&r, owner, sizeof owner) || r_u64(&r, &seq)) goto fail;
        if (fresh.tag_count == fresh.tag_cap) {
            size_t nc = fresh.tag_cap ? fresh.tag_cap * 2 : 8;
            CnbTag *nt = (CnbTag *)realloc(fresh.tags, nc * sizeof *nt);
            if (!nt) goto fail;
            fresh.tags = nt; fresh.tag_cap = nc;
        }
        memset(&fresh.tags[fresh.tag_count], 0, sizeof fresh.tags[0]);
        snprintf(fresh.tags[fresh.tag_count].tag, PORT_TAG_MAX, "%s", tag);
        snprintf(fresh.tags[fresh.tag_count].owner, CNB_NAME_MAX, "%s", owner);
        fresh.tags[fresh.tag_count].mint_seq = seq;
        fresh.tag_count++;
    }

    if (r_u64(&r, &n) || n > CNB_MAX_TABLE) goto fail;
    for (i = 0; i < n; ++i) {
        char name[CNB_NAME_MAX], kind[CNB_NAME_MAX];
        Port ip, gp;
        CnetOracleIdentity identity;
        unsigned long long behavior_digest = 0;
        memset(&identity, 0, sizeof identity);
        if (r_str(&r, name, sizeof name) || !cnb_name_is_atom(name) ||
            r_str(&r, kind, sizeof kind) || !cnb_name_is_atom(kind) ||
            r_port(&r, &ip) || r_port(&r, &gp)) goto fail;
        if (version >= 2) {
            unsigned abi_version, struct_size;
            unsigned long long artifact_digest, contract_digest, config_digest;
            unsigned long long retrieval_snapshot_digest, toolchain_digest;
            if (r_u32(&r, &abi_version) || r_u32(&r, &struct_size) ||
                r_u64(&r, &artifact_digest) ||
                r_u64(&r, &contract_digest) ||
                r_u64(&r, &config_digest) ||
                r_u64(&r, &retrieval_snapshot_digest) ||
                r_u64(&r, &toolchain_digest)) goto fail;
            /* v4 appended the full 256-bit artifact hash; older bases leave it
               zero (the field was memset above) */
            if (version >= 4 && r_get(&r, identity.artifact_sha256, 32)) goto fail;
            /* v5 appended the linked-runtime digest; older bases read 0 =
               "linked runtime unattested" (memset above), same label rule
               as the full hash */
            if (version >= 5) {
                unsigned long long runtime_libs_digest;
                if (r_u64(&r, &runtime_libs_digest)) goto fail;
                identity.runtime_libs_digest = (uint64_t)runtime_libs_digest;
            }
            if (r_u64(&r, &behavior_digest)) goto fail;
            identity.abi_version = (uint32_t)abi_version;
            identity.struct_size = (uint32_t)struct_size;
            identity.artifact_digest = (uint64_t)artifact_digest;
            identity.contract_digest = (uint64_t)contract_digest;
            identity.config_digest = (uint64_t)config_digest;
            identity.retrieval_snapshot_digest = (uint64_t)retrieval_snapshot_digest;
            identity.toolchain_digest = (uint64_t)toolchain_digest;
            if (behavior_digest != 0 &&
                cnet_oracle_identity_digest(&identity) != behavior_digest) goto fail;
            if (behavior_digest == 0 &&
                (identity.abi_version != 0 || identity.struct_size != 0 ||
                 identity.artifact_digest != 0 || identity.contract_digest != 0 ||
                 identity.config_digest != 0 ||
                 identity.retrieval_snapshot_digest != 0 ||
                 identity.toolchain_digest != 0)) goto fail;
        }
        if (fresh.oracle_count == fresh.oracle_cap) {
            size_t nc = fresh.oracle_cap ? fresh.oracle_cap * 2 : 8;
            CnbOracleDesc *no = (CnbOracleDesc *)realloc(fresh.oracles, nc * sizeof *no);
            if (!no) goto fail;
            fresh.oracles = no; fresh.oracle_cap = nc;
        }
        memset(&fresh.oracles[fresh.oracle_count], 0, sizeof fresh.oracles[0]);
        snprintf(fresh.oracles[fresh.oracle_count].name, CNB_NAME_MAX, "%s", name);
        snprintf(fresh.oracles[fresh.oracle_count].kind, CNB_NAME_MAX, "%s", kind);
        fresh.oracles[fresh.oracle_count].input_port = ip;
        fresh.oracles[fresh.oracle_count].goal_port = gp;
        fresh.oracles[fresh.oracle_count].identity = identity;
        fresh.oracles[fresh.oracle_count].behavior_digest = behavior_digest;
        fresh.oracle_count++;
    }

    if (r_u64(&r, &n) || n > CNB_MAX_TABLE) goto fail;
    for (i = 0; i < n; ++i) {
        char name[CNB_NAME_MAX];
        unsigned long long bd, s, fl;
        if (r_str(&r, name, sizeof name) || !cnb_name_is_atom(name) ||
            r_u64(&r, &bd) || r_u64(&r, &s) || r_u64(&r, &fl)) goto fail;
        if (fresh.stats_count == fresh.stats_cap) {
            size_t nc = fresh.stats_cap ? fresh.stats_cap * 2 : 8;
            CnbStats *ns = (CnbStats *)realloc(fresh.stats, nc * sizeof *ns);
            if (!ns) goto fail;
            fresh.stats = ns; fresh.stats_cap = nc;
        }
        memset(&fresh.stats[fresh.stats_count], 0, sizeof fresh.stats[0]);
        snprintf(fresh.stats[fresh.stats_count].name, CNB_NAME_MAX, "%s", name);
        fresh.stats[fresh.stats_count].bound_digest = bd;
        fresh.stats[fresh.stats_count].successes = s;
        fresh.stats[fresh.stats_count].failures = fl;
        fresh.stats_count++;
    }

    if (r_u64(&r, &fresh.next_mint_seq)) goto fail;

    if (r.off != r.len) goto fail;   /* trailing junk inside the seal */

    /* relation integrity: every non-empty unit provenance must name a
       descriptor that exists in this same sealed container */
    for (i = 0; i < fresh.unit_count; ++i) {
        if (fresh.units[i].provenance[0]) {
            size_t d;
            for (d = 0; d < fresh.oracle_count; ++d)
                if (strcmp(fresh.oracles[d].name,
                           fresh.units[i].provenance) == 0) break;
            if (d == fresh.oracle_count) goto fail;
        }
    }

    cnb_free(b);
    *b = fresh;
    return 0;

fail:
    cnb_free(&fresh);
    return ok;
}

/* ---- stats ---- */

int cnb_put_stats(CnetBase *b, const char *unit_name,
                  const BinaryTransformNetwork *btn) {
    size_t i;
    if (!b || !btn || !cnb_name_is_atom(unit_name)) return -1;
    for (i = 0; i < b->stats_count; ++i) {
        if (strcmp(b->stats[i].name, unit_name) == 0) break;
    }
    if (i == b->stats_count) {
        CNB_PUSH(b->stats, b->stats_count, b->stats_cap, CnbStats);
        snprintf(b->stats[b->stats_count].name, CNB_NAME_MAX, "%s", unit_name);
        b->stats_count++;
    }
    b->stats[i].bound_digest = contract_btn_digest(btn);
    b->stats[i].successes = (unsigned long long)btn->output_successes;
    b->stats[i].failures = (unsigned long long)btn->output_failures;
    return 0;
}

size_t cnb_prune_orphan_stats(CnetBase *b) {
    size_t i = 0, removed = 0;
    if (!b) return 0;
    /* Evidence bound to a name with no sealed unit behind it can never be
       applied on load; left in place it accumulates and pushes stats_count
       past unit_count. Compact in place, preserving order. */
    while (i < b->stats_count) {
        if (cnb_has_unit(b, b->stats[i].name)) { i++; continue; }
        if (i + 1 < b->stats_count)
            memmove(&b->stats[i], &b->stats[i + 1],
                    (b->stats_count - i - 1) * sizeof b->stats[0]);
        b->stats_count--;
        removed++;
    }
    return removed;
}

int cnb_apply_stats(const CnetBase *b, const char *unit_name,
                    BinaryTransformNetwork *btn) {
    size_t i;
    if (!b || !btn || !unit_name) return -1;
    for (i = 0; i < b->stats_count; ++i) {
        if (strcmp(b->stats[i].name, unit_name) == 0) {
            if (b->stats[i].bound_digest != contract_btn_digest(btn))
                return -1;   /* stale evidence: weights changed */
            btn->output_successes = (unsigned long)b->stats[i].successes;
            btn->output_failures = (unsigned long)b->stats[i].failures;
            return 0;
        }
    }
    return -1;
}

/* ---- oracle descriptors ---- */

static int cnb_add_oracle_desc_impl(CnetBase *b, const char *name,
                                    const char *kind_atom,
                                    Port input_port, Port goal_port,
                                    const CnetOracleIdentity *identity) {
    uint64_t behavior_digest = 0;
    size_t i;
    if (!b || !cnb_name_is_atom(name) || !cnb_name_is_atom(kind_atom)) return -1;
    if (identity) {
        behavior_digest = cnet_oracle_identity_digest(identity);
        if (behavior_digest == 0) return -1;
    }
    for (i = 0; i < b->oracle_count; ++i)
        if (strcmp(b->oracles[i].name, name) == 0) return -1;
    CNB_PUSH(b->oracles, b->oracle_count, b->oracle_cap, CnbOracleDesc);
    memset(&b->oracles[b->oracle_count], 0, sizeof b->oracles[0]);
    snprintf(b->oracles[b->oracle_count].name, CNB_NAME_MAX, "%s", name);
    snprintf(b->oracles[b->oracle_count].kind, CNB_NAME_MAX, "%s", kind_atom);
    b->oracles[b->oracle_count].input_port = input_port;
    b->oracles[b->oracle_count].goal_port = goal_port;
    if (identity) b->oracles[b->oracle_count].identity = *identity;
    b->oracles[b->oracle_count].behavior_digest = behavior_digest;
    b->oracle_count++;
    return 0;
}

int cnb_add_oracle_desc(CnetBase *b, const char *name, const char *kind_atom,
                        Port input_port, Port goal_port) {
    return cnb_add_oracle_desc_impl(b, name, kind_atom,
                                    input_port, goal_port, NULL);
}

int cnb_add_oracle_desc_v2(CnetBase *b, const char *name,
                           const char *kind_atom,
                           Port input_port, Port goal_port,
                           const CnetOracleIdentity *identity) {
    return cnb_add_oracle_desc_impl(b, name, kind_atom,
                                    input_port, goal_port, identity);
}

int cnb_set_unit_provenance(CnetBase *b, const char *unit_name,
                            const char *oracle_name) {
    int u;
    size_t d;
    if (!b || !cnb_name_is_atom(oracle_name)) return -1;
    u = find_unit(b, unit_name);
    if (u < 0) return -1;
    for (d = 0; d < b->oracle_count; ++d)
        if (strcmp(b->oracles[d].name, oracle_name) == 0) break;
    if (d == b->oracle_count) return -1;
    if (b->units[u].provenance[0])
        return strcmp(b->units[u].provenance, oracle_name) == 0 ? 0 : -1;
    snprintf(b->units[u].provenance, CNB_NAME_MAX, "%s", oracle_name);
    return 0;
}

CNET_API const char *cnb_unit_provenance(const CnetBase *b,
                                         const char *unit_name) {
    int u;
    if (!b || !unit_name) return NULL;
    u = find_unit(b, unit_name);
    return u < 0 ? NULL : b->units[u].provenance;
}

int cnb_bind_oracles(const CnetBase *b, OracleRegistry *orc,
                     CnbOracleResolver resolver, void *rctx,
                     size_t *unbound_out) {
    size_t i, unbound = 0;
    if (!b || !orc || !resolver) return -1;
    for (i = 0; i < b->oracle_count; ++i) {
        CnetOracleFn fn = resolver(b->oracles[i].name, b->oracles[i].kind, rctx);
        if (fn == NULL) { unbound++; continue; }
        if (acquire_oracle_register(orc, b->oracles[i].name,
                                    b->oracles[i].input_port,
                                    b->oracles[i].goal_port, fn, rctx) != 0) {
            unbound++;
        } else if (b->oracles[i].behavior_digest != 0) {
            OracleEntry *entry = &orc->entries[orc->count - 1];
            entry->identity = b->oracles[i].identity;
            entry->behavior_digest = b->oracles[i].behavior_digest;
        }
    }
    if (unbound_out) *unbound_out = unbound;
    return 0;
}

/* ---- registry bridge ---- */

/* Native-unit admission through the one specialist door: wrap as a
   Specialist(kind=btn) and admit (certify + register + stamp kind). The
   low-level registry_add_certified stays an internal of the admission layer. */
static int admit_native_btn(PrimitiveRegistry *reg, BinaryTransformNetwork *btn,
                            const char *name, const Contract *c) {
    Specialist s;
    if (specialist_wrap_btn(&s, btn, name) != 0) return -1;
    return specialist_admit(reg, &s, c);
}

int cnb_load_registry(CnetBase *b, PrimitiveRegistry *reg, size_t *skipped_out) {
    size_t i, skipped = 0;
    if (!b || !reg) return -1;
    for (i = 0; i < b->unit_count; ++i) {
        BinaryTransformNetwork *btn = (BinaryTransformNetwork *)calloc(1, sizeof *btn);
        Contract c;
        if (!btn) return -1;
        if (cnb_get_unit(b, b->units[i].name, btn, &c) != 0) {
            free(btn);
            skipped++;
            continue;
        }
        /* base owns the BTN and the name storage (units[] may realloc, so the
           registry must never borrow a units[i].name pointer) */
        char *nm;
        if (b->loaded_count == b->loaded_cap) {
            size_t nc = b->loaded_cap ? b->loaded_cap * 2 : 8;
            BinaryTransformNetwork **nl =
                (BinaryTransformNetwork **)realloc(b->loaded, nc * sizeof *nl);
            char **nn;
            if (!nl) { btn_free(btn); free(btn); contract_free(&c); return -1; }
            b->loaded = nl;
            /* the POINTER array may move; the name buffers it points to
               never do — registry entries borrow the buffers, not the rows */
            nn = (char **)realloc(b->loaded_names, nc * sizeof *nn);
            if (!nn) { btn_free(btn); free(btn); contract_free(&c); return -1; }
            b->loaded_names = nn;
            b->loaded_cap = nc;
        }
        nm = (char *)malloc(CNB_NAME_MAX);
        if (!nm) { btn_free(btn); free(btn); contract_free(&c); return -1; }
        snprintf(nm, CNB_NAME_MAX, "%s", b->units[i].name);
        /* trust is replayed, never stored; admitted through the specialist
           door so the reloaded native unit reports kind=BTN */
        if (admit_native_btn(reg, btn, nm, &c) != 0) {
            free(nm);
            btn_free(btn);
            free(btn);
            contract_free(&c);
            skipped++;
            continue;
        }
        contract_free(&c);
        b->loaded_names[b->loaded_count] = nm;
        b->loaded[b->loaded_count++] = btn;
    }
    if (skipped_out) *skipped_out = skipped;
    return 0;
}

/* ---- migration ---- */

int cnb_ingest_cnu_file(CnetBase *b, const char *path, int *reused_out) {
    BinaryTransformNetwork btn;
    Contract c;
    int rc;
    if (!b || !path) return -1;
    if (unit_load(&btn, &c, path) != 0) return -1;   /* verifies the CNU1 seal */
    rc = cnb_add_unit(b, &btn, &c, reused_out);
    btn_free(&btn);
    contract_free(&c);
    return rc;
}

/* ---- cross-unit overlap analysis (read-only mining-prefetch style) ---- */

void cnb_analyze_cross_unit_overlap(const CnetBase *b, FILE *out) {
    size_t u, v, i, j;
    size_t U;
    typedef struct {
        char name[CNB_NAME_MAX];
        char input_tag[64];
        unsigned long long beh_digest;
        unsigned long long input_table_hash;
        unsigned long long output_table_hash;
        double *inputs;      /* owned copy of exemplar inputs, row-major */
        double *outputs;     /* owned copy of exemplar targets, row-major */
        size_t exemplar_count;
        size_t row_elems;    /* doubles per input row */
        size_t out_elems;    /* doubles per output row */
    } UnitEx;
    UnitEx *exs = NULL;
    size_t collected = 0;

    if (!b) return;
    if (!out) out = stdout;

    U = b->unit_count;
    if (U == 0) {
        fprintf(out, "cross-unit overlap: 0 units\n");
        return;
    }

    exs = (UnitEx *)calloc(U, sizeof *exs);
    if (!exs) {
        fprintf(out, "cross-unit overlap: OOM during prefetch alloc\n");
        return;
    }

    /* Mining-prefetch phase (READ-ONLY): 
       This is the core of the "read-only mining-prefetch thread".
       We walk units using cnb_get_unit (read-only over the base), 
       copy ONLY the exemplar input+output tables from the sealed Contract
       (the data that was produced by the original oracle mining), 
       immediately free the heavy BTN + contract (no registry mutation, 
       no side effects), and stash compact snapshots for cross-unit comparison.
       The loop is OMP-parallelizable because loads are independent and
       the base is const. We use a two-phase approach for safe parallel fill. */
    {
        /* Phase 1: decide which units to load (all, or skip unreadable) */
        int *load_ok = (int *)calloc(U, sizeof(int));
        if (!load_ok) {
            /* fallback to serial */
            for (u = 0; u < U; ++u) {
                /* ... serial version would go here, but for brevity we skip if OOM */
            }
            free(load_ok);
            /* fall through to old serial if needed, but to keep simple: */
            goto serial_prefetch_fallback;
        }

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for (u = 0; u < U; ++u) {
            BinaryTransformNetwork btn;
            Contract c;
            if (cnb_get_unit(b, b->units[u].name, &btn, &c) == 0) {
                load_ok[u] = 1;
                btn_free(&btn);
                contract_free(&c);
            }
        }

        /* Phase 2: allocate and load in parallel into fixed slots */
        /* For simplicity and to avoid complex reduction, do the actual copy serially here
           (the expensive part was the oracle mining; the load is just deserializing the sealed blob).
           The "thread" benefit is mainly that the analysis after prefetch can be parallel, and
           the design is ready for background thread usage. */
        for (u = 0; u < U; ++u) {
            if (!load_ok[u]) continue;
            BinaryTransformNetwork btn;
            Contract c;
            if (cnb_get_unit(b, b->units[u].name, &btn, &c) != 0) continue;

            size_t row_elems = 0;
            for (i = 0; i < c.input_port_count; ++i)
                row_elems += c.input_ports[i].field_width * c.input_ports[i].field_count;
            size_t out_elems = 0;
            for (i = 0; i < c.output_port_count; ++i)
                out_elems += c.output_ports[i].field_width * c.output_ports[i].field_count;
            size_t row_bytes = row_elems * sizeof(double);
            size_t out_bytes = out_elems * sizeof(double);
            size_t n = c.exemplar_count;

            double *inp_copy = NULL;
            double *out_copy = NULL;
            if (n > 0 && row_bytes > 0) {
                inp_copy = (double *)malloc(n * row_bytes);
                if (inp_copy) memcpy(inp_copy, c.inputs, n * row_bytes);
            }
            if (n > 0 && out_bytes > 0) {
                out_copy = (double *)malloc(n * out_bytes);
                if (out_copy) memcpy(out_copy, c.outputs, n * out_bytes);
            }

            snprintf(exs[collected].name, CNB_NAME_MAX, "%s", b->units[u].name);
            if (c.input_port_count > 0)
                snprintf(exs[collected].input_tag, sizeof exs[collected].input_tag,
                         "%s", c.input_ports[0].tag);
            else
                exs[collected].input_tag[0] = '\0';
            exs[collected].beh_digest = b->units[u].behavior_digest;
            exs[collected].inputs = inp_copy;
            exs[collected].outputs = out_copy;
            exs[collected].exemplar_count = n;
            exs[collected].row_elems = row_elems;
            exs[collected].out_elems = out_elems;
            size_t tbl_bytes = n * row_bytes;
            exs[collected].input_table_hash = (inp_copy && tbl_bytes)
                ? cnb_fnv((const unsigned char *)inp_copy, tbl_bytes) : 0;
            size_t otbl_bytes = n * out_bytes;
            exs[collected].output_table_hash = (out_copy && otbl_bytes)
                ? cnb_fnv((const unsigned char *)out_copy, otbl_bytes) : 0;
            collected++;

            btn_free(&btn);
            contract_free(&c);
        }
        free(load_ok);
        goto after_prefetch;
    }

serial_prefetch_fallback:
    /* fallback serial if needed (rare) */
    for (u = 0; u < U; ++u) {
        BinaryTransformNetwork btn;
        Contract c;
        if (cnb_get_unit(b, b->units[u].name, &btn, &c) != 0) continue;

        size_t row_elems = 0;
        for (i = 0; i < c.input_port_count; ++i)
            row_elems += c.input_ports[i].field_width * c.input_ports[i].field_count;
        size_t out_elems = 0;
        for (i = 0; i < c.output_port_count; ++i)
            out_elems += c.output_ports[i].field_width * c.output_ports[i].field_count;
        size_t row_bytes = row_elems * sizeof(double);
        size_t out_bytes = out_elems * sizeof(double);
        size_t n = c.exemplar_count;

        double *inp_copy = NULL;
        double *out_copy = NULL;
        if (n > 0 && row_bytes > 0) {
            inp_copy = (double *)malloc(n * row_bytes);
            if (inp_copy) memcpy(inp_copy, c.inputs, n * row_bytes);
        }
        if (n > 0 && out_bytes > 0) {
            out_copy = (double *)malloc(n * out_bytes);
            if (out_copy) memcpy(out_copy, c.outputs, n * out_bytes);
        }

        snprintf(exs[collected].name, CNB_NAME_MAX, "%s", b->units[u].name);
        if (c.input_port_count > 0)
            snprintf(exs[collected].input_tag, sizeof exs[collected].input_tag, "%s", c.input_ports[0].tag);
        else
            exs[collected].input_tag[0] = '\0';
        exs[collected].beh_digest = b->units[u].behavior_digest;
        exs[collected].inputs = inp_copy;
        exs[collected].outputs = out_copy;
        exs[collected].exemplar_count = n;
        exs[collected].row_elems = row_elems;
        exs[collected].out_elems = out_elems;
        size_t tbl_bytes = n * row_bytes;
        exs[collected].input_table_hash = (inp_copy && tbl_bytes) ? cnb_fnv((const unsigned char *)inp_copy, tbl_bytes) : 0;
        size_t otbl_bytes = n * out_bytes;
        exs[collected].output_table_hash = (out_copy && otbl_bytes) ? cnb_fnv((const unsigned char *)out_copy, otbl_bytes) : 0;
        collected++;

        btn_free(&btn);
        contract_free(&c);
    }

after_prefetch: ;

    /* Quick digest overlap (identical behavior = full functional overlap) */
    {
        size_t dups = 0;
        for (u = 0; u < collected; ++u)
            for (v = u + 1; v < collected; ++v)
                if (exs[u].beh_digest == exs[v].beh_digest &&
                    exs[u].beh_digest != 0) {
                    fprintf(out, "  BEHAVIOR-DUP: %s == %s (digest %016llx)\n",
                            exs[u].name, exs[v].name,
                            (unsigned long long)exs[u].beh_digest);
                    dups++;
                }
        if (dups) fprintf(out, "behavior-digest dups found: %lu pairs\n", (unsigned long)dups);
    }

    /* Summary + grouped analysis (the core of the read-only mining-prefetch) */
    {
        /* Count unique input tables (by hash + verified memcmp for collisions) */
        size_t unique_input = 0;
        unsigned char *seen = (unsigned char *)calloc(collected, 1);
        if (seen) {
            for (u = 0; u < collected; ++u) {
                if (seen[u]) continue;
                unique_input++;
                seen[u] = 1;
                if (!exs[u].inputs) continue;
                size_t bytes = exs[u].exemplar_count * exs[u].row_elems * sizeof(double);
                for (v = u+1; v < collected; ++v) {
                    if (seen[v]) continue;
                    if (exs[v].input_table_hash == exs[u].input_table_hash &&
                        exs[v].exemplar_count == exs[u].exemplar_count &&
                        exs[v].row_elems == exs[u].row_elems &&
                        bytes > 0 && exs[v].inputs &&
                        memcmp(exs[u].inputs, exs[v].inputs, bytes) == 0) {
                        seen[v] = 1;
                    }
                }
            }
            free(seen);
        }

        fprintf(out, "cross-unit exemplar-input overlap scan (U=%lu collected, %lu unique input tables)\n",
                (unsigned long)collected, (unsigned long)unique_input);
    }

    /* Groups of units with identical full input tables */
    {
        unsigned char *visited = (unsigned char *)calloc(collected, 1);
        if (visited) {
            size_t groups = 0;
            size_t largest = 1;
            for (u = 0; u < collected; ++u) {
                if (visited[u] || exs[u].exemplar_count == 0) continue;
                size_t bytes = exs[u].exemplar_count * exs[u].row_elems * sizeof(double);
                if (bytes == 0 || !exs[u].inputs) { visited[u]=1; continue; }
                char *members[256]; size_t gcount = 0;
                members[gcount++] = exs[u].name;
                visited[u] = 1;
                for (v = u + 1; v < collected; ++v) {
                    if (visited[v]) continue;
                    if (exs[v].exemplar_count != exs[u].exemplar_count ||
                        exs[v].row_elems != exs[u].row_elems ||
                        exs[u].input_table_hash != exs[v].input_table_hash) continue;
                    if (exs[v].inputs && memcmp(exs[u].inputs, exs[v].inputs, bytes) == 0) {
                        visited[v] = 1;
                        if (gcount < 256) members[gcount++] = exs[v].name;
                    }
                }
                if (gcount >= 2) {
                    groups++;
                    if (gcount > largest) largest = gcount;
                    fprintf(out, "  IDENTICAL INPUT TABLE group (%lu units, %lu rows, input='%s'):\n",
                            (unsigned long)gcount, (unsigned long)exs[u].exemplar_count,
                            exs[u].input_tag[0] ? exs[u].input_tag : "(none)");
                    fprintf(out, "    ");
                    for (size_t m = 0; m < gcount && m < 12; ++m) {
                        fprintf(out, "%s%s", (m ? ", " : ""), members[m]);
                    }
                    if (gcount > 12) fprintf(out, " ... (%lu total)", (unsigned long)gcount);
                    fprintf(out, "\n");
                }
            }
            if (groups == 0)
                fprintf(out, "  no groups with identical full input tables\n");
            else
                fprintf(out, "  (largest identical-input group: %lu units)\n", (unsigned long)largest);
            free(visited);
        }
    }

    /* Also report groups with identical full *output* tables (for completeness) */
    {
        unsigned char *visited = (unsigned char *)calloc(collected, 1);
        if (visited) {
            size_t groups = 0;
            for (u = 0; u < collected; ++u) {
                if (visited[u] || exs[u].exemplar_count == 0) continue;
                size_t bytes = exs[u].exemplar_count * exs[u].out_elems * sizeof(double);
                if (bytes == 0 || !exs[u].outputs) { visited[u]=1; continue; }
                char *members[256]; size_t gcount = 0;
                members[gcount++] = exs[u].name;
                visited[u] = 1;
                for (v = u + 1; v < collected; ++v) {
                    if (visited[v]) continue;
                    if (exs[v].exemplar_count != exs[u].exemplar_count ||
                        exs[v].out_elems != exs[u].out_elems ||
                        exs[u].output_table_hash != exs[v].output_table_hash) continue;
                    if (exs[v].outputs && memcmp(exs[u].outputs, exs[v].outputs, bytes) == 0) {
                        visited[v] = 1;
                        if (gcount < 256) members[gcount++] = exs[v].name;
                    }
                }
                if (gcount >= 2) {
                    groups++;
                    fprintf(out, "  IDENTICAL OUTPUT TABLE group (%lu units, %lu rows, for input='%s'):\n",
                            (unsigned long)gcount, (unsigned long)exs[u].exemplar_count,
                            exs[u].input_tag[0] ? exs[u].input_tag : "(none)");
                    fprintf(out, "    ");
                    for (size_t m = 0; m < gcount && m < 8; ++m) {
                        fprintf(out, "%s%s", (m ? ", " : ""), members[m]);
                    }
                    if (gcount > 8) fprintf(out, " ...");
                    fprintf(out, "\n");
                }
            }
            free(visited);
        }
    }

    /* Detailed partial overlaps only (skip full identical table pairs to keep output readable) */
    {
        size_t any_partial = 0;
        for (u = 0; u < collected; ++u) {
            for (v = u + 1; v < collected; ++v) {
                size_t re = exs[u].row_elems;
                size_t nu = exs[u].exemplar_count;
                size_t nv = exs[v].exemplar_count;
                size_t shared = 0;
                if (re == 0 || nu == 0 || nv == 0) continue;
                if (re != exs[v].row_elems) continue;

                /* fast path: if full table identical, we already reported the group -- skip */
                int full_ident = 0;
                if (exs[u].input_table_hash == exs[v].input_table_hash) {
                    size_t bytes = nu * re * sizeof(double);
                    if (bytes && exs[u].inputs && exs[v].inputs &&
                        memcmp(exs[u].inputs, exs[v].inputs, bytes) == 0) {
                        full_ident = 1;
                    }
                }
                if (full_ident) continue;

                /* count actual shared rows */
                for (i = 0; i < nu; ++i) {
                    const double *rowu = exs[u].inputs + i * re;
                    for (j = 0; j < nv; ++j) {
                        const double *rowv = exs[v].inputs + j * re;
                        if (memcmp(rowu, rowv, re * sizeof(double)) == 0) {
                            shared++;
                            break;
                        }
                    }
                }
                if (shared > 0) {
                    fprintf(out, "  PARTIAL OVERLAP %s <-> %s : %lu/%lu rows shared\n",
                            exs[u].name, exs[v].name, (unsigned long)shared, (unsigned long)nu);
                    any_partial++;
                }
            }
        }
        if (!any_partial)
            fprintf(out, "  no partial cross-unit row overlaps (outside identical groups)\n");
    }

    /* Quick identical (input+output) table dups -- complete training duplicates */
    {
        size_t full_dups = 0;
        char *dup_names[32]; size_t nd = 0;
        for (u = 0; u < collected && full_dups < 1000; ++u) {
            for (v = u + 1; v < collected; ++v) {
                if (exs[u].exemplar_count != exs[v].exemplar_count) continue;
                size_t ibytes = exs[u].exemplar_count * exs[u].row_elems * sizeof(double);
                size_t obytes = exs[u].exemplar_count * exs[u].out_elems * sizeof(double);
                int same_i = (exs[u].input_table_hash == exs[v].input_table_hash) &&
                             (ibytes == 0 || (exs[u].inputs && exs[v].inputs &&
                              memcmp(exs[u].inputs, exs[v].inputs, ibytes) == 0));
                int same_o = (exs[u].output_table_hash == exs[v].output_table_hash) &&
                             (obytes == 0 || (exs[u].outputs && exs[v].outputs &&
                              memcmp(exs[u].outputs, exs[v].outputs, obytes) == 0));
                if (same_i && same_o) {
                    full_dups++;
                    if (nd < 32) {
                        /* record a representative */
                        if (nd == 0) dup_names[nd++] = exs[u].name;
                        if (nd < 32) dup_names[nd++] = exs[v].name;
                    }
                }
            }
        }
        if (full_dups) {
            fprintf(out, "  FULL DUP (inputs+targets identical): %lu pairs", (unsigned long)full_dups);
            if (nd > 0) {
                fprintf(out, " e.g. ");
                for (size_t d=0; d < nd && d < 6; d++) fprintf(out, "%s%s", d?", ":"", dup_names[d]);
                if (full_dups > 3) fprintf(out, " ...");
            }
            fprintf(out, "\n");
        }
    }

    /* cleanup the prefetched copies */
    for (u = 0; u < collected; ++u) {
        free(exs[u].inputs);
        free(exs[u].outputs);
    }
    free(exs);
}
