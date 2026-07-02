#include "../include/cce/cce_archive.h"
#include "../include/cce/cce_tensor.h"
#include <stdio.h>

int main(void) {
    printf("CCE Archive Test\n");

    const char* fname = "test_archive.cce";
    remove(fname);

    cce_archive* ar = NULL;
    if (cce_archive_open(&ar, fname) != CCE_OK) {
        printf("Open failed\n");
        return 1;
    }

    float data[4] = {1.1f, 2.2f, 3.3f, 4.4f};
    size_t off = 0;
    if (cce_archive_append_section(ar, "testsec", data, sizeof(data), &off) != CCE_OK) {
        printf("Append failed\n");
        return 1;
    }
    printf("Appended at offset %zu\n", off);

    size_t foff = 0, fsz = 0;
    if (cce_archive_find_section(ar, "testsec", &foff, &fsz) == CCE_OK) {
        printf("Found section at %zu size %zu\n", foff, fsz);
    }

    int shape[1] = {4};
    cce_tensor v;
    if (cce_archive_get_tensor_view(ar, off, shape, 1, &v) == CCE_OK) {
        printf("View OK, owns=%d, data[0]=%.1f\n", v.owns_memory, v.data[0]);
        cce_tensor_free(&v);
    }

    cce_archive_close(ar);
    remove(fname);

    printf("Archive Test: SUCCESS\n");
    return 0;
}
