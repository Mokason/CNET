#include "cce/cce_campaign_provenance.h"
#include "cnet_compete_artifacts.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MANIFEST_PATH_MAX 1024

static int path_join(char output[MANIFEST_PATH_MAX], const char *root,
                     const char *leaf) {
    int written = snprintf(output, MANIFEST_PATH_MAX, "%s/%s", root, leaf);
    return written < 0 || written >= MANIFEST_PATH_MAX ? -1 : 0;
}

static int append_text(char **buffer, size_t *length, size_t *capacity,
                       const char *text) {
    size_t add;
    char *expanded;
    if (buffer == NULL || length == NULL || capacity == NULL || text == NULL ||
        *length > *capacity || (*buffer == NULL && (*length != 0 || *capacity != 0)))
        return -1;
    add = strlen(text);
    if (add > SIZE_MAX - *length - 1u) return -1;
    if (*buffer == NULL || *length + add + 1u > *capacity) {
        size_t next = *capacity == 0 ? 4096u : *capacity;
        while (next < *length + add + 1u) {
            if (next > SIZE_MAX / 2u) return -1;
            next *= 2u;
        }
        expanded = (char *)realloc(*buffer, next);
        if (expanded == NULL) return -1;
        *buffer = expanded;
        *capacity = next;
    }
    memcpy(*buffer + *length, text, add);
    *length += add;
    (*buffer)[*length] = '\0';
    return 0;
}

static int regular_size(const char *path, unsigned long long *size_out) {
    struct stat status;
    if (lstat(path, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size <= 0)
        return -1;
    *size_out = (unsigned long long)status.st_size;
    return 0;
}

static int read_exact(const char *path, char **content, size_t *length) {
    struct stat status;
    FILE *file = NULL;
    char *buffer = NULL;
    int rc = -1;
    if (lstat(path, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size <= 0 || (unsigned long long)status.st_size > 1024u * 1024u)
        return -1;
    buffer = (char *)malloc((size_t)status.st_size + 1u);
    file = fopen(path, "rb");
    if (buffer == NULL || file == NULL) goto done;
    if (fread(buffer, 1, (size_t)status.st_size, file) !=
            (size_t)status.st_size || ferror(file))
        goto done;
    if (fclose(file) != 0) { file = NULL; goto done; }
    file = NULL;
    buffer[status.st_size] = '\0';
    *content = buffer;
    *length = (size_t)status.st_size;
    buffer = NULL;
    rc = 0;
done:
    if (file != NULL) (void)fclose(file);
    free(buffer);
    return rc;
}

static int write_all(int file, const char *data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t written = write(file, data + offset, length - offset);
        if (written <= 0) return -1;
        offset += (size_t)written;
    }
    return 0;
}

static int fsync_parent_directory(const char *path) {
    char directory[MANIFEST_PATH_MAX], *slash;
    int descriptor, rc;
    if (path == NULL || strlen(path) >= sizeof directory) return -1;
    memcpy(directory, path, strlen(path) + 1u);
    slash = strrchr(directory, '/');
    if (slash == NULL) {
        memcpy(directory, ".", 2u);
    } else if (slash == directory) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    descriptor = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (descriptor < 0) return -1;
    rc = fsync(descriptor);
    if (close(descriptor) != 0) rc = -1;
    return rc;
}

static int publish_manifest(const char *path, const char *content,
                            size_t length) {
    struct stat status;
    char temporary[MANIFEST_PATH_MAX];
    int file, written;
    if (lstat(path, &status) == 0) {
        char *existing = NULL;
        size_t existing_length = 0;
        int equal = read_exact(path, &existing, &existing_length) == 0 &&
                    existing_length == length &&
                    memcmp(existing, content, length) == 0;
        free(existing);
        return equal && fsync_parent_directory(path) == 0 ? 0 : -1;
    }
    if (errno != ENOENT) return -1;
    written = snprintf(temporary, sizeof temporary, "%s.tmp.%ld", path,
                       (long)getpid());
    if (written < 0 || (size_t)written >= sizeof temporary) return -1;
    file = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (file < 0) return -1;
    if (write_all(file, content, length) != 0 || fsync(file) != 0) {
        (void)close(file);
        (void)unlink(temporary);
        return -1;
    }
    if (close(file) != 0 || rename(temporary, path) != 0 ||
        fsync_parent_directory(path) != 0) {
        (void)unlink(temporary);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    char *manifest = NULL;
    size_t length = 0, capacity = 0, index;
    unsigned long long total_bytes = 0;
    char manifest_sha[65];
    int rc = 1;
    if (argc != 3) {
        fprintf(stderr, "usage: %s ARTIFACT_ROOT MANIFEST\n", argv[0]);
        return 2;
    }
    for (index = 0; index < CNET_COMPETE_ARTIFACT_FILE_COUNT; ++index) {
        char path[MANIFEST_PATH_MAX], hash[65], line[MANIFEST_PATH_MAX + 70];
        unsigned long long bytes;
        int written;
        if (path_join(path, argv[1], cnet_compete_artifact_member(index)) != 0 ||
            regular_size(path, &bytes) != 0 ||
            cce_sha256_file_hex(path, hash) != 0)
            goto done;
        written = snprintf(line, sizeof line, "%s  %s\n", hash, path);
        if (written < 0 || (size_t)written >= sizeof line ||
            append_text(&manifest, &length, &capacity, line) != 0)
            goto done;
        if (ULLONG_MAX - total_bytes < bytes) goto done;
        total_bytes += bytes;
    }
    if (publish_manifest(argv[2], manifest, length) != 0 ||
        cce_sha256_file_hex(argv[2], manifest_sha) != 0)
        goto done;
    if (ULLONG_MAX - total_bytes < length) goto done;
    printf("CNET_7B_ARTIFACT_MANIFEST_PASS files=%zu member_bytes=%llu "
           "manifest_bytes=%zu complete_bytes=%llu sha256=%s\n",
           (size_t)CNET_COMPETE_ARTIFACT_FILE_COUNT, total_bytes, length,
           total_bytes + (unsigned long long)length, manifest_sha);
    rc = 0;
done:
    free(manifest);
    if (rc != 0) printf("CNET_7B_ARTIFACT_MANIFEST_FAIL\n");
    return rc;
}
