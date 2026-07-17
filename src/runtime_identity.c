/* Linked-runtime attestation for oracle identities (spec:
   plans/toolchain_digest_linked_runtime.md).

   The toolchain digest folds compiler + flags + source revision, but the
   numerics of a teaching stack also ride on code linked at run time —
   libm's expf/tanh, the OpenMP runtime's scheduling, libc itself. This
   unit folds what is ACTUALLY loaded: every DSO's (soname, GNU build-id)
   pair, soname-sorted so dlopen order cannot change the digest, plus the
   glibc version (build-ids can be stripped; the version string cannot).

   GPU driver/kernel folding is a documented follow-up: CPU-only teaching
   must not change identity when a GPU driver updates, so nothing GPU is
   folded here — the GPU oracle pool folds its own driver identity when a
   GPU lane teaches (not in this slice).

   Own translation unit because dl_iterate_phdr needs _GNU_SOURCE, which
   must not leak into acquire.c's feature-macro environment. */

#if defined(__GLIBC__) || defined(__gnu_linux__)
#define _GNU_SOURCE
#endif

#include "../include/acquire.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rt_fnv_bytes(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; ++i) {
        h ^= b[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

#ifdef __GLIBC__

#include <link.h>
#include <gnu/libc-version.h>

#define RT_MAX_OBJS 256
#define RT_NAME_MAX 128
#define RT_BUILD_ID_MAX 64

typedef struct {
    char name[RT_NAME_MAX];
    unsigned char build_id[RT_BUILD_ID_MAX];
    size_t build_id_len;
} RtObj;

typedef struct {
    RtObj obj[RT_MAX_OBJS];
    size_t count;
} RtScan;

/* NT_GNU_BUILD_ID note inside a PT_NOTE segment: 4-byte-aligned records of
   (namesz, descsz, type) + "GNU\0" + id bytes. */
static void rt_find_build_id(const struct dl_phdr_info *info, RtObj *o) {
    size_t ph;
    for (ph = 0; ph < info->dlpi_phnum; ++ph) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[ph];
        const unsigned char *cur, *end;
        if (p->p_type != PT_NOTE) continue;
        cur = (const unsigned char *)(info->dlpi_addr + p->p_vaddr);
        end = cur + p->p_memsz;
        while (cur + sizeof(ElfW(Nhdr)) <= end) {
            const ElfW(Nhdr) *n = (const ElfW(Nhdr) *)cur;
            size_t namesz = (n->n_namesz + 3u) & ~3u;
            size_t descsz = (n->n_descsz + 3u) & ~3u;
            const unsigned char *name = cur + sizeof(ElfW(Nhdr));
            const unsigned char *desc = name + namesz;
            if (desc + descsz > end) break;
            if (n->n_type == NT_GNU_BUILD_ID && n->n_namesz == 4 &&
                memcmp(name, "GNU", 4) == 0 && n->n_descsz > 0) {
                o->build_id_len = n->n_descsz < RT_BUILD_ID_MAX
                                      ? n->n_descsz : RT_BUILD_ID_MAX;
                memcpy(o->build_id, desc, o->build_id_len);
                return;
            }
            cur = desc + descsz;
        }
    }
}

static int rt_collect(struct dl_phdr_info *info, size_t size, void *data) {
    RtScan *s = (RtScan *)data;
    RtObj *o;
    const char *base;
    (void)size;
    if (s->count >= RT_MAX_OBJS) return 0;
    o = &s->obj[s->count];
    memset(o, 0, sizeof *o);
    /* the main executable reports an empty name — give it a stable one */
    base = (info->dlpi_name && info->dlpi_name[0]) ? info->dlpi_name
                                                   : "(main)";
    {
        const char *slash = strrchr(base, '/');
        if (slash) base = slash + 1;
    }
    snprintf(o->name, sizeof o->name, "%s", base);
    rt_find_build_id(info, o);
    s->count++;
    return 0;
}

static int rt_cmp(const void *a, const void *b) {
    return strcmp(((const RtObj *)a)->name, ((const RtObj *)b)->name);
}

uint64_t cnet_runtime_libs_digest(void) {
    static RtScan s;   /* ~50 KB — off the stack; computed per call */
    uint64_t h = UINT64_C(1469598103934665603);
    size_t i;
    unsigned char sep_pair = 0xff, sep_field = 0xfe;
    s.count = 0;
    dl_iterate_phdr(rt_collect, &s);
    qsort(s.obj, s.count, sizeof s.obj[0], rt_cmp);
    for (i = 0; i < s.count; ++i) {
        h = rt_fnv_bytes(h, s.obj[i].name, strlen(s.obj[i].name));
        h = rt_fnv_bytes(h, &sep_field, 1);
        if (s.obj[i].build_id_len)
            h = rt_fnv_bytes(h, s.obj[i].build_id, s.obj[i].build_id_len);
        else
            h = rt_fnv_bytes(h, "no-build-id", 11);
        h = rt_fnv_bytes(h, &sep_pair, 1);
    }
    {
        const char *v = gnu_get_libc_version();
        h = rt_fnv_bytes(h, "glibc:", 6);
        h = rt_fnv_bytes(h, v, strlen(v));
    }
    return h ? h : UINT64_C(1);
}

#else /* !__GLIBC__ */

/* No dl introspection: 0 = "linked runtime unattested", the same visible
   label an old base carries. Never fold a fake sentinel as if it were
   real data. */
uint64_t cnet_runtime_libs_digest(void) { return 0; }

#endif
