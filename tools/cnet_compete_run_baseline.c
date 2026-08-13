#include "cnet_compete_eval.h"
#include "cnet_compete_client_identity.h"

#include <curl/curl.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define HTTP_RESPONSE_MAX (256u * 1024u)
#define SYSTEM_MAX 16384u

#ifndef CNET_COMPETE_BUILD_COMMIT
#define CNET_COMPETE_BUILD_COMMIT "unknown"
#endif
#ifndef CNET_COMPETE_BUILD_TREE
#define CNET_COMPETE_BUILD_TREE "unknown"
#endif

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    int failed;
} ResponseBuffer;

typedef struct {
    const char *path;
    const char *sha256;
} RuntimeFile;

typedef struct {
    int descriptor;
    dev_t device;
    ino_t inode;
    off_t bytes;
    struct timespec modified;
    struct timespec changed;
} RuntimeGuard;

typedef struct {
    int descriptor;
    dev_t device;
    ino_t inode;
    unsigned long long bytes;
    struct timespec modified;
    struct timespec changed;
    char sha256[65];
} ModelGuard;

static const RuntimeFile runtime_files[] = {
    {CNET_COMPETE_BASELINE_SERVER_EXE,
     CNET_COMPETE_BASELINE_SERVER_SHA256},
    {"/home/marble/AI/llama.cpp-fallback/build-rocm/bin/"
     "libllama-server-impl.so",
     "e13f138e3afdbf4d504cdbc89c54cfc1be722b6719cbf945ef576ddb84f4eec4"},
    {"/home/marble/AI/llama.cpp-fallback/build-rocm/bin/"
     "libllama.so.0.0.1",
     "058482d7aade2dd219a20824687bad46757805f68497853ba4469185a3f0f913"},
    {"/home/marble/AI/llama.cpp-fallback/build-rocm/bin/"
     "libllama-common.so.0.0.1",
     "8319985ce2df46f300f935cdc425b3ff2fae1907cc0ee5fbfc202e209a22aeb2"},
    {"/home/marble/AI/llama.cpp-fallback/build-rocm/bin/"
     "libggml.so.0.17.0",
     "e344a22ae6dff32ed057f93b20c5f9e18d091e8771df1e0803776255d71d93f0"},
    {"/home/marble/AI/llama.cpp-fallback/build-rocm/bin/"
     "libggml-base.so.0.17.0",
     "fd0313260b49fb84fbe6c74f69e32020f0f3930826167d8afbce6ceb1c27a7a9"},
    {"/home/marble/AI/llama.cpp-fallback/build-rocm/bin/"
     "libggml-cpu.so.0.17.0",
     "e1b56faca03415ede8a3ae5f9d9b9d9b0825ed565057fc18083d46859d2c4a37"},
    {"/home/marble/AI/llama.cpp-fallback/build-rocm/bin/"
     "libggml-hip.so.0.17.0",
     "f76c8d1340451d752c981601f5490116f8cefe5d0c621c2e1f96e427c765808d"},
    {"/home/marble/AI/llama.cpp-fallback/build-rocm/bin/"
     "libmtmd.so.0.0.1",
     "d6e98d4c6a296ebce1d8790c7ed678d138dfb420d2a8c77c5865bf0aa86e5f7f"},
    {"/opt/amdgpu/lib/x86_64-linux-gnu/libdrm.so.2.125.0",
     "02e4fdda794e669129f373ec51abae9f1afbdeedc84779171ed5636f5831a9ca"},
    {"/opt/amdgpu/lib/x86_64-linux-gnu/libdrm_amdgpu.so.1.125.0",
     "46eb202ce5452f63b510cdf92ee1eb769e6b5ede1c2719b56c0f3b6d7d316ac5"},
    {"/opt/rocm-7.2.3/lib/libamd_comgr.so.3.0.0",
     "cbae40b0e4fdeb6c1dab7d2b187260bcc9f402e37de2f819044648c4bd459605"},
    {"/opt/rocm-7.2.3/lib/libamdhip64.so.7.2.70203",
     "770eb323b8e2c48514fde3e2b319985b37f8ebc8e6593f5c05487a8a7a7684ac"},
    {"/opt/rocm-7.2.3/lib/libhipblas.so.3.2.70203",
     "dc17f16ae4b6264bc279329acdb8a1c3afa09217e85713b53d287a3ac992c192"},
    {"/opt/rocm-7.2.3/lib/libhipblaslt.so.1.2.70203",
     "143a4d10017e0bc175f81250aebeae23a36352fca680daa273c59932292189ba"},
    {"/opt/rocm-7.2.3/lib/libhsa-runtime64.so.1.18.70203",
     "7d2149819a73eee2b857068332e9fad8925924eeb8dc68ccefcb199ab1b2d1f7"},
    {"/opt/rocm-7.2.3/lib/librocblas.so.5.2.70203",
     "1f10207869e8d7325ddbc0e6b4a7ac42b385113252bb0e08c04caf1880e56257"},
    {"/opt/rocm-7.2.3/lib/librocprofiler-register.so.0.6.0",
     "1a23a2b5a62dc7feee9259b9a307bca94caef7cfbaebc7b1b20a0289ed7e69f0"},
    {"/opt/rocm-7.2.3/lib/librocroller.so.1.0.0",
     "ecb1f6a8eb1dc854b952cb87d17c09c4921c858ff1cb244a5be85dd6ace649f3"},
    {"/opt/rocm-7.2.3/lib/librocsolver.so.0.7.70203",
     "bcb9a916416af0c3625894fe8a27f28169870db50a2b6b3c9587d16ab7fa2df2"},
    {"/opt/rocm-7.2.3/lib/libroctx64.so.4.1.70203",
     "22bbc6946fdf5d7d8b1755cbd738c42a63f3795d18ac3ed1285b09cc772dee17"},
    {"/usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
     "cd4df4f3c7b83673d61189bf2eaebd33ca4f2853ab9772b8a25e025ef99b1e81"},
    {"/usr/lib/x86_64-linux-gnu/libc.so.6",
     "8db37cf3f2169f59a0f07ef1fea308c35656668c64c8ff294e1860f4121eb161"},
    {"/usr/lib/x86_64-linux-gnu/libcrypto.so.3",
     "e35b898aeda0c9ae473810a3b6935d8ef2d4b91396dc018acbc52638363471ab"},
    {"/usr/lib/x86_64-linux-gnu/libelf-0.190.so",
     "248c989360783cdc52f3659921de852c2efcc2097253ce58e34443f856412814"},
    {"/usr/lib/x86_64-linux-gnu/libgcc_s.so.1",
     "d93224d2b0dab4247598be683adca02f5cf00586f99c187579cd7e92058fb7cb"},
    {"/usr/lib/x86_64-linux-gnu/libgomp.so.1.0.0",
     "135f3c8f006d2fe5e68e51281c7974cb991a03de3bfb3593d68d174dfcf854d1"},
    {"/usr/lib/x86_64-linux-gnu/libm.so.6",
     "e9c4b28d340e415b8137480ec442662f981e1399386c5931dae0e886e3639e91"},
    {"/usr/lib/x86_64-linux-gnu/libnuma.so.1.0.0",
     "02d7582c5d391e460e56aa67a414360e3183b968206645b9123f9dc7bff5d009"},
    {"/usr/lib/x86_64-linux-gnu/libssl.so.3",
     "2f4b14490bdf3e1d4f5d4a9c0e624b95e2074ab40380e7d545aa1eb709584ce6"},
    {"/usr/lib/x86_64-linux-gnu/libstdc++.so.6.0.33",
     "1fd75fe70354a416d75aef22bcae68c47bd25d20e2d0568c30b1a9838cf62f11"},
    {"/usr/lib/x86_64-linux-gnu/libz.so.1.3",
     "9b64150b28505a33d6bc3ecf709c279f6de97a1c184dbda65d06ee4537f6d286"},
    {"/usr/lib/x86_64-linux-gnu/libzstd.so.1.5.5",
     "0a2128bc10841fb29e76d08d945864dfb0b6a66da5df6df5d8299197439e54bb"}
};

#define RUNTIME_FILE_COUNT \
    (sizeof runtime_files / sizeof runtime_files[0])
static RuntimeGuard runtime_guards[RUNTIME_FILE_COUNT];

static int timestamp_equal(struct timespec left, struct timespec right) {
    return left.tv_sec == right.tv_sec && left.tv_nsec == right.tv_nsec;
}

static void runtime_guards_close(void) {
    size_t index;
    for (index = 0; index < RUNTIME_FILE_COUNT; ++index) {
        if (runtime_guards[index].descriptor >= 0)
            (void)close(runtime_guards[index].descriptor);
        runtime_guards[index].descriptor = -1;
    }
}

static int runtime_guards_open(void) {
    size_t index;
    for (index = 0; index < RUNTIME_FILE_COUNT; ++index)
        runtime_guards[index].descriptor = -1;
    for (index = 0; index < RUNTIME_FILE_COUNT; ++index) {
        struct stat status;
        char descriptor_path[64], actual[65];
        RuntimeGuard *guard = &runtime_guards[index];
        guard->descriptor = open(runtime_files[index].path,
                                 O_RDONLY | O_NOFOLLOW);
        if (guard->descriptor < 0 ||
            flock(guard->descriptor, LOCK_SH | LOCK_NB) != 0 ||
            fstat(guard->descriptor, &status) != 0 ||
            !S_ISREG(status.st_mode) || status.st_size <= 0 ||
            snprintf(descriptor_path, sizeof descriptor_path,
                     "/proc/self/fd/%d", guard->descriptor) >=
                (int)sizeof descriptor_path ||
            cnet_compete_eval_file_sha256(descriptor_path, actual) != 0 ||
            strcmp(actual, runtime_files[index].sha256) != 0) {
            runtime_guards_close();
            return -1;
        }
        guard->device = status.st_dev;
        guard->inode = status.st_ino;
        guard->bytes = status.st_size;
        guard->modified = status.st_mtim;
        guard->changed = status.st_ctim;
    }
    return 0;
}

static int runtime_guard_shape_matches(size_t index) {
    struct stat descriptor_status, path_status;
    const RuntimeGuard *guard;
    if (index >= RUNTIME_FILE_COUNT) return 0;
    guard = &runtime_guards[index];
    return guard->descriptor >= 0 &&
           fstat(guard->descriptor, &descriptor_status) == 0 &&
           lstat(runtime_files[index].path, &path_status) == 0 &&
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

static int runtime_guards_shape_match(void) {
    size_t index;
    for (index = 0; index < RUNTIME_FILE_COUNT; ++index)
        if (!runtime_guard_shape_matches(index)) return 0;
    return 1;
}

static int runtime_guards_verify(void) {
    size_t index;
    for (index = 0; index < RUNTIME_FILE_COUNT; ++index) {
        char descriptor_path[64], actual[65];
        if (!runtime_guard_shape_matches(index) ||
            snprintf(descriptor_path, sizeof descriptor_path,
                     "/proc/self/fd/%d", runtime_guards[index].descriptor) >=
                (int)sizeof descriptor_path ||
            cnet_compete_eval_file_sha256(descriptor_path, actual) != 0 ||
            strcmp(actual, runtime_files[index].sha256) != 0)
            return 0;
    }
    return 1;
}

static int model_guard_open(ModelGuard *guard) {
    struct stat status;
    char descriptor_path[64];
    if (guard == NULL) return -1;
    memset(guard, 0, sizeof *guard);
    guard->descriptor = -1;
    guard->descriptor = open(CNET_COMPETE_BASELINE_MODEL,
                             O_RDONLY | O_NOFOLLOW);
    if (guard->descriptor < 0 ||
        flock(guard->descriptor, LOCK_SH | LOCK_NB) != 0 ||
        fstat(guard->descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size < 0 ||
        snprintf(descriptor_path, sizeof descriptor_path, "/proc/self/fd/%d",
                 guard->descriptor) >= (int)sizeof descriptor_path ||
        cnet_compete_eval_file_sha256(descriptor_path, guard->sha256) != 0 ||
        strcmp(guard->sha256, CNET_COMPETE_BASELINE_SHA256) != 0 ||
        (unsigned long long)status.st_size != CNET_COMPETE_BASELINE_BYTES) {
        if (guard->descriptor >= 0) (void)close(guard->descriptor);
        guard->descriptor = -1;
        return -1;
    }
    guard->device = status.st_dev;
    guard->inode = status.st_ino;
    guard->bytes = (unsigned long long)status.st_size;
    guard->modified = status.st_mtim;
    guard->changed = status.st_ctim;
    return 0;
}

static int model_guard_shape_matches(const ModelGuard *guard) {
    struct stat descriptor_status, path_status;
    return guard != NULL && guard->descriptor >= 0 &&
           fstat(guard->descriptor, &descriptor_status) == 0 &&
           lstat(CNET_COMPETE_BASELINE_MODEL, &path_status) == 0 &&
           S_ISREG(descriptor_status.st_mode) && S_ISREG(path_status.st_mode) &&
           descriptor_status.st_dev == guard->device &&
           descriptor_status.st_ino == guard->inode &&
           path_status.st_dev == guard->device && path_status.st_ino == guard->inode &&
           (unsigned long long)descriptor_status.st_size == guard->bytes &&
           (unsigned long long)path_status.st_size == guard->bytes &&
           timestamp_equal(descriptor_status.st_mtim, guard->modified) &&
           timestamp_equal(descriptor_status.st_ctim, guard->changed) &&
           timestamp_equal(path_status.st_mtim, guard->modified) &&
           timestamp_equal(path_status.st_ctim, guard->changed);
}

static int model_guard_verify(const ModelGuard *guard) {
    char descriptor_path[64], actual[65];
    return model_guard_shape_matches(guard) &&
           snprintf(descriptor_path, sizeof descriptor_path, "/proc/self/fd/%d",
                    guard->descriptor) < (int)sizeof descriptor_path &&
           cnet_compete_eval_file_sha256(descriptor_path, actual) == 0 &&
           strcmp(actual, guard->sha256) == 0;
}

static void model_guard_close(ModelGuard *guard) {
    if (guard == NULL) return;
    if (guard->descriptor >= 0) (void)close(guard->descriptor);
    guard->descriptor = -1;
}

static int server_maps_model(const ModelGuard *guard) {
    char path[64], line[4096], mapped[PATH_MAX];
    FILE *file;
    if (guard == NULL || guard->descriptor < 0 ||
        snprintf(path, sizeof path, "/proc/%d/maps",
                 CNET_COMPETE_BASELINE_SERVER_PID) >= (int)sizeof path)
        return 0;
    file = fopen(path, "rb");
    if (file == NULL) return 0;
    while (fgets(line, sizeof line, file) != NULL) {
        unsigned device_major = 0, device_minor = 0;
        unsigned long long inode = 0;
        mapped[0] = '\0';
        if (sscanf(line, "%*s %*s %*s %x:%x %llu %1023s",
                   &device_major, &device_minor, &inode, mapped) == 4 &&
            device_major == major(guard->device) &&
            device_minor == minor(guard->device) &&
            inode == (unsigned long long)guard->inode &&
            strcmp(mapped, CNET_COMPETE_BASELINE_MODEL) == 0) {
            (void)fclose(file);
            return 1;
        }
    }
    (void)fclose(file);
    return 0;
}

static int read_server_start_ticks(unsigned long long *ticks_out) {
    char path[64], line[4096], *cursor, *end;
    FILE *file;
    int field;
    unsigned long long value = 0;
    if (ticks_out == NULL ||
        snprintf(path, sizeof path, "/proc/%d/stat",
                 CNET_COMPETE_BASELINE_SERVER_PID) >= (int)sizeof path)
        return -1;
    file = fopen(path, "rb");
    if (file == NULL || fgets(line, sizeof line, file) == NULL ||
        ferror(file) || fclose(file) != 0)
        return -1;
    cursor = strrchr(line, ')');
    if (cursor == NULL || cursor[1] != ' ') return -1;
    cursor += 2;
    for (field = 3; field <= 22; ++field) {
        while (*cursor == ' ') ++cursor;
        if (*cursor == '\0' || *cursor == '\n') return -1;
        if (field == 22) {
            char *number_end = NULL;
            value = strtoull(cursor, &number_end, 10);
            if (number_end == cursor ||
                (*number_end != ' ' && *number_end != '\n' &&
                 *number_end != '\0'))
                return -1;
            break;
        }
        end = strchr(cursor, ' ');
        if (end == NULL) return -1;
        cursor = end + 1;
    }
    *ticks_out = value;
    return 0;
}

static int line_file_equals(const char *path, const char *expected) {
    char line[128];
    size_t length;
    FILE *file = fopen(path, "rb");
    if (file == NULL || fgets(line, sizeof line, file) == NULL || ferror(file) ||
        fclose(file) != 0)
        return 0;
    length = strlen(line);
    if (length > 0 && line[length - 1u] == '\n') line[--length] = '\0';
    return strcmp(line, expected) == 0;
}

static int server_environment_matches(void) {
    char path[64], actual[65];
    return snprintf(path, sizeof path, "/proc/%d/environ",
                    CNET_COMPETE_BASELINE_SERVER_PID) < (int)sizeof path &&
           cnet_compete_eval_file_sha256(path, actual) == 0 &&
           strcmp(actual, CNET_COMPETE_BASELINE_ENVIRONMENT_SHA256) == 0;
}

static int server_mapped_runtime_matches(void) {
    char path[64], line[4096];
    unsigned long long seen = 0;
    int saw_kfd = 0, saw_render = 0;
    FILE *file;
    size_t index;
    if (RUNTIME_FILE_COUNT >= 64u ||
        snprintf(path, sizeof path, "/proc/%d/maps",
                 CNET_COMPETE_BASELINE_SERVER_PID) >= (int)sizeof path)
        return 0;
    file = fopen(path, "rb");
    if (file == NULL) return 0;
    while (fgets(line, sizeof line, file) != NULL) {
        unsigned device_major = 0, device_minor = 0;
        unsigned long long inode = 0;
        char permissions[8] = "";
        char *mapped = strchr(line, '/');
        int matched = 0;
        if (sscanf(line, "%*s %7s", permissions) != 1 ||
            strlen(permissions) != 4u)
            goto fail;
        if (mapped == NULL) {
            char *named = strchr(line, '[');
            if (strchr(permissions, 'x') != NULL &&
                (named == NULL ||
                 (strncmp(named, "[vdso]", 6) != 0 &&
                  strncmp(named, "[vsyscall]", 10) != 0)))
                goto fail;
            continue;
        }
        mapped[strcspn(mapped, "\r\n")] = '\0';
        if (sscanf(line, "%*s %*s %*s %x:%x %llu", &device_major,
                   &device_minor, &inode) != 3)
            goto fail;
        if (strcmp(mapped, CNET_COMPETE_BASELINE_MODEL) == 0)
            continue;
        if (strcmp(mapped, "/dev/kfd") == 0) {
            if (device_major != 0 || device_minor != 6 || inode != 1112u)
                goto fail;
            saw_kfd = 1;
            continue;
        }
        if (strcmp(mapped, "/dev/dri/renderD128") == 0) {
            if (device_major != 0 || device_minor != 6 || inode != 1122u)
                goto fail;
            saw_render = 1;
            continue;
        }
        for (index = 0; index < RUNTIME_FILE_COUNT; ++index) {
            if (strcmp(mapped, runtime_files[index].path) == 0) {
                const RuntimeGuard *guard = &runtime_guards[index];
                if (device_major != major(guard->device) ||
                    device_minor != minor(guard->device) ||
                    inode != (unsigned long long)guard->inode)
                    goto fail;
                seen |= UINT64_C(1) << index;
                matched = 1;
                break;
            }
        }
        if (!matched) goto fail;
    }
    if (ferror(file) || fclose(file) != 0) return 0;
    return seen == ((UINT64_C(1) << RUNTIME_FILE_COUNT) - 1u) &&
           saw_kfd && saw_render;
fail:
    (void)fclose(file);
    return 0;
}

static int server_runtime_files_match(void) {
    return runtime_guards_verify() && server_mapped_runtime_matches() &&
           server_environment_matches();
}

static int listener_inode(unsigned long long *inode_out) {
    char path[64], line[1024];
    FILE *file;
    unsigned matches = 0;
    unsigned long long matched_inode = 0;
    if (inode_out == NULL ||
        snprintf(path, sizeof path, "/proc/%d/net/tcp",
                 CNET_COMPETE_BASELINE_SERVER_PID) >= (int)sizeof path)
        return 0;
    file = fopen(path, "rb");
    if (file == NULL) return 0;
    while (fgets(line, sizeof line, file) != NULL) {
        char *save = NULL, *field = strtok_r(line, " \t\r\n", &save);
        int number = 0, local = 0, remote = 0, listening = 0;
        unsigned long long inode = 0;
        while (field != NULL) {
            if (number == 1) local = strcmp(field, "0100007F:1F90") == 0;
            else if (number == 2) remote = strcmp(field, "00000000:0000") == 0;
            else if (number == 3) listening = strcmp(field, "0A") == 0;
            else if (number == 9) {
                char *end = NULL;
                errno = 0;
                inode = strtoull(field, &end, 10);
                if (errno != 0 || end == field || *end != '\0') inode = 0;
                break;
            }
            ++number;
            field = strtok_r(NULL, " \t\r\n", &save);
        }
        if (local && remote && listening && inode != 0) {
            matched_inode = inode;
            ++matches;
        }
    }
    if (ferror(file) || fclose(file) != 0 || matches != 1u) return 0;
    *inode_out = matched_inode;
    return 1;
}

static int server_owns_listener(void) {
    char directory_path[64], link_path[128], target[128], expected[64];
    unsigned long long inode;
    DIR *directory;
    struct dirent *entry;
    int found = 0;
    if (!listener_inode(&inode) ||
        snprintf(directory_path, sizeof directory_path, "/proc/%d/fd",
                 CNET_COMPETE_BASELINE_SERVER_PID) >=
            (int)sizeof directory_path ||
        snprintf(expected, sizeof expected, "socket:[%llu]", inode) >=
            (int)sizeof expected)
        return 0;
    directory = opendir(directory_path);
    if (directory == NULL) return 0;
    while ((entry = readdir(directory)) != NULL) {
        ssize_t length;
        if (entry->d_name[0] == '.') continue;
        if (snprintf(link_path, sizeof link_path, "%s/%s", directory_path,
                     entry->d_name) >= (int)sizeof link_path)
            continue;
        length = readlink(link_path, target, sizeof target - 1u);
        if (length < 0 || (size_t)length >= sizeof target) continue;
        target[length] = '\0';
        if (strcmp(target, expected) == 0) { found = 1; break; }
    }
    if (closedir(directory) != 0) found = 0;
    return found;
}

static int server_process_matches(void) {
    char proc_exe[64], proc_cmdline[64], resolved[PATH_MAX];
    char exe_sha[65], config_sha[65];
    unsigned long long start_ticks = 0;
    if (snprintf(proc_exe, sizeof proc_exe, "/proc/%d/exe",
                 CNET_COMPETE_BASELINE_SERVER_PID) >= (int)sizeof proc_exe ||
        snprintf(proc_cmdline, sizeof proc_cmdline, "/proc/%d/cmdline",
                 CNET_COMPETE_BASELINE_SERVER_PID) >=
            (int)sizeof proc_cmdline ||
        realpath(proc_exe, resolved) == NULL ||
        strcmp(resolved, CNET_COMPETE_BASELINE_SERVER_EXE) != 0 ||
        cnet_compete_eval_file_sha256(proc_exe, exe_sha) != 0 ||
        strcmp(exe_sha, CNET_COMPETE_BASELINE_SERVER_SHA256) != 0 ||
        cnet_compete_eval_file_sha256(proc_cmdline, config_sha) != 0 ||
        strcmp(config_sha, CNET_COMPETE_BASELINE_SERVER_CONFIG_SHA256) != 0 ||
        read_server_start_ticks(&start_ticks) != 0 ||
        start_ticks != CNET_COMPETE_BASELINE_SERVER_START_TICKS ||
        !runtime_guards_shape_match() || !server_mapped_runtime_matches() ||
        !server_environment_matches() || !server_owns_listener() ||
        !line_file_equals("/proc/sys/kernel/random/boot_id",
                          CNET_COMPETE_BASELINE_BOOT_ID))
        return 0;
    return 1;
}

static size_t receive_response(void *contents, size_t size, size_t count,
                               void *context) {
    ResponseBuffer *buffer = (ResponseBuffer *)context;
    size_t bytes;
    char *expanded;
    if (size != 0 && count > SIZE_MAX / size) return 0;
    bytes = size * count;
    if (buffer == NULL || buffer->failed ||
        (bytes > 0 && memchr(contents, '\0', bytes) != NULL) ||
        bytes > HTTP_RESPONSE_MAX - buffer->length - 1u)
        return 0;
    if (buffer->length + bytes + 1u > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 4096u : buffer->capacity;
        while (capacity < buffer->length + bytes + 1u) capacity *= 2u;
        if (capacity > HTTP_RESPONSE_MAX) capacity = HTTP_RESPONSE_MAX;
        expanded = (char *)realloc(buffer->data, capacity);
        if (expanded == NULL) { buffer->failed = 1; return 0; }
        buffer->data = expanded;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, contents, bytes);
    buffer->length += bytes;
    buffer->data[buffer->length] = '\0';
    return bytes;
}

static int read_regular_text(const char *path, char **text_out) {
    struct stat status;
    FILE *file = NULL;
    char *text = NULL;
    int rc = -1;
    if (path == NULL || text_out == NULL || lstat(path, &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_size <= 0 ||
        (unsigned long long)status.st_size > SYSTEM_MAX)
        return -1;
    text = (char *)calloc((size_t)status.st_size + 1u, 1u);
    file = fopen(path, "rb");
    if (text == NULL || file == NULL) goto done;
    if (fread(text, 1, (size_t)status.st_size, file) !=
            (size_t)status.st_size || ferror(file))
        goto done;
    if (fclose(file) != 0) { file = NULL; goto done; }
    file = NULL;
    *text_out = text;
    text = NULL;
    rc = 0;
done:
    if (file != NULL) (void)fclose(file);
    free(text);
    return rc;
}

static char *request_body(const char *system, const char *prompt) {
    char *escaped_system = NULL, *escaped_prompt = NULL, *escaped_model = NULL;
    char *body = NULL;
    size_t system_capacity, prompt_capacity, model_capacity, body_capacity;
    int written;
    system_capacity = strlen(system) * 6u + 1u;
    prompt_capacity = strlen(prompt) * 6u + 1u;
    model_capacity = strlen(CNET_COMPETE_BASELINE_MODEL) * 2u + 1u;
    escaped_system = (char *)malloc(system_capacity);
    escaped_prompt = (char *)malloc(prompt_capacity);
    escaped_model = (char *)malloc(model_capacity);
    body_capacity = system_capacity + prompt_capacity + model_capacity + 512u;
    body = (char *)malloc(body_capacity);
    if (escaped_system == NULL || escaped_prompt == NULL ||
        escaped_model == NULL || body == NULL ||
        cnet_compete_eval_json_escape(system, escaped_system,
                                      system_capacity) != 0 ||
        cnet_compete_eval_json_escape(prompt, escaped_prompt,
                                      prompt_capacity) != 0 ||
        cnet_compete_eval_json_escape(CNET_COMPETE_BASELINE_MODEL,
                                      escaped_model, model_capacity) != 0)
        goto fail;
    written = snprintf(
        body, body_capacity,
        "{\"model\":\"%s\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"%s\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}],"
        "\"temperature\":0,\"max_tokens\":128,\"seed\":20260813,"
        "\"stream\":false}",
        escaped_model, escaped_system, escaped_prompt);
    if (written < 0 || (size_t)written >= body_capacity) goto fail;
    free(escaped_model); free(escaped_prompt); free(escaped_system);
    return body;
fail:
    free(body);
    free(escaped_model); free(escaped_prompt); free(escaped_system);
    return NULL;
}

static int configure_curl(CURL *curl, struct curl_slist *headers,
                          ResponseBuffer *response) {
    return curl_easy_setopt(curl, CURLOPT_URL,
                            CNET_COMPETE_BASELINE_ENDPOINT) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_POST, 1L) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                            receive_response) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_WRITEDATA, response) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 300000L) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_PROXY, "") == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_NOPROXY, "*") == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
           curl_easy_setopt(curl, CURLOPT_USERAGENT,
                            "CNET-ASI-5-v1-native-benchmark") == CURLE_OK
               ? 0 : -1;
}

static int verify_server_model(CURL *curl, ResponseBuffer *response) {
    long status = 0;
    CURLcode result;
    response->length = 0;
    response->failed = 0;
    if (response->data != NULL) response->data[0] = '\0';
    curl_easy_reset(curl);
    if (curl_easy_setopt(curl, CURLOPT_URL,
                         CNET_COMPETE_BASELINE_MODELS_ENDPOINT) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                         receive_response) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, response) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_PROXY, "") != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NOPROXY, "*") != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK)
        return -1;
    result = curl_easy_perform(curl);
    if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK ||
        result != CURLE_OK || status != 200 || response->failed ||
        response->data == NULL)
        return -1;
    return cnet_compete_eval_models_response_matches(
               response->data, CNET_COMPETE_BASELINE_MODEL,
               CNET_COMPETE_BASELINE_PARAMETERS, "Q1_0")
               ? 0 : -1;
}

int main(int argc, char **argv) {
    CnetCompeteEvalFixture fixture;
    CnetCompeteEvalSnapshot snapshot;
    CnetCompeteJournalHeader header;
    CnetCompeteJournal journal;
    ModelGuard model_guard;
    ResponseBuffer response;
    struct curl_slist *headers = NULL;
    CURL *curl = NULL;
    char *system = NULL;
    char model_sha[65], runner_sha[65], error[256];
    unsigned long long model_bytes = 0;
    size_t index;
    int curl_initialized = 0, preflight = 0, rc = 1;
    if (argc != 2) {
        fprintf(stderr, "usage: %s RESULT|--preflight\n", argv[0]);
        return 2;
    }
    if (!cnet_compete_client_release_lock_acquire()) {
        fprintf(stderr, "baseline runner refused release lock\n");
        return 2;
    }
    preflight = strcmp(argv[1], "--preflight") == 0;
    if (!preflight && strcmp(argv[1], CNET_COMPETE_BASELINE_RESULT_PATH) != 0) {
        fprintf(stderr, "baseline runner refused noncanonical result ledger\n");
        return 2;
    }
    if (!cnet_compete_eval_git_identity_valid(CNET_COMPETE_BUILD_COMMIT,
                                              CNET_COMPETE_BUILD_TREE)) {
        fprintf(stderr, "baseline runner refused unknown build identity\n");
        return 2;
    }
    memset(&fixture, 0, sizeof fixture);
    memset(&snapshot, 0, sizeof snapshot);
    memset(&header, 0, sizeof header);
    memset(&journal, 0, sizeof journal);
    memset(&model_guard, 0, sizeof model_guard);
    model_guard.descriptor = -1;
    {
        size_t guard_index;
        for (guard_index = 0; guard_index < RUNTIME_FILE_COUNT; ++guard_index)
            runtime_guards[guard_index].descriptor = -1;
    }
    memset(&response, 0, sizeof response);
    error[0] = '\0';
    if (!cnet_compete_client_environment_matches() ||
        !cnet_compete_client_runtime_matches(CNET_COMPETE_CLIENT_BASELINE) ||
        cnet_compete_eval_file_sha256("/proc/self/exe", runner_sha) != 0 ||
        cnet_compete_eval_snapshot_create(&snapshot, 0,
                                          error, sizeof error) != 0 ||
        runtime_guards_open() != 0 ||
        model_guard_open(&model_guard) != 0 ||
        !server_runtime_files_match() || !server_maps_model(&model_guard) ||
        !server_process_matches() ||
        cnet_compete_eval_load_fixture(snapshot.fixture, &fixture,
                                       error, sizeof error) != 0 ||
        read_regular_text(snapshot.system, &system) != 0) {
        fprintf(stderr, "baseline runner identity refused: %s\n", error);
        goto done;
    }
    snprintf(model_sha, sizeof model_sha, "%s", model_guard.sha256);
    model_bytes = model_guard.bytes;
    snprintf(header.backend, sizeof header.backend, "bonsai_8b_cpu_q1_0");
    if (snprintf(
            header.identity, sizeof header.identity,
            "model=%s;commit=%s;tree=%s;system=%s;server=%s;config=%s;"
            "runtime_set=%s;pid=%d;start=%llu;boot=%s;runner=%s;"
            "client_runtime=%s",
            model_sha, CNET_COMPETE_BUILD_COMMIT, CNET_COMPETE_BUILD_TREE,
            CNET_COMPETE_SYSTEM_SHA256, CNET_COMPETE_BASELINE_SERVER_SHA256,
            CNET_COMPETE_BASELINE_SERVER_CONFIG_SHA256,
            CNET_COMPETE_BASELINE_RUNTIME_SET_SHA256,
            CNET_COMPETE_BASELINE_SERVER_PID,
            CNET_COMPETE_BASELINE_SERVER_START_TICKS,
            CNET_COMPETE_BASELINE_BOOT_ID, runner_sha,
            CNET_COMPETE_BASELINE_CLIENT_RUNTIME_SET_SHA256) >=
        (int)sizeof header.identity)
        goto done;
    header.parameters = CNET_COMPETE_BASELINE_PARAMETERS;
    header.model_bytes = model_bytes;
    header.artifact_bytes = model_bytes;
    if (cnet_compete_eval_prepare_results_directory(error, sizeof error) != 0) {
        fprintf(stderr, "baseline result directory refused: %s\n", error);
        goto done;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) goto done;
    curl_initialized = 1;
    curl = curl_easy_init();
    if (curl == NULL || verify_server_model(curl, &response) != 0) {
        fprintf(stderr, "baseline server model identity refused\n");
        goto done;
    }
    if (preflight) {
        if (!model_guard_verify(&model_guard) ||
            !server_runtime_files_match() || !server_process_matches() ||
            !server_maps_model(&model_guard))
            goto done;
        printf("CNET_7B_BASELINE_PREFLIGHT_PASS model_sha256=%s "
               "parameters=%llu bytes=%llu server_sha256=%s "
               "server_config_sha256=%s runtime_set_sha256=%s "
               "runner_sha256=%s pid=%d start=%llu "
               "commit=%s tree=%s endpoint=%s\n",
               model_sha, CNET_COMPETE_BASELINE_PARAMETERS, model_bytes,
               CNET_COMPETE_BASELINE_SERVER_SHA256,
               CNET_COMPETE_BASELINE_SERVER_CONFIG_SHA256,
               CNET_COMPETE_BASELINE_RUNTIME_SET_SHA256,
               runner_sha,
               CNET_COMPETE_BASELINE_SERVER_PID,
               CNET_COMPETE_BASELINE_SERVER_START_TICKS,
               CNET_COMPETE_BUILD_COMMIT, CNET_COMPETE_BUILD_TREE,
               CNET_COMPETE_BASELINE_MODELS_ENDPOINT);
        rc = 0;
        goto done;
    }
    if (cnet_compete_journal_open(&journal, argv[1], &fixture, &header,
                                  error, sizeof error) != 0) {
        fprintf(stderr, "baseline result journal refused: %s\n", error);
        goto done;
    }
    if (!journal.complete) {
        headers = curl_slist_append(headers, "Content-Type: application/json");
        curl_easy_reset(curl);
        if (headers == NULL || configure_curl(curl, headers, &response) != 0)
            goto done;
        for (index = journal.completed_rows; index < fixture.count; ++index) {
            char content[CNET_COMPETE_EVAL_OUTPUT_MAX] = "";
            char *body = request_body(system, fixture.rows[index].prompt);
            uint64_t start, finish;
            CURLcode curl_result;
            long http_status = 0;
            int run_rc = 0;
            if (body == NULL) goto done;
            response.length = 0;
            response.failed = 0;
            if (response.data != NULL) response.data[0] = '\0';
            if (curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body) != CURLE_OK ||
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                                 (curl_off_t)strlen(body)) != CURLE_OK) {
                free(body);
                goto done;
            }
            if (!model_guard_shape_matches(&model_guard) ||
                !runtime_guards_shape_match() || !server_process_matches() ||
                !server_maps_model(&model_guard) ||
                !cnet_compete_client_environment_matches() ||
                !cnet_compete_client_runtime_shape_matches(
                    CNET_COMPETE_CLIENT_BASELINE)) {
                (void)cnet_compete_journal_fail(&journal,
                                                "baseline_identity_changed");
                free(body);
                goto done;
            }
            if (cnet_compete_journal_issue(&journal, &fixture.rows[index]) != 0) {
                free(body);
                goto done;
            }
            start = cnet_compete_eval_now_ns();
            curl_result = curl_easy_perform(curl);
            if (curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE,
                                  &http_status) != CURLE_OK)
                http_status = 0;
            if (!model_guard_shape_matches(&model_guard) ||
                !runtime_guards_shape_match() || !server_process_matches() ||
                !server_maps_model(&model_guard) ||
                !cnet_compete_client_environment_matches() ||
                !cnet_compete_client_runtime_shape_matches(
                    CNET_COMPETE_CLIENT_BASELINE)) {
                run_rc = 6;
            } else if (run_rc == 0 &&
                (curl_result != CURLE_OK || http_status != 200 ||
                 response.failed || response.data == NULL)) {
                run_rc = 1;
            } else if (run_rc == 0 && cnet_compete_eval_extract_chat_content(
                           response.data, CNET_COMPETE_BASELINE_MODEL,
                           content, sizeof content) != 0) {
                run_rc = 3;
            }
            finish = cnet_compete_eval_now_ns();
            if (start == 0 || finish < start) run_rc = 4;
            free(body);
            if (cnet_compete_journal_append(
                    &journal, &fixture.rows[index],
                    finish >= start ? finish - start : 0,
                    run_rc, 0, run_rc == 0 ? content : "") != 0)
                goto done;
            if ((index + 1u) % 16u == 0 || index + 1u == fixture.count)
                fprintf(stderr, "baseline held-out progress %zu/%zu\n",
                        index + 1u, fixture.count);
        }
        if (!model_guard_verify(&model_guard) ||
            !server_runtime_files_match() || !server_process_matches() ||
            !server_maps_model(&model_guard) ||
            !cnet_compete_client_environment_matches() ||
            !cnet_compete_client_runtime_matches(
                CNET_COMPETE_CLIENT_BASELINE) ||
            cnet_compete_eval_snapshot_verify(&snapshot, error,
                                              sizeof error) != 0) {
            (void)cnet_compete_journal_fail(&journal,
                                            "baseline_identity_changed");
            goto done;
        }
        if (cnet_compete_journal_finish(&journal, fixture.count) != 0)
            goto done;
    }
    if (!model_guard_verify(&model_guard) || !server_runtime_files_match() ||
        !server_process_matches() || !server_maps_model(&model_guard) ||
        !cnet_compete_client_environment_matches() ||
        !cnet_compete_client_runtime_matches(CNET_COMPETE_CLIENT_BASELINE) ||
        cnet_compete_eval_snapshot_verify(&snapshot, error,
                                          sizeof error) != 0)
        goto done;
    printf("CNET_7B_BASELINE_RUN_PASS rows=%zu model_sha256=%s "
           "endpoint=%s\n", journal.completed_rows, model_sha,
           CNET_COMPETE_BASELINE_ENDPOINT);
    rc = 0;
done:
    if (curl != NULL) curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    if (curl_initialized) curl_global_cleanup();
    free(response.data);
    free(system);
    cnet_compete_journal_close(&journal);
    cnet_compete_eval_free_fixture(&fixture);
    cnet_compete_eval_snapshot_destroy(&snapshot);
    model_guard_close(&model_guard);
    runtime_guards_close();
    return rc;
}
