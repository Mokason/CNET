#include "cnet_compete_client_identity.h"

#include "cnet_compete_eval.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

typedef struct {
    const char *path;
    const char *sha256;
} ClientRuntimeFile;

typedef struct {
    const ClientRuntimeFile *file;
    int descriptor;
    dev_t device;
    ino_t inode;
    off_t bytes;
    struct timespec modified;
    struct timespec changed;
} ClientRuntimeGuard;

#define CLIENT_RUNTIME_GUARD_MAX 32u
static ClientRuntimeGuard runtime_guards[CLIENT_RUNTIME_GUARD_MAX];
static size_t runtime_guard_count;
static int initialized_runtime_kind = -1;
static dev_t self_device;
static ino_t self_inode;
static int release_lock_descriptor = -1;

static const ClientRuntimeFile common_files[] = {
    {"/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2", "cd4df4f3c7b83673d61189bf2eaebd33ca4f2853ab9772b8a25e025ef99b1e81"},
    {"/usr/lib/x86_64-linux-gnu/libc.so.6", "8db37cf3f2169f59a0f07ef1fea308c35656668c64c8ff294e1860f4121eb161"}
};

static const ClientRuntimeFile math_file = {
    "/usr/lib/x86_64-linux-gnu/libm.so.6",
    "e9c4b28d340e415b8137480ec442662f981e1399386c5931dae0e886e3639e91"
};

static const ClientRuntimeFile curl_files[] = {
    {"/usr/lib/x86_64-linux-gnu/libbrotlicommon.so.1.1.0", "a91ead095d2c80520c55a89057bbe10b031a075340442e63f44b310f93883a1b"},
    {"/usr/lib/x86_64-linux-gnu/libbrotlidec.so.1.1.0", "64d8a5019d4c294b89fde1193343ea324bbd8603652554e5545f0a01595fa2c5"},
    {"/usr/lib/x86_64-linux-gnu/libcom_err.so.2.1", "022943b3b11c860b049bce41342f1c2594941b7b401d95dfdf235521099fee08"},
    {"/usr/lib/x86_64-linux-gnu/libcrypto.so.3", "e35b898aeda0c9ae473810a3b6935d8ef2d4b91396dc018acbc52638363471ab"},
    {"/usr/lib/x86_64-linux-gnu/libcurl.so.4.8.0", "4102f6dff5aaa48566b74f5b2966ba054e6478f6d447cd1e50f4b00b3a7a49f4"},
    {"/usr/lib/x86_64-linux-gnu/libffi.so.8.1.4", "00f593fe192f2851b8ce23b25cec2488d769beb5a8f63e8c9e563071e1075153"},
    {"/usr/lib/x86_64-linux-gnu/libgmp.so.10.5.0", "0ccdfb6d6f5c039465f6d002cf7e4c072d48ac6a2cffc8dd6c748dec31592804"},
    {"/usr/lib/x86_64-linux-gnu/libgnutls.so.30.37.1", "ddbae2995750875c07bc218d12a062d73f8250678f54dedda7e9be5068781c98"},
    {"/usr/lib/x86_64-linux-gnu/libgssapi_krb5.so.2.2", "8d0b00978cde2a5a9ec76bee1e16a80efd502bc4db8647174a00a61c9d90f989"},
    {"/usr/lib/x86_64-linux-gnu/libhogweed.so.6.8", "c3c606139d4f7776efa4dbd4a6b62e30c916b07a0cafd8c3ef173d6f093607e3"},
    {"/usr/lib/x86_64-linux-gnu/libidn2.so.0.4.0", "6e632fd9a3125db60bbc623719bbc17b0470330321c5b1c57f1ff7190e5551ad"},
    {"/usr/lib/x86_64-linux-gnu/libk5crypto.so.3.1", "dd8b93b95adf53a638772061dbe81dc0252d2d8857ae1df774aed78c6a79e0c0"},
    {"/usr/lib/x86_64-linux-gnu/libkeyutils.so.1.10", "f48214417757f18793ed6e180cc14ee1d6f04252a518fc7270e8ca1d0b4260fe"},
    {"/usr/lib/x86_64-linux-gnu/libkrb5.so.3.3", "8cd11f2aa69efdac7c040a2794f8bf1a737405abff53908b737c721d1c4ba65d"},
    {"/usr/lib/x86_64-linux-gnu/libkrb5support.so.0.1", "8253bcdf7f39fcf8f25e797757808e671d777efc00b0cd1fbbcd9da61b0e98f7"},
    {"/usr/lib/x86_64-linux-gnu/liblber.so.2.0.200", "ddf14275b1fb70ba06510094071adcc1462e675f15ae6cdcadbf40c1dabe5a03"},
    {"/usr/lib/x86_64-linux-gnu/libldap.so.2.0.200", "a9829b999c95452eb0a34cc7b5349427b76dbf3e5d5c8672a5ca69e454c1b151"},
    {"/usr/lib/x86_64-linux-gnu/libnettle.so.8.8", "a245ed916229583d6fe0b07657b3ed945bc148c28cc08b5266295f620d2b91cc"},
    {"/usr/lib/x86_64-linux-gnu/libnghttp2.so.14.26.0", "46764ab5b6ca7e353a322054f0bee45d9158a27f3e6a82a2fa4447f2ba9ba268"},
    {"/usr/lib/x86_64-linux-gnu/libp11-kit.so.0.3.1", "c6b469a006685ebf2075e200180c3b21f41b88ad722c5697d736ea558cfe3ea1"},
    {"/usr/lib/x86_64-linux-gnu/libpsl.so.5.3.4", "ae6c5a9aecc249fc66e03c16ec6c4385d09d7e12e0ea7694bb831ab4fee8133c"},
    {"/usr/lib/x86_64-linux-gnu/libresolv.so.2", "4086b12d3b95ef6f02373b30b88444e7d79fb8693ed1cfeaa5162e6959878912"},
    {"/usr/lib/x86_64-linux-gnu/librtmp.so.1", "15a2a75e8c437b9c99971b5ee10278ab110beef1a56d1d9e5d6759ea140e98ac"},
    {"/usr/lib/x86_64-linux-gnu/libsasl2.so.2.0.25", "a0f410ad093236856eee59c4ff2838dc04d932ea0993400f23ed77d67d85af34"},
    {"/usr/lib/x86_64-linux-gnu/libssh.so.4.9.6", "ad1513debc90c2662c2c63b587cd815b118b03b3a983f7fdbd89968354674d63"},
    {"/usr/lib/x86_64-linux-gnu/libssl.so.3", "2f4b14490bdf3e1d4f5d4a9c0e624b95e2074ab40380e7d545aa1eb709584ce6"},
    {"/usr/lib/x86_64-linux-gnu/libtasn1.so.6.6.3", "59994414d0cdec4f678ef91eaaafc44b26b00bca507e4cc7978caaf79f584173"},
    {"/usr/lib/x86_64-linux-gnu/libunistring.so.5.0.0", "7bcf825cd449892db5105ad2aed91dcf94a27b796426600c765720c584c54dd1"},
    {"/usr/lib/x86_64-linux-gnu/libz.so.1.3", "9b64150b28505a33d6bc3ecf709c279f6de97a1c184dbda65d06ee4537f6d286"},
    {"/usr/lib/x86_64-linux-gnu/libzstd.so.1.5.5", "0a2128bc10841fb29e76d08d945864dfb0b6a66da5df6df5d8299197439e54bb"}
};

int cnet_compete_client_environment_matches(void) {
    extern char **environ;
    static const char *const expected[] = {
        "PATH=/usr/bin:/bin", "LANG=C", "LC_ALL=C", "TZ=UTC"
    };
    char **cursor;
    unsigned seen = 0;
    size_t entry;
    if (environ == NULL) return 0;
    for (cursor = environ; *cursor != NULL; ++cursor) {
        int matched = 0;
        for (entry = 0; entry < sizeof expected / sizeof expected[0]; ++entry) {
            if (strcmp(*cursor, expected[entry]) == 0) {
                if ((seen & (1u << entry)) != 0) return 0;
                seen |= 1u << entry;
                matched = 1;
                break;
            }
        }
        if (!matched) return 0;
    }
    return seen == (1u << (sizeof expected / sizeof expected[0])) - 1u;
}

int cnet_compete_client_release_lock_acquire(void) {
    struct stat parent_status, descriptor_status, path_status;
    int descriptor;
    if (release_lock_descriptor >= 0) {
        return fstat(release_lock_descriptor, &descriptor_status) == 0 &&
               lstat("/home/marble/.local/state/cnet/.release.lock",
                     &path_status) == 0 &&
               S_ISREG(descriptor_status.st_mode) &&
               S_ISREG(path_status.st_mode) &&
               descriptor_status.st_uid == geteuid() &&
               path_status.st_uid == geteuid() &&
               descriptor_status.st_nlink == 1 &&
               path_status.st_nlink == 1 &&
               (descriptor_status.st_mode & 0777) == 0600 &&
               (path_status.st_mode & 0777) == 0600 &&
               descriptor_status.st_dev == path_status.st_dev &&
               descriptor_status.st_ino == path_status.st_ino;
    }
    if (lstat("/home/marble/.local/state/cnet", &parent_status) != 0 ||
        !S_ISDIR(parent_status.st_mode) ||
        parent_status.st_uid != geteuid() ||
        (parent_status.st_mode & 0777) != 0700)
        return 0;
    descriptor = open("/home/marble/.local/state/cnet/.release.lock",
                      O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0 || fstat(descriptor, &descriptor_status) != 0 ||
        lstat("/home/marble/.local/state/cnet/.release.lock",
              &path_status) != 0 ||
        !S_ISREG(descriptor_status.st_mode) ||
        !S_ISREG(path_status.st_mode) ||
        descriptor_status.st_uid != geteuid() ||
        path_status.st_uid != geteuid() ||
        descriptor_status.st_nlink != 1 || path_status.st_nlink != 1 ||
        (descriptor_status.st_mode & 0777) != 0600 ||
        (path_status.st_mode & 0777) != 0600 ||
        descriptor_status.st_dev != path_status.st_dev ||
        descriptor_status.st_ino != path_status.st_ino ||
        flock(descriptor, LOCK_SH | LOCK_NB) != 0) {
        if (descriptor >= 0) (void)close(descriptor);
        return 0;
    }
    release_lock_descriptor = descriptor;
    return 1;
}

static const ClientRuntimeFile *selected_file(int runtime_kind, size_t index) {
    size_t common_count = sizeof common_files / sizeof common_files[0];
    if (index < common_count) return &common_files[index];
    index -= common_count;
    if (runtime_kind == CNET_COMPETE_CLIENT_FIXTURE)
        return index == 0 ? &math_file : NULL;
    if (runtime_kind == CNET_COMPETE_CLIENT_BASELINE)
        return index < sizeof curl_files / sizeof curl_files[0]
                   ? &curl_files[index] : NULL;
    return NULL;
}

static int mapped_identity_seen(dev_t device, ino_t inode,
                                const char *expected_path) {
    char line[4096], resolved[4096];
    FILE *maps = fopen("/proc/self/maps", "rb");
    if (maps == NULL) return 0;
    while (fgets(line, sizeof line, maps) != NULL) {
        unsigned major_number = 0, minor_number = 0;
        unsigned long long mapped_inode = 0;
        char *path = strchr(line, '/');
        if (path == NULL) continue;
        path[strcspn(path, "\r\n")] = '\0';
        if (sscanf(line, "%*s %*s %*s %x:%x %llu", &major_number,
                   &minor_number, &mapped_inode) != 3 ||
            major_number != major(device) || minor_number != minor(device) ||
            mapped_inode != (unsigned long long)inode)
            continue;
        if (realpath(path, resolved) != NULL &&
            strcmp(resolved, expected_path) == 0) {
            (void)fclose(maps);
            return 1;
        }
    }
    (void)fclose(maps);
    return 0;
}

static int timestamp_equal(struct timespec left, struct timespec right) {
    return left.tv_sec == right.tv_sec && left.tv_nsec == right.tv_nsec;
}

static int guard_shape_matches(const ClientRuntimeGuard *guard) {
    struct stat descriptor_status, path_status;
    return guard != NULL && guard->file != NULL && guard->descriptor >= 0 &&
           fstat(guard->descriptor, &descriptor_status) == 0 &&
           lstat(guard->file->path, &path_status) == 0 &&
           S_ISREG(descriptor_status.st_mode) && S_ISREG(path_status.st_mode) &&
           descriptor_status.st_dev == guard->device &&
           descriptor_status.st_ino == guard->inode &&
           descriptor_status.st_size == guard->bytes &&
           path_status.st_dev == guard->device &&
           path_status.st_ino == guard->inode &&
           path_status.st_size == guard->bytes &&
           timestamp_equal(descriptor_status.st_mtim, guard->modified) &&
           timestamp_equal(descriptor_status.st_ctim, guard->changed) &&
           timestamp_equal(path_status.st_mtim, guard->modified) &&
           timestamp_equal(path_status.st_ctim, guard->changed);
}

static int guard_bytes_match(const ClientRuntimeGuard *guard) {
    char descriptor_path[64], actual[65];
    return guard_shape_matches(guard) &&
           snprintf(descriptor_path, sizeof descriptor_path,
                    "/proc/self/fd/%d", guard->descriptor) <
               (int)sizeof descriptor_path &&
           cnet_compete_eval_file_sha256(descriptor_path, actual) == 0 &&
           strcmp(actual, guard->file->sha256) == 0;
}

static void close_guards(void) {
    size_t index;
    for (index = 0; index < runtime_guard_count; ++index) {
        if (runtime_guards[index].descriptor >= 0)
            (void)close(runtime_guards[index].descriptor);
        runtime_guards[index].descriptor = -1;
    }
    runtime_guard_count = 0;
    initialized_runtime_kind = -1;
    self_device = 0;
    self_inode = 0;
}

static int initialize_guards(int runtime_kind) {
    const ClientRuntimeFile *file;
    struct stat self_status;
    int self_descriptor = -1;
    size_t index;
    close_guards();
    self_descriptor = open("/proc/self/exe", O_RDONLY);
    if (self_descriptor < 0 || fstat(self_descriptor, &self_status) != 0 ||
        !S_ISREG(self_status.st_mode) || close(self_descriptor) != 0)
        goto fail;
    self_descriptor = -1;
    self_device = self_status.st_dev;
    self_inode = self_status.st_ino;
    for (index = 0; index < CLIENT_RUNTIME_GUARD_MAX; ++index)
        runtime_guards[index].descriptor = -1;
    for (index = 0; (file = selected_file(runtime_kind, index)) != NULL;
         ++index) {
        ClientRuntimeGuard *guard;
        struct stat status;
        if (index >= CLIENT_RUNTIME_GUARD_MAX) goto fail;
        guard = &runtime_guards[index];
        guard->file = file;
        guard->descriptor = open(file->path, O_RDONLY | O_NOFOLLOW);
        runtime_guard_count = index + 1u;
        if (guard->descriptor < 0 ||
            flock(guard->descriptor, LOCK_SH | LOCK_NB) != 0 ||
            fstat(guard->descriptor, &status) != 0 ||
            !S_ISREG(status.st_mode) || status.st_size <= 0)
            goto fail;
        guard->device = status.st_dev;
        guard->inode = status.st_ino;
        guard->bytes = status.st_size;
        guard->modified = status.st_mtim;
        guard->changed = status.st_ctim;
        if (!guard_bytes_match(guard) ||
            !mapped_identity_seen(guard->device, guard->inode,
                                  guard->file->path))
            goto fail;
    }
    if (runtime_guard_count == 0) goto fail;
    initialized_runtime_kind = runtime_kind;
    return 1;
fail:
    if (self_descriptor >= 0) (void)close(self_descriptor);
    close_guards();
    return 0;
}

static int executable_mapping_expected(dev_t device, ino_t inode) {
    size_t index;
    if (device == self_device && inode == self_inode) return 1;
    for (index = 0; index < runtime_guard_count; ++index)
        if (runtime_guards[index].device == device &&
            runtime_guards[index].inode == inode)
            return 1;
    return 0;
}

static int no_unexpected_executable_mapping(void) {
    char line[4096];
    FILE *maps = fopen("/proc/self/maps", "rb");
    if (maps == NULL) return 0;
    while (fgets(line, sizeof line, maps) != NULL) {
        char address[64], permissions[8], offset[32], device_text[32];
        char inode_text[32], path[3072] = "";
        unsigned major_number, minor_number;
        unsigned long long mapped_inode;
        char *end = NULL;
        int fields = sscanf(line, "%63s %7s %31s %31s %31s %3071[^\n]",
                            address, permissions, offset, device_text,
                            inode_text, path);
        if (fields < 5 || strlen(permissions) != 4u) goto fail;
        if (strchr(permissions, 'x') == NULL) continue;
        if (fields == 6 &&
            (strcmp(path, "[vdso]") == 0 ||
             strcmp(path, "[vsyscall]") == 0))
            continue;
        if (fields != 6 || sscanf(device_text, "%x:%x", &major_number,
                                  &minor_number) != 2)
            goto fail;
        errno = 0;
        mapped_inode = strtoull(inode_text, &end, 10);
        if (errno != 0 || end == inode_text || *end != '\0' ||
            mapped_inode == 0 ||
            !executable_mapping_expected(
                makedev(major_number, minor_number), (ino_t)mapped_inode))
            goto fail;
    }
    if (ferror(maps) || fclose(maps) != 0) return 0;
    return 1;
fail:
    (void)fclose(maps);
    return 0;
}

int cnet_compete_client_runtime_shape_matches(int runtime_kind) {
    size_t index;
    if (!cnet_compete_client_release_lock_acquire() ||
        runtime_kind < CNET_COMPETE_CLIENT_FIXTURE ||
        runtime_kind > CNET_COMPETE_CLIENT_SCORER ||
        initialized_runtime_kind != runtime_kind)
        return 0;
    for (index = 0; index < runtime_guard_count; ++index)
        if (!guard_shape_matches(&runtime_guards[index]) ||
            !mapped_identity_seen(runtime_guards[index].device,
                                  runtime_guards[index].inode,
                                  runtime_guards[index].file->path))
            return 0;
    (void)runtime_kind;
    return no_unexpected_executable_mapping();
}

int cnet_compete_client_runtime_matches(int runtime_kind) {
    size_t index;
    if ((initialized_runtime_kind == -1 && !initialize_guards(runtime_kind)) ||
        initialized_runtime_kind != runtime_kind ||
        !cnet_compete_client_runtime_shape_matches(runtime_kind))
        return 0;
    for (index = 0; index < runtime_guard_count; ++index)
        if (!guard_bytes_match(&runtime_guards[index])) return 0;
    return 1;
}
