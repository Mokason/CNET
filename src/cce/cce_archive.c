#include "../../include/cce/cce_archive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* cce_archive: single-file archive with memory-mapped sections for zero-copy views.
   This enables the "hot/warm/cold" tiered partial load model without full deserialization. */

struct cce_archive {
    char   path[256];
    size_t file_size;
    void*  mapped;           /* base of memory mapping */
    size_t next_write_offset;

    /* Simple on-disk directory for fast lookup of thousands of contracts */
#define CCE_ARCHIVE_MAX_SECTIONS 4096
    struct {
        char   name[64];
        size_t offset;
        size_t size;
        uint32_t flags;
    } dir[CCE_ARCHIVE_MAX_SECTIONS];
    int    num_sections;

    /* Simple fixed header for dir persistence */
#define CCE_ARCH_DIR_HEADER_SIZE 4096
    size_t dir_offset;  /* location of last written dir blob (header-based) */
    size_t dir_blob_size;
    /* Header at offset 0: magic[4] + dir_offset + dir_size (for reliable reload) */

#ifdef _WIN32
    HANDLE file_handle;
    HANDLE map_handle;
#else
    FILE*  f;  /* fallback */
#endif
};

/* Dir persist prototypes */
static void cce_archive_load_dir_if_present(cce_archive* arc);
void cce_archive_flush_dir(cce_archive* arc);

cce_result cce_archive_open(cce_archive** arc_out, const char* path) {
    if (!arc_out || !path) return CCE_ERR_INVALID_ARG;

    cce_archive* arc = (cce_archive*)calloc(1, sizeof(cce_archive));
    if (!arc) return CCE_ERR_OOM;

    strncpy(arc->path, path, sizeof(arc->path) - 1);

#ifdef _WIN32
    arc->file_handle = CreateFileA(
        path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (arc->file_handle == INVALID_HANDLE_VALUE) {
        free(arc);
        return CCE_ERR_IO;
    }

    LARGE_INTEGER file_size_li;
    if (!GetFileSizeEx(arc->file_handle, &file_size_li)) {
        file_size_li.QuadPart = 0;
    }
    arc->file_size = (size_t)file_size_li.QuadPart;

    /* Only create mapping if file has size > 0 */
    if (arc->file_size > 0) {
        arc->map_handle = CreateFileMappingA(
            arc->file_handle,
            NULL,
            PAGE_READWRITE,
            0, 0,
            NULL
        );

        if (arc->map_handle) {
            arc->mapped = MapViewOfFile(
                arc->map_handle,
                FILE_MAP_ALL_ACCESS,
                0, 0,
                0
            );
        }
    }
#else
    arc->f = fopen(path, "r+b");
    if (!arc->f) arc->f = fopen(path, "w+b");
    if (!arc->f) {
        free(arc);
        return CCE_ERR_IO;
    }
    fseek(arc->f, 0, SEEK_END);
    arc->file_size = ftell(arc->f);
    rewind(arc->f);
#endif

    /* Reserve space for the CCE1 header (4 bytes magic + 2 × size_t) at offset 0
       so that the first appended section never lands at offset 0 and the header
       write on close does not corrupt cascade data. For existing (non-empty) files
       next_write_offset stays at file_size as before. */
    {
        size_t hdr_reserve = 4 + sizeof(size_t) * 2;
        arc->next_write_offset = (arc->file_size == 0) ? hdr_reserve : arc->file_size;
    }
    arc->num_sections = 0;

    /* Header-based dir persistence: read magic + dir_offset + dir_size from offset 0 */
    if (arc->file_size >= (4 + sizeof(size_t)*2)) {
        unsigned char hdr[4 + sizeof(size_t)*2] = {0};
        if (cce_archive_read_raw(arc, 0, hdr, sizeof(hdr)) == CCE_OK &&
            memcmp(hdr, "CCE1", 4) == 0) {
            memcpy(&arc->dir_offset, hdr + 4, sizeof(size_t));
            memcpy(&arc->dir_blob_size, hdr + 4 + sizeof(size_t), sizeof(size_t));
        }
    }

    cce_archive_load_dir_if_present(arc);

    *arc_out = arc;
    return CCE_OK;
}

void cce_archive_close(cce_archive* arc) {
    if (!arc) return;

    /* Persist the directory so reopen can recover sections (last __DIR__ wins) */
    cce_archive_flush_dir(arc);

#ifdef _WIN32
    if (arc->mapped) UnmapViewOfFile(arc->mapped);
    if (arc->map_handle) CloseHandle(arc->map_handle);
    if (arc->file_handle != INVALID_HANDLE_VALUE) CloseHandle(arc->file_handle);
#else
    if (arc->f) fclose(arc->f);
#endif
    free(arc);
}

cce_result cce_archive_get_tensor_view(cce_archive* arc,
                                       size_t section_offset,
                                       const int* shape, int ndim,
                                       cce_tensor* out_view) {
    if (!arc || !out_view || !shape) return CCE_ERR_INVALID_ARG;

    size_t total = 1;
    for (int i = 0; i < ndim; ++i) total *= (size_t)shape[i];
    size_t bytes_needed = total * sizeof(float);

    if (section_offset + bytes_needed > arc->file_size) return CCE_ERR_INVALID_ARG;

#ifdef _WIN32
    if (!arc->mapped) {
        /* No mapping yet (new/empty file) - fall back to alloc+read */
        cce_result r = cce_tensor_alloc(out_view, shape, ndim);
        if (r != CCE_OK) return r;

        LARGE_INTEGER li; li.QuadPart = section_offset;
        SetFilePointerEx(arc->file_handle, li, NULL, FILE_BEGIN);

        DWORD bytes_read = 0;
        if (!ReadFile(arc->file_handle, out_view->data, (DWORD)bytes_needed, &bytes_read, NULL) ||
            bytes_read != bytes_needed) {
            cce_tensor_free(out_view);
            return CCE_ERR_IO;
        }
        return CCE_OK;
    }

    float* ptr = (float*)((char*)arc->mapped + section_offset);
    cce_result r = cce_tensor_view(out_view, ptr, shape, ndim, NULL);
    if (r == CCE_OK) out_view->owns_memory = 0;
    return r;
#else
    cce_result r = cce_tensor_alloc(out_view, shape, ndim);
    if (r != CCE_OK) return r;

    fseek(arc->f, (long)section_offset, SEEK_SET);
    size_t read = fread(out_view->data, 1, bytes_needed, arc->f);
    if (read != bytes_needed) {
        cce_tensor_free(out_view);
        return CCE_ERR_IO;
    }
    return CCE_OK;
#endif
}

cce_result cce_archive_append_section(cce_archive* arc,
                                      const char* name,
                                      const void* data, size_t size,
                                      size_t* out_offset) {
    if (!arc || !data || size == 0) return CCE_ERR_INVALID_ARG;

#ifdef _WIN32
    size_t new_size = arc->next_write_offset + size;

    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)new_size;

    /* Extend file */
    if (!SetFilePointerEx(arc->file_handle, li, NULL, FILE_BEGIN) ||
        !SetEndOfFile(arc->file_handle)) {
        return CCE_ERR_IO;
    }

    /* Remap the larger file */
    if (arc->mapped) { UnmapViewOfFile(arc->mapped); arc->mapped = NULL; }
    if (arc->map_handle) { CloseHandle(arc->map_handle); arc->map_handle = NULL; }

    arc->map_handle = CreateFileMappingA(
        arc->file_handle, NULL, PAGE_READWRITE, 0, 0, NULL
    );
    if (!arc->map_handle) return CCE_ERR_IO;

    arc->mapped = MapViewOfFile(arc->map_handle, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!arc->mapped) return CCE_ERR_IO;

    /* Write the data into the view */
    memcpy((char*)arc->mapped + arc->next_write_offset, data, size);

    if (arc->num_sections < CCE_ARCHIVE_MAX_SECTIONS) {
        int s = arc->num_sections++;
        strncpy(arc->dir[s].name, name ? name : "unnamed", 63);
        arc->dir[s].offset = arc->next_write_offset;
        arc->dir[s].size = size;
        arc->dir[s].flags = 0;
    }

    if (out_offset) *out_offset = arc->next_write_offset;
    arc->next_write_offset = new_size;
    arc->file_size = new_size;

    return CCE_OK;
#else
    fseek(arc->f, (long)arc->next_write_offset, SEEK_SET);
    if (fwrite(data, 1, size, arc->f) != size) return CCE_ERR_IO;

    if (arc->num_sections < CCE_ARCHIVE_MAX_SECTIONS) {
        int s = arc->num_sections++;
        strncpy(arc->dir[s].name, name ? name : "unnamed", 63);
        arc->dir[s].offset = arc->next_write_offset;
        arc->dir[s].size = size;
    }

    if (out_offset) *out_offset = arc->next_write_offset;
    arc->next_write_offset += size;
    return CCE_OK;
#endif
}

cce_result cce_archive_find_section(cce_archive* arc, const char* name,
                                    size_t* out_offset, size_t* out_size) {
    if (!arc || !name) return CCE_ERR_INVALID_ARG;
    for (int i = 0; i < arc->num_sections; ++i) {
        if (strncmp(arc->dir[i].name, name, 63) == 0) {
            if (out_offset) *out_offset = arc->dir[i].offset;
            if (out_size) *out_size = arc->dir[i].size;
            return CCE_OK;
        }
    }
    return CCE_ERR_NOT_FOUND;
}

cce_result cce_archive_read_raw(cce_archive* arc, size_t offset, void* buf, size_t len) {
    if (!arc || !buf || len == 0) return CCE_ERR_INVALID_ARG;
    if (offset + len > arc->file_size) return CCE_ERR_INVALID_ARG;

#ifdef _WIN32
    if (arc->mapped) {
        memcpy(buf, (char*)arc->mapped + offset, len);
        return CCE_OK;
    }
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)offset;
    SetFilePointerEx(arc->file_handle, li, NULL, FILE_BEGIN);
    DWORD br = 0;
    if (!ReadFile(arc->file_handle, buf, (DWORD)len, &br, NULL) || br != (DWORD)len) return CCE_ERR_IO;
    return CCE_OK;
#else
    fseek(arc->f, (long)offset, SEEK_SET);
    size_t r = fread(buf, 1, len, arc->f);
    return (r == len) ? CCE_OK : CCE_ERR_IO;
#endif
}

/* Dir persist helpers: write a compact __DIR__ section on close/flush.
   Format: int32 num_sections followed by packed entries (name64 + offset + size + flags32)
   On open we find the *last* __DIR__ and repopulate the RAM dir. */

static void cce_archive_write_dir_section(cce_archive* arc) {
    if (!arc || arc->num_sections <= 0) return;

    /* Build compact blob */
    size_t blob_size = sizeof(int) + (size_t)arc->num_sections * (64 + sizeof(size_t)*2 + sizeof(uint32_t));
    unsigned char* blob = (unsigned char*)calloc(1, blob_size);
    if (!blob) return;

    int num = arc->num_sections;
    memcpy(blob, &num, sizeof(int));
    size_t pos = sizeof(int);
    for (int i = 0; i < num; ++i) {
        memcpy(blob + pos, arc->dir[i].name, 64); pos += 64;
        memcpy(blob + pos, &arc->dir[i].offset, sizeof(size_t)); pos += sizeof(size_t);
        memcpy(blob + pos, &arc->dir[i].size, sizeof(size_t)); pos += sizeof(size_t);
        memcpy(blob + pos, &arc->dir[i].flags, sizeof(uint32_t)); pos += sizeof(uint32_t);
    }

    /* Better atomicity: always append the new dir blob at current end (after data),
       then update the header at offset 0 to point to it.
       This way old dirs are left but header always points to latest.
       On Windows with mapped, we flush after header update. */
    size_t dummy_off = 0;
    cce_result rc = cce_archive_append_section(arc, "__DIR__", blob, blob_size, &dummy_off);
    if (rc != CCE_OK) {
        free(blob);
        return;
    }
    arc->dir_offset = dummy_off;
    arc->dir_blob_size = blob_size;

    /* Write header LAST for better atomicity */
    unsigned char hdr[4 + sizeof(size_t)*2];
    memcpy(hdr, "CCE1", 4);
    memcpy(hdr + 4, &arc->dir_offset, sizeof(size_t));
    memcpy(hdr + 4 + sizeof(size_t), &arc->dir_blob_size, sizeof(size_t));

#ifdef _WIN32
    LARGE_INTEGER li; li.QuadPart = 0;
    SetFilePointerEx(arc->file_handle, li, NULL, FILE_BEGIN);
    DWORD written = 0;
    WriteFile(arc->file_handle, hdr, sizeof(hdr), &written, NULL);
    if (arc->mapped) {
        /* remap or copy to mapped view if possible */
        memcpy((char*)arc->mapped, hdr, sizeof(hdr));
    }
    FlushFileBuffers(arc->file_handle);  /* attempt atomic-ish */
#else
    fseek(arc->f, 0, SEEK_SET);
    fwrite(hdr, 1, sizeof(hdr), arc->f);
    fflush(arc->f);
    /* fsync( fileno(arc->f) ); if available */
#endif

    free(blob);
}

void cce_archive_load_dir_if_present(cce_archive* arc) {
    if (!arc) return;

    size_t dir_off = arc->dir_offset;
    size_t dir_sz = arc->dir_blob_size;

    if (dir_off == 0 || dir_sz < sizeof(int)) {
        /* Fallback to scanning for __DIR__ appended section */
        if (cce_archive_find_section(arc, "__DIR__", &dir_off, &dir_sz) != CCE_OK || dir_sz < sizeof(int)) {
            return; /* no persisted dir yet */
        }
    }

    unsigned char* buf = (unsigned char*)malloc(dir_sz);
    if (!buf) return;

    if (cce_archive_read_raw(arc, dir_off, buf, dir_sz) != CCE_OK) {
        free(buf);
        return;
    }

    int num = 0;
    memcpy(&num, buf, sizeof(int));
    if (num > 0 && num <= CCE_ARCHIVE_MAX_SECTIONS) {
        arc->num_sections = 0;
        size_t pos = sizeof(int);
        for (int i = 0; i < num; ++i) {
            if (pos + 64 + sizeof(size_t)*2 + sizeof(uint32_t) > dir_sz) break;
            if (arc->num_sections >= CCE_ARCHIVE_MAX_SECTIONS) break;
            memcpy(arc->dir[i].name, buf + pos, 64); pos += 64;
            memcpy(&arc->dir[i].offset, buf + pos, sizeof(size_t)); pos += sizeof(size_t);
            memcpy(&arc->dir[i].size, buf + pos, sizeof(size_t)); pos += sizeof(size_t);
            memcpy(&arc->dir[i].flags, buf + pos, sizeof(uint32_t)); pos += sizeof(uint32_t);
            /* basic validation */
            if (arc->dir[i].size > 0 && arc->dir[i].offset < arc->file_size) {
                arc->num_sections++;
            }
        }
    }
    free(buf);
}

void cce_archive_flush_dir(cce_archive* arc) {
    if (arc) cce_archive_write_dir_section(arc);
}

