#include "cnet_capsule_core.h"
#include "cnet_capsule.h"
#include "cnet_capsule_evidence.h"
#include "cnet_core_selector.h"
#include "cce/cce_campaign_provenance.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CORE_STATES 1024u
#define CORE_CAPSULES 4096u
#define CORE_COVERAGE_BANKS ((CORE_CAPSULES + HYBRID_COVERAGE_MAX - 1) / HYBRID_COVERAGE_MAX)
typedef struct { size_t left; struct timespec start; double seconds; } CoreBudget;
static void budget_init(CoreBudget *b, size_t work) {
    b->left = work; b->seconds = 2; clock_gettime(CLOCK_MONOTONIC, &b->start);
}
/* Inventory-wide checks scale with admitted count, not kernel complexity.
 * Small/dense inputs retain the old cap; per-query work remains 65536. */
static size_t admission_work(size_t count) {
    size_t banks = (count + 255u) / 256u;
    if (!banks) banks = 1;
    if (banks > CORE_CAPSULES / 256u) banks = CORE_CAPSULES / 256u;
    return banks * 2000000u;
}
static int charge(CoreBudget *b) {
    if (!b->left) return -1;
    if ((--b->left & 255u) == 0) {
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - b->start.tv_sec) * 1e9 + now.tv_nsec - b->start.tv_nsec > b->seconds * 1e9) return -1;
    }
    return 0;
}
typedef struct {
    Port port;
    double value[64];
    size_t depth, parent, edge;
} CoreState;
typedef struct { const HybridCoverage *record; HybridAi *bank; } CoreCoverageRef;
typedef struct {char unit[64];CnetCapsuleEvidence *evidence;} CoreEvidenceRef;

struct CnetCapsuleCore {
    CnetBase base;
    /* Private banks preserve the public HybridAi ABI and legacy trace bound.
     * A capsule imports one unit and at most one sampled coverage record. */
    HybridAi *coverage[CORE_COVERAGE_BANKS];
    PrimitiveRegistry registry;
    size_t edge_order[CORE_CAPSULES], indexed_count;
    Port edge_keys[CORE_CAPSULES];
    int index_ready;
    CoreCoverageRef guards[CORE_CAPSULES];
    size_t guard_count;
    int guards_ready;
    CoreEvidenceRef evidence[CORE_CAPSULES];
    size_t evidence_count;
    /* The original CNB carries each capsule's own provenance descriptor.
     * A merged base can reuse a descriptor name with different source ports;
     * that order-dependent registry metadata is not capsule identity. */
    char payload_identity[CORE_CAPSULES][65];
};

static CnetCapsuleEvidence *find_evidence(const CnetCapsuleCore *c,const char *unit) {
    size_t low=0,high=c->evidence_count;
    while(low<high){size_t mid=low+(high-low)/2;
        if(strcmp(c->evidence[mid].unit,unit)<0)low=mid+1;else high=mid;}
    return low<c->evidence_count&&!strcmp(c->evidence[low].unit,unit)?c->evidence[low].evidence:NULL;
}

static int guard_order(const void *aa, const void *bb) {
    const CoreCoverageRef *a = aa, *b = bb;
    return strcmp(a->record->unit, b->record->unit);
}
static int build_guard_index(CnetCapsuleCore *c) {
    c->guards_ready = 0; c->guard_count = 0;
    for (size_t bank=0; bank<CORE_COVERAGE_BANKS; bank++) {
        HybridAi *h = c->coverage[bank];
        if (!h) continue;
        for (size_t j=0; j<h->coverage_count; j++) if (h->coverage[j].active) {
            if (c->guard_count == CORE_CAPSULES) return -1;
            c->guards[c->guard_count++] = (CoreCoverageRef){&h->coverage[j], h};
        }
    }
    qsort(c->guards, c->guard_count, sizeof c->guards[0], guard_order);
    for (size_t i=1; i<c->guard_count; i++)
        if (!strcmp(c->guards[i-1].record->unit, c->guards[i].record->unit)) return -1;
    c->guards_ready = 1; return 0;
}
static const CoreCoverageRef *find_guard(const CnetCapsuleCore *c, const char *unit) {
    size_t lo=0, hi=c->guard_count;
    while (lo<hi) {
        size_t mid=lo+(hi-lo)/2;
        int cmp=strcmp(c->guards[mid].record->unit, unit);
        if (cmp<0) lo=mid+1; else hi=mid;
    }
    return lo<c->guard_count && !strcmp(c->guards[lo].record->unit,unit) ? &c->guards[lo] : NULL;
}

static int port_order(Port a, Port b) {
    int tag = strcmp(a.tag, b.tag);
    if (tag) return tag;
    if (a.family != b.family) return (a.family > b.family) - (a.family < b.family);
    if (a.field_width != b.field_width) return (a.field_width > b.field_width) - (a.field_width < b.field_width);
    return (a.field_count > b.field_count) - (a.field_count < b.field_count);
}
static int build_edge_index(CnetCapsuleCore *c) {
    c->index_ready = 0;
    if (c->registry.count > CORE_CAPSULES) return -1;
    for (size_t i = 0; i < c->registry.count; i++) {
        BinaryTransformNetwork *btn = c->registry.entries[i].btn;
        if (!btn || btn->input_port_count != 1) return -1;
        c->edge_keys[i] = btn->input_ports[0];
        size_t j = i;
        /* Stable within a signature: preserve original route tie order. */
        while (j && port_order(c->edge_keys[c->edge_order[j-1]], c->edge_keys[i]) > 0) {
            c->edge_order[j] = c->edge_order[j-1]; j--;
        }
        c->edge_order[j] = i;
    }
    c->indexed_count = c->registry.count; c->index_ready = 1;
    return 0;
}
static int edge_range(CnetCapsuleCore *c, Port port, size_t *begin, size_t *end,
                      CoreBudget *budget, size_t *local_work) {
    if (!c->index_ready || c->indexed_count != c->registry.count) return -1;
    size_t low = 0, high = c->indexed_count;
    while (low < high) {
        if ((local_work && !(*local_work)--) || charge(budget)) return -1;
        size_t mid = low + (high-low)/2;
        int order = port_order(c->edge_keys[c->edge_order[mid]], port);
        if (order < 0) low = mid+1; else high = mid;
    }
    *begin = low;
    /* Walk only this signature's run, charging every comparison. Sparse
     * inventories avoid a second full binary lookup; dense runs remain bounded. */
    while (low < c->indexed_count) {
        if ((local_work && !(*local_work)--) || charge(budget)) return -1;
        if (port_order(c->edge_keys[c->edge_order[low]], port)) break;
        low++;
    }
    *end = low;
    return 0;
}

static int original_payload_identity(const char *path,char hash[65]) {
    int parent=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(parent<0)return -1;
    int fd=openat(parent,"unit.cnb",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    close(parent);if(fd<0)return -1;
    struct stat before,after;unsigned char *bytes=NULL;int rc=-1;
    if(fstat(fd,&before)||!S_ISREG(before.st_mode)||before.st_nlink!=1||
       before.st_size<1||(uint64_t)before.st_size>64u*1024u*1024u)goto done;
    size_t length=(size_t)before.st_size,used=0;bytes=malloc(length);if(!bytes)goto done;
    while(used<length){ssize_t n=read(fd,bytes+used,length-used);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)goto done;
        used+=(size_t)n;
    }
    unsigned char extra;
    if(read(fd,&extra,1)!=0||fstat(fd,&after)||before.st_dev!=after.st_dev||
       before.st_ino!=after.st_ino||before.st_size!=after.st_size||
       before.st_uid!=after.st_uid||before.st_mode!=after.st_mode||before.st_nlink!=after.st_nlink||
       before.st_mtim.tv_sec!=after.st_mtim.tv_sec||before.st_mtim.tv_nsec!=after.st_mtim.tv_nsec||
       before.st_ctim.tv_sec!=after.st_ctim.tv_sec||before.st_ctim.tv_nsec!=after.st_ctim.tv_nsec)goto done;
    rc=cce_sha256_bytes_hex(bytes,length,hash);
done:free(bytes);if(close(fd))rc=-1;return rc;
}

static int import_capsule(CnetCapsuleCore *c, const char *path, char *error, size_t cap) {
    if (c->base.unit_count >= CORE_CAPSULES) {
        if (error && cap) snprintf(error, cap, "capsule_count_limit");
        return -1;
    }
    size_t bank = c->base.unit_count / HYBRID_COVERAGE_MAX;
    if (!c->coverage[bank]) {
        c->coverage[bank] = calloc(1, sizeof *c->coverage[bank]);
        if (!c->coverage[bank]) {
            if (error && cap) snprintf(error, cap, "coverage_allocation_failed");
            return -1;
        }
        hybrid_ai_init(c->coverage[bank]);
    }
    size_t original_index=c->base.unit_count;
    CnetCapsuleReport report;void *asset=NULL;size_t asset_length=0;unsigned asset_schema=0;
    if (cnet_capsule_import_asset(&c->base, c->coverage[bank], path,&asset,&asset_length,&asset_schema,&report)) {
        free(asset);
        if (error && cap) snprintf(error, cap, "import_refused:%.120s", report.reject_reason);
        return -1;
    }
    /* Imports and this read use owner-stable source or private immutable
     * snapshots. This is not a transaction against arbitrary source writers. */
    if(original_index>=CORE_CAPSULES||c->base.unit_count!=original_index+1||
       strcmp(report.unit,c->base.units[original_index].name)||
       original_payload_identity(path,c->payload_identity[original_index])) {
        free(asset);
        if(error&&cap)snprintf(error,cap,"original_payload_identity_refused");
        return -1;
    }
    if(report.schema==CNET_CAPSULE_SCHEMA_ASSET) {
        BinaryTransformNetwork btn={0};Contract contract={0};const HybridCoverage *coverage=NULL;
        HybridAi *h=c->coverage[bank];
        for(size_t i=0;i<h->coverage_count;i++)if(!strcmp(h->coverage[i].unit,report.unit))coverage=&h->coverage[i];
        CnetCapsuleEvidence *e=NULL;
        if(!cnb_get_unit(&c->base,report.unit,&btn,&contract))
            e=cnet_capsule_evidence_parse(asset,asset_length,asset_schema,&contract,coverage,error,cap);
        contract_free(&contract);btn_free(&btn);free(asset);asset=NULL;
        if(!e)return -1;
        for(size_t i=0;i<c->evidence_count;i++)if(cnet_capsule_evidence_compatible(c->evidence[i].evidence,e)){
            cnet_capsule_evidence_close(e);
            if(error&&cap)snprintf(error,cap,"conflicting_source_decoder");
            return -1;
        }
        size_t at=c->evidence_count++;
        while(at&&strcmp(c->evidence[at-1].unit,report.unit)>0){c->evidence[at]=c->evidence[at-1];at--;}
        snprintf(c->evidence[at].unit,sizeof c->evidence[at].unit,"%.63s",report.unit);c->evidence[at].evidence=e;
    }
    free(asset);
    return 0;
}

static int source_interfaces_bound(const CnetCapsuleCore *c) {
    for(size_t i=0;i<c->registry.count;i++){
        const RegistryEntry *e=c->registry.entries+i;
        /* Reserve the source entry point too: a schema-1 alternate path must
         * not turn a stale source query into an apparently certified answer. */
        for(size_t j=0;j<e->btn->input_port_count;j++)
            if(!strcmp(e->btn->input_ports[j].tag,CNET_CAPSULE_EVIDENCE_INPUT)&&!find_evidence(c,e->name))return -1;
        for(size_t j=0;j<e->btn->output_port_count;j++)
            if(!strcmp(e->btn->output_ports[j].tag,CNET_CAPSULE_EVIDENCE_OUTPUT)&&!find_evidence(c,e->name))return -1;
    }
    return 0;
}

static int same_port(Port a, Port b);
static int supported(Port p);

static int identity_order(const void *a, const void *b) {
    return strcmp(((const CnetCapsuleIdentity *)a)->unit,
                  ((const CnetCapsuleIdentity *)b)->unit);
}
int cnet_capsule_core_identities(const CnetCapsuleCore *c,
        CnetCapsuleIdentity *out, size_t capacity, size_t *count) {
    if (count) *count=0;
    if (!c || !count || c->base.unit_count>capacity || (!out && c->base.unit_count)) return -1;
    for (size_t i=0;i<c->base.unit_count;i++) {
        const CnbUnitRef *u=c->base.units+i;
        if (u->blob_index>=c->base.blob_count) return -1;
        const CnbBlob *blob=c->base.blobs+u->blob_index;
        char hashes[6][65]={{0}}, metadata[192]={0};
        if (cce_sha256_bytes_hex(blob->bytes,blob->len,hashes[0])) return -1;
        if(!c->payload_identity[i][0])return -1;
        memcpy(hashes[1],c->payload_identity[i],sizeof hashes[1]);
        const CoreCoverageRef *guard=find_guard(c,u->name);
        if (guard) {
            const HybridCoverage *g=guard->record;
            if (!g->rows || g->n_rows>4096 || g->in_dim>64 || g->out_dim>64 ||
                (g->out_dim && !g->targets)) return -1;
            snprintf(metadata,sizeof metadata,"%zu:%zu:%zu:%d:%zu",g->n_rows,g->in_dim,g->out_dim,g->generalizes,g->held_out);
            if (cce_sha256_bytes_hex(g->rows,g->n_rows*g->in_dim*sizeof(double),hashes[2]) ||
                cce_sha256_bytes_hex(g->targets,g->n_rows*g->out_dim*sizeof(double),hashes[3])) return -1;
        }
        CnetCapsuleEvidence *e=find_evidence(c,u->name);
        if((e&&cnet_capsule_evidence_identity(e,hashes[5])) || cce_sha256_bytes_hex(metadata,sizeof metadata,hashes[4]) ||
            cce_sha256_bytes_hex(hashes,e?sizeof hashes:5*sizeof hashes[0],out[i].sha256)) return -1;
        snprintf(out[i].unit,sizeof out[i].unit,"%s",u->name);
    }
    if(c->base.unit_count) qsort(out,c->base.unit_count,sizeof *out,identity_order);
    *count=c->base.unit_count;
    return 0;
}

CnetCapsuleCore *cnet_capsule_core_empty(void) {
    CnetCapsuleCore *c=calloc(1,sizeof *c);
    if (!c) return NULL;
    cnb_init(&c->base); registry_init_production(&c->registry);
    if (build_guard_index(c) || build_edge_index(c)) {
        cnet_capsule_core_close(c); return NULL;
    }
    return c;
}
static int coverage_labelled(const CnetCapsuleCore *c) {
    CoreBudget budget; budget_init(&budget, admission_work(c->registry.count));
    for (size_t i = 0; i < c->registry.count; i++) {
        const RegistryEntry *e = &c->registry.entries[i];
        const BinaryTransformNetwork *b = e->btn;
        const RegistryCertCoverage *labels = e->cert_cov;
        if (b->input_port_count != 1 || b->output_port_count != 1 ||
            !supported(b->input_ports[0]) || !supported(b->output_ports[0]) ||
            b->hidden_count > 256 || !labels || !labels->inputs || !labels->targets ||
            !labels->n_rows || labels->n_rows > 4096 || labels->in_dim != b->input_count ||
            labels->out_dim != b->output_count) return 0;
        if (!c->guards_ready) return 0;
        const CoreCoverageRef *ref = find_guard(c, e->name);
        if (ref) {
            const HybridCoverage *h = ref->record;
            if (h->in_dim != labels->in_dim || !same_port(h->input_port, b->input_ports[0]) ||
                !same_port(h->goal_port, b->output_ports[0])) return 0;
            for (size_t r = 0; r < h->n_rows; r++) {
                int found = 0;
                for (size_t k = 0; k < labels->n_rows; k++) {
                    if (charge(&budget)) return 0;
                    if (!memcmp(h->rows + r*h->in_dim, labels->inputs + k*labels->in_dim, h->in_dim*sizeof(double))) {
                        found = 1; break;
                    }
                }
                if (!found) return 0;
            }
        }
    }
    return 1;
}
static int entry_input_order(const void *aa, const void *bb) {
    const RegistryEntry *a = *(const RegistryEntry * const *)aa;
    const RegistryEntry *b = *(const RegistryEntry * const *)bb;
    int order = port_order(a->btn->input_ports[0], b->btn->input_ports[0]);
    return order ? order : port_order(a->btn->output_ports[0], b->btn->output_ports[0]);
}
static int entry_output_order(const void *aa, const void *bb) {
    const RegistryEntry *a = *(const RegistryEntry * const *)aa;
    const RegistryEntry *b = *(const RegistryEntry * const *)bb;
    return port_order(a->btn->output_ports[0], b->btn->output_ports[0]);
}
static int contracts_consistent(const PrimitiveRegistry *reg) {
    CoreBudget budget; budget_init(&budget, 2000000);
    if (reg->count > CORE_CAPSULES) return 0;
    if (!reg->count) return 1;
    const RegistryEntry **sorted = calloc(reg->count, sizeof *sorted);
    if (!sorted) return 0;
    int ok = 0;
    for (size_t i = 0; i < reg->count; i++) {
        sorted[i] = &reg->entries[i];
        if (!sorted[i]->btn || sorted[i]->btn->input_port_count != 1 ||
            sorted[i]->btn->output_port_count != 1) goto done;
    }
    /* Validate each textual direction independently. Do not reorder registry
     * entries: their order is the stable route tie-breaker. */
    qsort(sorted, reg->count, sizeof *sorted, entry_output_order);
    for (size_t i = 1; i < reg->count; i++) {
        Port a = sorted[i-1]->btn->output_ports[0], b = sorted[i]->btn->output_ports[0];
        if (charge(&budget) || (!strcmp(a.tag, b.tag) && !same_port(a,b))) goto done;
    }
    qsort(sorted, reg->count, sizeof *sorted, entry_input_order);
    for (size_t i = 1; i < reg->count; i++) {
        Port a = sorted[i-1]->btn->input_ports[0], b = sorted[i]->btn->input_ports[0];
        if (charge(&budget) || (!strcmp(a.tag, b.tag) && !same_port(a,b))) goto done;
    }
    for (size_t i = 0; i < reg->count; i++) for (size_t j = i; j > 0;) {
        j--;
        if (charge(&budget)) goto done;
        const RegistryEntry *a = sorted[i], *b = sorted[j];
        if (!same_port(a->btn->input_ports[0], b->btn->input_ports[0]) ||
            !same_port(a->btn->output_ports[0], b->btn->output_ports[0])) break;
        const RegistryCertCoverage *x = a->cert_cov, *y = b->cert_cov;
        if (!x || !y || x->in_dim != y->in_dim || x->out_dim != y->out_dim) goto done;
        for (size_t p = 0; p < x->n_rows; p++) for (size_t q = 0; q < y->n_rows; q++) {
            if (charge(&budget)) goto done;
            if (!memcmp(x->inputs + p*x->in_dim, y->inputs + q*y->in_dim, x->in_dim*sizeof(double)) &&
                 memcmp(x->targets + p*x->out_dim, y->targets + q*y->out_dim, x->out_dim*sizeof(double))) goto done;
        }
    }
    ok = 1;
done:
    free(sorted); return ok;
}

void cnet_capsule_core_close(CnetCapsuleCore *c) {
    if (!c) return;
    registry_free(&c->registry);
    for (size_t bank = 0; bank < CORE_COVERAGE_BANKS; bank++) if (c->coverage[bank]) {
        hybrid_ai_free(c->coverage[bank]);
        free(c->coverage[bank]);
    }
    cnb_free(&c->base);
    for(size_t i=0;i<c->evidence_count;i++)cnet_capsule_evidence_close(c->evidence[i].evidence);
    free(c);
}

CnetCapsuleCore *cnet_capsule_core_open(const char *root, char *error, size_t cap) {
    struct dirent **names = NULL;
    int count = -1, loaded = 0;
    const char *why = "capsule_directory_unavailable";
    CnetCapsuleCore *c = calloc(1, sizeof *c);
    if (error && cap) error[0] = 0;
    if (!c) return NULL;
    cnb_init(&c->base);
    registry_init_production(&c->registry);
    if (!root || !root[0] || (count = scandir(root, &names, NULL, alphasort)) < 0) goto fail;
    for (int i = 0; i < count; i++) {
        char path[1200]; struct stat st;
        if (names[i]->d_name[0] == '.') continue;
        int n = snprintf(path, sizeof path, "%s/%s", root, names[i]->d_name);
        if (n < 0 || (size_t)n >= sizeof path || lstat(path, &st)) goto fail;
        if (!S_ISDIR(st.st_mode)) { why = "unexpected_capsule_entry"; goto fail; }
        if (++loaded > (int)CORE_CAPSULES) { why = "capsule_count_limit"; goto fail; }
        if (import_capsule(c, path, error, cap)) goto fail;
    }
    size_t skipped = 0;
    if (cnb_load_registry(&c->base, &c->registry, &skipped) < 0 || skipped ||
        c->registry.count != c->base.unit_count) {
        why = "certification_replay_failed"; goto fail;
    }
    if (build_guard_index(c) || !coverage_labelled(c) || source_interfaces_bound(c)) { why = "unlabelled_coverage_or_unsupported_kernel"; goto fail; }
    if (!contracts_consistent(&c->registry)) { why = "conflicting_contracts_or_comparison_budget"; goto fail; }
    if (build_edge_index(c)) { why = "edge_index_refused"; goto fail; }
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
    return c;
fail:
    if (error && cap && !error[0]) snprintf(error, cap, "%s", why);
    for (int i = 0; i < count; i++) free(names[i]);
    free(names); cnet_capsule_core_close(c); return NULL;
}

static int same_port(Port a, Port b) {
    return a.family == b.family && a.field_width == b.field_width &&
           a.field_count == b.field_count && !strcmp(a.tag, b.tag);
}

CnetCapsuleCore *cnet_capsule_core_open_candidate(const char *root,
        const char *candidate, char *error, size_t cap) {
    CnetCapsuleCore *c = cnet_capsule_core_open(root, error, cap);
    if (!c) return NULL;
    if (import_capsule(c, candidate, error, cap)) {
        cnet_capsule_core_close(c); return NULL;
    }
    registry_free(&c->registry); registry_init_production(&c->registry);
    size_t skipped = 0;
    if (cnb_load_registry(&c->base, &c->registry, &skipped) < 0 || skipped ||
        c->registry.count != c->base.unit_count || c->registry.count > CORE_CAPSULES ||
        build_guard_index(c) || !coverage_labelled(c) || source_interfaces_bound(c) || !contracts_consistent(&c->registry) || build_edge_index(c)) {
        if (error && cap) snprintf(error, cap, "candidate_contract_conflict");
        cnet_capsule_core_close(c); return NULL;
    }
    return c;
}

static int resolve_port(CnetCapsuleCore *c, const char *tag, int output, Port *p) {
    int found = 0;
    for (size_t i = 0; i < c->registry.count; i++) {
        BinaryTransformNetwork *b = c->registry.entries[i].btn;
        size_t n = output ? b->output_port_count : b->input_port_count;
        if (n != 1) continue;
        Port candidate = output ? b->output_ports[0] : b->input_ports[0];
        if (strcmp(candidate.tag, tag)) continue;
        if (found && !same_port(*p, candidate)) return -1;
        *p = candidate; found = 1;
    }
    if (!found || p->field_count != 1 || !p->field_width) return -1;
    if (p->family == PORT_ONEHOT) return p->field_width <= 64 ? 0 : -1;
    if (p->family == PORT_BINARY_MSB || p->family == PORT_BINARY_LSB)
        return p->field_width <= 16 ? 0 : -1;
    return -1;
}

typedef struct { CnetCapsuleCore *core; const char *blocked; int live; } CoreGuard;
static int covered(const char *unit, const BinaryTransformNetwork *btn,
                   const double *in, size_t len, void *ctx) {
    CoreGuard *g = ctx;
    CnetCapsuleCore *c = g->core;
    if (btn->input_port_count != 1 || btn->output_port_count != 1) { g->blocked = unit; return -1; }
    if (!c->guards_ready) { g->blocked = unit; return -1; }
    CnetCapsuleEvidence *e=find_evidence(c,unit);
    if(g->live&&e&&cnet_capsule_evidence_fresh(e,getenv("CNET_CAPSULE_SOURCE_ROOT"),NULL,0)) {
        g->blocked=unit;return -1;
    }
    const CoreCoverageRef *ref = find_guard(c, unit);
    /* Canonical import proves exhaustive scope when there is no record. */
    if (!ref || hybrid_coverage_admits_exact(ref->bank, unit, btn->input_ports[0],
                btn->output_ports[0], in, len)) return 0;
    g->blocked = unit; return -1;
}

static int supported(Port p) {
    return p.field_count == 1 && p.field_width &&
        ((p.family == PORT_ONEHOT && p.field_width <= 64) ||
         ((p.family == PORT_BINARY_MSB || p.family == PORT_BINARY_LSB) && p.field_width <= 16));
}
static unsigned decode(Port p, const double *v) {
    unsigned value = 0;
    for (size_t i = 0; i < p.field_width; i++) if (v[i] == 1.0) {
        if (p.family == PORT_ONEHOT) value = (unsigned)i;
        else value |= 1u << (p.family == PORT_BINARY_MSB ? p.field_width - 1 - i : i);
    }
    return value;
}
/* Values, not types alone, determine coverage. Execute each expansion through
 * the canonical strict executor; no guessed/model-free serving fast path. */
static int search(CnetCapsuleCore *c, Port pin, Port goal, const double *input,
                  CnetCapsuleCoreReply *r, CoreBudget *budget,int live) {
    CoreState *states = calloc(CORE_STATES, sizeof *states);
    if (!states) return -1;
    states[0].port = pin; memcpy(states[0].value, input, pin.field_width * sizeof(double));
    size_t count = 1, work = 65536; int rc = -1;
    CoreGuard cg = {c, NULL, live}; DagNodeGuard guard = {covered, &cg};
    snprintf(r->reason, sizeof r->reason, "no_covered_certified_plan");
    for (size_t head = 0; head < count; head++) {
        CoreState *s = &states[head];
        if (s->depth == ROUTE_MAX_STEPS) continue;
        size_t begin, end;
        if (edge_range(c, s->port, &begin, &end, budget, &work)) goto exhausted;
        for (size_t edge = begin; edge < end; edge++) {
            size_t i = c->edge_order[edge];
            if (!work-- || charge(budget)) goto exhausted;
            RegistryEntry *e = &c->registry.entries[i]; BinaryTransformNetwork *b = e->btn;
            /* The snapshot is private. Audit only the selected entry, through
             * the canonical auditor, before every execution (also in replay).
             * Fail immediately on demotion: audit frees that entry's labels. */
            PrimitiveRegistry selected = {0}; selected.entries = e; selected.count = 1;
            if (registry_audit_certified(&selected)) {
                snprintf(r->reason, sizeof r->reason, "certification_audit_refused"); goto done;
            }
            if (!e->certified || e->state == PRIM_RESET || b->input_port_count != 1 ||
                b->output_port_count != 1 || !same_port(s->port, b->input_ports[0])) continue;
            Port next = b->output_ports[0];
            if (!supported(next) || b->hidden_count > 256) { snprintf(r->reason, sizeof r->reason, "unsupported_kernel_shape"); goto done; }
            RoutePlan step = {0}; step.length = 1; step.strict = 1; step.goal = next;
            step.steps[0] = b; step.names[0] = e->name;
            double out[64];
            int exec = route_execute_guarded(&step, s->value, s->port.field_width, out, next.field_width, NULL, &guard);
            if (exec == ROUTE_EXEC_REFUSED_GUARD) continue;
            if (exec) { snprintf(r->reason, sizeof r->reason, "execution_refused"); goto done; }
            /* A positive executed cycle may reach the initial state as goal. */
            if (same_port(next, goal)) {
                size_t chain[ROUTE_MAX_STEPS], depth = s->depth + 1, at = head;
                chain[depth - 1] = i;
                for (size_t n = depth - 1; n; n--) { chain[n - 1] = states[at].edge; at = states[at].parent; }
                size_t used = 0;
                for (size_t n = 0; n < depth; n++) {
                    int k = snprintf(r->units + used, sizeof r->units - used, "%s%s", n ? "," : "", c->registry.entries[chain[n]].name);
                    if (k < 0 || (size_t)k >= sizeof r->units - used) { snprintf(r->reason, sizeof r->reason, "receipt_overflow"); goto done; }
                    used += (size_t)k;
                }
                r->value = decode(goal, out); r->hops = depth; r->verified = 1; r->reason[0] = 0;
                rc = 0; goto done;
            }
            int seen = 0;
            for (size_t n = 0; n < count; n++) {
                if (!work-- || charge(budget)) goto exhausted;
                if (same_port(states[n].port, next) && !memcmp(states[n].value, out, next.field_width * sizeof(double))) { seen = 1; break; }
            }
            if (seen) continue;
            if (count == CORE_STATES) goto exhausted;
            CoreState *added = &states[count++]; added->port = next; added->depth = s->depth + 1;
            added->parent = head; added->edge = i;
            memcpy(added->value, out, next.field_width * sizeof(double));
        }
    }
    goto done;
exhausted:
    snprintf(r->reason, sizeof r->reason, "search_budget_exhausted");
done:
    if (rc) { r->verified = 0; r->value = 0; r->hops = 0; r->units[0] = 0; }
    free(states); return rc;
}

/* Compose SEALED LABELS through exact guarded joins. These expected outputs
 * never come from executing CNET, and are used only to check admission. */
static int cell_search(CnetCapsuleCore *,Port,Port,const double *,const CnetCoreCell *,uint64_t,CnetCapsuleCoreReply *,CoreBudget *,int);
static int replay_labels(CnetCapsuleCore *labels, CnetCapsuleCore *candidate,
                         CoreBudget *budget, size_t *checked,const CnetCoreCell *cell,uint64_t generation) {
    CoreState *states = calloc(CORE_STATES, sizeof *states);
    if (!states) return -1;
    int rc = -1;
    CoreGuard cg = {labels, NULL, 0};
    for (size_t source = 0; source < labels->registry.count; source++) {
        const RegistryEntry *origin = &labels->registry.entries[source];
        if (!origin->btn || !origin->certified || !origin->cert_cov) goto done;
        Port input_port = origin->btn->input_ports[0];
        for (size_t row = 0; row < origin->cert_cov->n_rows; row++) {
            const double *input = origin->cert_cov->inputs + row*origin->cert_cov->in_dim;
            if (covered(origin->name, origin->btn, input, input_port.field_width, &cg)) continue;
            size_t count = 1; states[0].port = input_port; states[0].depth = 0;
            memcpy(states[0].value, input, input_port.field_width*sizeof(double));
            for (size_t head = 0; head < count; head++) {
                CoreState *s = &states[head];
                if (s->depth == ROUTE_MAX_STEPS) continue;
                size_t begin, end;
                if (edge_range(labels, s->port, &begin, &end, budget, NULL)) goto done;
                for (size_t edge = begin; edge < end; edge++) {
                    size_t i = labels->edge_order[edge];
                    if (charge(budget)) goto done;
                    const RegistryEntry *e = &labels->registry.entries[i];
                    if (!e->btn || !e->certified || !e->cert_cov) goto done;
                    if (!same_port(s->port, e->btn->input_ports[0]) ||
                        covered(e->name, e->btn, s->value, s->port.field_width, &cg)) continue;
                    const RegistryCertCoverage *table = e->cert_cov;
                    for (size_t r = 0; r < table->n_rows; r++) {
                        if (charge(budget)) goto done;
                        if (memcmp(s->value, table->inputs + r*table->in_dim, table->in_dim*sizeof(double))) continue;
                        const double *expected = table->targets + r*table->out_dim;
                        Port next = e->btn->output_ports[0]; CnetCapsuleCoreReply result = {0};
                        int status=cell?cell_search(candidate,input_port,next,input,cell,generation,&result,budget,0):
                            search(candidate,input_port,next,input,&result,budget,0);
                        if (status ||
                            !result.verified || result.value != decode(next, expected)) goto done;
                        (*checked)++;
                        int seen = 0;
                        for (size_t n = 0; n < count; n++) {
                            if (charge(budget)) goto done;
                            if (same_port(states[n].port, next) && !memcmp(states[n].value, expected, table->out_dim*sizeof(double))) { seen = 1; break; }
                        }
                        if (seen) continue;
                        if (count == CORE_STATES) goto done;
                        CoreState *added = &states[count++]; added->port = next; added->depth = s->depth + 1;
                        memcpy(added->value, expected, table->out_dim*sizeof(double));
                    }
                }
            }
        }
    }
    rc = 0;
done:
    free(states); return rc;
}

static int hash_order(const void *a,const void *b){return strcmp(a,b);}
static int evidence_growth(const CnetCapsuleCore *before,const CnetCapsuleCore *candidate) {
    if(!before->evidence_count)return 0;
    if(!candidate->evidence_count||cnet_capsule_evidence_compatible(before->evidence[0].evidence,candidate->evidence[0].evidence))return -1;
    char (*hashes)[65]=calloc(candidate->evidence_count,sizeof *hashes);
    if(!hashes)return -1;
    int rc=-1;
    for(size_t i=0;i<candidate->evidence_count;i++)
        if(cnet_capsule_evidence_identity(candidate->evidence[i].evidence,hashes[i]))goto done;
    qsort(hashes,candidate->evidence_count,sizeof *hashes,hash_order);
    for(size_t i=0;i<before->evidence_count;i++) {
        char hash[65];
        if(cnet_capsule_evidence_identity(before->evidence[i].evidence,hash)||
           !bsearch(hash,hashes,candidate->evidence_count,sizeof *hashes,hash_order))goto done;
    }
    rc=0;
done:free(hashes);return rc;
}
int cnet_capsule_core_validate_growth(CnetCapsuleCore *before, CnetCapsuleCore *candidate,
                                    size_t *label_obligations) {
    if (!before || !candidate || !label_obligations) return -1;
    *label_obligations = 0;
    /* Static semantic obligations accompany numeric labels. Live freshness is
     * a serving guard: stale facts must not disable unrelated resident units. */
    if(evidence_growth(before,candidate))return -1;
    size_t count = before->registry.count > candidate->registry.count ? before->registry.count : candidate->registry.count;
    CoreBudget budget; budget_init(&budget, admission_work(count));
    /* Full-inventory replay now audits every selected kernel as well. Its
     * cooperative deadline scales with inventory work (2..32 seconds), while
     * ordinary queries and the other admission phases retain two seconds. */
    budget.seconds = (double)admission_work(count) / 1000000.0;
    /* Checking the candidate's closure also rejects NEW contradictory paths,
     * even when the old shortest answer would otherwise remain unchanged. */
    if (replay_labels(before, candidate, &budget, label_obligations,NULL,0) ||
        replay_labels(candidate, candidate, &budget, label_obligations,NULL,0)) return -1;
    return 0;
}
int cnet_capsule_core_validate_growth_cell(CnetCapsuleCore *before,CnetCapsuleCore *candidate,
    const CnetCoreCell *cell,uint64_t generation,size_t *obligations){
    if(!generation||cnet_core_cell_validate(cell)||cnet_capsule_core_validate_growth(before,candidate,obligations))return -1;
    size_t count=before->registry.count>candidate->registry.count?before->registry.count:candidate->registry.count;
    CoreBudget budget;budget_init(&budget,admission_work(count));budget.seconds=(double)admission_work(count)/1000000;
    return replay_labels(before,candidate,&budget,obligations,cell,generation)||replay_labels(candidate,candidate,&budget,obligations,cell,generation)?-1:0;
}

static unsigned type_id(Port *types,unsigned *count,Port p){
    for(unsigned i=0;i<*count;i++)if(same_port(types[i],p))return i+1;
    types[*count]=p;return ++*count;
}
static int cell_search(CnetCapsuleCore *c,Port pin,Port goal,const double *input,
    const CnetCoreCell *cell,uint64_t generation,CnetCapsuleCoreReply *r,CoreBudget *budget,int live){
    snprintf(r->reason,sizeof r->reason,"cell_proposal_refused");
    if(!generation||c->registry.count>62||cnet_core_cell_validate(cell))return -1;
    CnetSelectorGraph g={0};g.feature_version=1;g.generation=generation;g.n=(unsigned)c->registry.count+2;g.goal=g.n-1;
    Port types[128];unsigned count=0;
    unsigned a=type_id(types,&count,pin),b=type_id(types,&count,goal);
    g.node[0]=(CnetSelectorNode){1,1,a,a,1};g.node[g.goal]=(CnetSelectorNode){UINT64_MAX,1,b,b,1};
    for(unsigned i=0;i<c->registry.count;i++){
        RegistryEntry *e=&c->registry.entries[i];BinaryTransformNetwork *btn=e->btn;
        if(!e->certified||btn->input_port_count!=1||btn->output_port_count!=1)return -1;
        uint64_t version=contract_btn_digest(btn);if(!version)return -1;
        g.node[i+1]=(CnetSelectorNode){i+2,version,type_id(types,&count,btn->input_ports[0]),type_id(types,&count,btn->output_ports[0]),1};
    }
    for(unsigned u=0;u<g.goal;u++)for(unsigned v=1;v<g.n;v++)
        if(g.node[u].output_type==g.node[v].input_type&&!(u==0&&v==g.goal))g.edge[u]|=UINT64_C(1)<<v;
    CnetSelectorProposal p;
    if(cnet_core_selector_propose(cell,&g,64,&p)||!p.hops||p.hops>ROUTE_MAX_STEPS+1)return -1;
    CoreGuard cg={c,NULL,live};DagNodeGuard guard={covered,&cg};double value[64]={0};memcpy(value,input,pin.field_width*sizeof(double));
    Port current=pin;size_t used=0,hops=0;
    for(unsigned h=0;h+1<p.hops;h++){
        unsigned index=p.path[h];if(!index||index>=g.goal||charge(budget))goto refuse;
        RegistryEntry *e=&c->registry.entries[index-1];BinaryTransformNetwork *btn=e->btn;
        PrimitiveRegistry selected={0};selected.entries=e;selected.count=1;
        if(registry_audit_certified(&selected)||!e->certified||e->state==PRIM_RESET||!same_port(current,btn->input_ports[0]))goto refuse;
        Port next=btn->output_ports[0];if(!supported(next)||btn->hidden_count>256)goto refuse;
        RoutePlan step={0};step.length=1;step.strict=1;step.goal=next;step.steps[0]=btn;step.names[0]=e->name;
        double out[64];if(route_execute_guarded(&step,value,current.field_width,out,next.field_width,NULL,&guard))goto refuse;
        int n=snprintf(r->units+used,sizeof r->units-used,"%s%s",hops?",":"",e->name);
        if(n<0||(size_t)n>=sizeof r->units-used)goto refuse;
        used+=(size_t)n;hops++;memcpy(value,out,next.field_width*sizeof(double));current=next;
    }
    if(!hops||p.path[p.hops-1]!=g.goal||!same_port(current,goal))goto refuse;
    r->verified=1;r->value=decode(goal,value);r->hops=hops;r->reason[0]=0;return 0;
refuse:
    memset(r,0,sizeof *r);snprintf(r->reason,sizeof r->reason,"cell_execution_refused");return -1;
}
static int finish_answer(CnetCapsuleCore *c,CnetCapsuleCoreReply *r,char *text,size_t cap) {
    char units[sizeof r->units];memcpy(units,r->units,sizeof units);
    char *save=NULL;CnetCapsuleEvidence *terminal=NULL;
    for(char *unit=strtok_r(units,",",&save);unit;unit=strtok_r(NULL,",",&save)){
        terminal=find_evidence(c,unit);
        if(terminal&&cnet_capsule_evidence_fresh(terminal,getenv("CNET_CAPSULE_SOURCE_ROOT"),NULL,0))goto refused;
    }
    if(text) {
        if(terminal){if(cnet_capsule_evidence_render(terminal,r->value,text,cap))goto refused;}
        else {int n=snprintf(text,cap,"%u",r->value);if(n<0||(size_t)n>=cap)goto refused;}
    }
    return 0;
refused:
    if(text&&cap)text[0]=0;
    memset(r,0,sizeof *r);snprintf(r->reason,sizeof r->reason,"source_freshness_or_render_refused");return -1;
}
static int ask_mode(CnetCapsuleCore *c,const char *request,const CnetCoreCell *cell,uint64_t generation,
                    CnetCapsuleCoreReply *r,char *text,size_t cap) {
    char verb[16], in_tag[32], out_tag[32], value[32], extra;
    Port pin = {0}, pout = {0};
    double in[64] = {0};
    if (!r) return -1;
    if(text&&cap)text[0]=0;
    memset(r, 0, sizeof *r);
    snprintf(r->reason, sizeof r->reason, "invalid_typed_request");
    if (!c || !request || sscanf(request, "%15s %31s %31s %31s %c", verb,
        in_tag, out_tag, value, &extra) != 4 || strcmp(verb, "capsule")) return -1;
    for (size_t i = 0; value[i]; i++) if (!isdigit((unsigned char)value[i])) return -1;
    errno = 0; char *end;
    unsigned long x = strtoul(value, &end, 10);
    if (errno || *end || x > 65535) return -1;
    if (resolve_port(c, in_tag, 0, &pin) || resolve_port(c, out_tag, 1, &pout)) {
        snprintf(r->reason, sizeof r->reason, "unknown_or_ambiguous_interface"); return -1;
    }
    unsigned long domain = pin.family == PORT_ONEHOT ? pin.field_width : (1UL << pin.field_width);
    if (x >= domain) return -1;
    for (size_t i = 0; i < pin.field_width; i++) {
        size_t bit = pin.family == PORT_BINARY_MSB ? pin.field_width - 1 - i : i;
        in[i] = pin.family == PORT_ONEHOT ? (x == i) : (double)((x >> bit) & 1UL);
    }
    CoreBudget budget; budget_init(&budget, 65536);
    int rc=cell?cell_search(c,pin,pout,in,cell,generation,r,&budget,1):search(c,pin,pout,in,r,&budget,1);
    return rc?rc:finish_answer(c,r,text,cap);
}
int cnet_capsule_core_ask(CnetCapsuleCore *c,const char *request,CnetCapsuleCoreReply *r){return ask_mode(c,request,NULL,0,r,NULL,0);}
int cnet_capsule_core_ask_cell(CnetCapsuleCore *c,const char *request,const CnetCoreCell *cell,uint64_t generation,CnetCapsuleCoreReply *r){
    if(!cell){if(r)memset(r,0,sizeof *r);return -1;}
    return ask_mode(c,request,cell,generation,r,NULL,0);
}
int cnet_capsule_core_ask_text(CnetCapsuleCore *c,const char *request,CnetCapsuleCoreReply *r,char *text,size_t cap) {
    if(!text||!cap){if(r)memset(r,0,sizeof *r);return -1;}
    return ask_mode(c,request,NULL,0,r,text,cap);
}
int cnet_capsule_core_ask_cell_text(CnetCapsuleCore *c,const char *request,const CnetCoreCell *cell,uint64_t generation,
                                 CnetCapsuleCoreReply *r,char *text,size_t cap) {
    if(text&&cap)text[0]=0;
    if(!cell||!text||!cap){if(r)memset(r,0,sizeof *r);return -1;}
    return ask_mode(c,request,cell,generation,r,text,cap);
}
