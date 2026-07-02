#ifndef CCE_ARCHIVE_H
#define CCE_ARCHIVE_H

#include "cce_defs.h"
#include "cce_tensor.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Simple archive for single-file partial contract loading via mmap/views */
typedef struct cce_archive cce_archive;

cce_result cce_archive_open(cce_archive** arc, const char* path);
void       cce_archive_close(cce_archive* arc);

/* Get a tensor view into a section (zero copy when possible) */
cce_result cce_archive_get_tensor_view(cce_archive* arc,
                                       size_t section_offset,
                                       const int* shape, int ndim,
                                       cce_tensor* out_view);

/* Append a new contract section (for training -> freeze -> store).
   Returns offset of the data section. Directory is updated. */
cce_result cce_archive_append_section(cce_archive* arc,
                                      const char* name,
                                      const void* data, size_t size,
                                      size_t* out_offset);

/* Fast lookup by name (for thousands of contracts) */
cce_result cce_archive_find_section(cce_archive* arc, const char* name,
                                    size_t* out_offset, size_t* out_size);

/* Raw read from archive (portable across Win mmap / posix). For cascade load etc. */
cce_result cce_archive_read_raw(cce_archive* arc, size_t offset, void* buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* CCE_ARCHIVE_H */
