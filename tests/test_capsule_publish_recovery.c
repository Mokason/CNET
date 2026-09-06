#define main capsule_cli_main
#include "../tools/cnet_capsule_core_main.c"
#undef main
#include <sys/syscall.h>

static char sync_root[512], final_artifact[600];
static int inject, failures, root_parent_synced;
int fsync(int fd) {
    char link[64], path[1024];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof path - 1);
    if (n < 0) return -1;
    path[n] = 0;
    if (!strcmp(path, sync_root)) root_parent_synced++;
    if (inject && access(final_artifact, F_OK) == 0 &&
        strstr(path, "/capsules") && !strstr(path, "/.pending-")) {
        errno = EIO; return -1;
    }
    return (int)syscall(SYS_fsync, fd);
}
static void check(int ok, const char *label) {
    if (!ok) { fprintf(stderr, "FAIL %s\n", label); failures++; }
}
int main(void) {
    char root[] = "/tmp/cnet-publish-recovery-XXXXXX", store[600], rows[600], error[160];
    if (!mkdtemp(root)) return 2;
    snprintf(sync_root, sizeof sync_root, "%s", root);
    snprintf(store, sizeof store, "%s/capsules/", root);
    snprintf(final_artifact, sizeof final_artifact, "%s/capsules/recover", root);
    snprintf(rows, sizeof rows, "%s/rows.tsv", root);
    FILE *fp = fopen(rows, "w"); if (!fp) return 2;
    fputs("0 0\n1 1\n", fp); fclose(fp);
    char *args[] = {"core", "teach", store, "recover", "left", "right", "1", "1", "verified_tool", rows};
    unsetenv("CNET_CAPSULE_EVAL_FILE");
    inject = 1;
    check(teach(10, args) != 0, "publication reports post-rename sync failure");
    check(!access(final_artifact, F_OK), "post-rename failure retains certified artifact");
    CnetCapsuleCore *core = cnet_capsule_core_open(store, error, sizeof error);
    check(core != NULL, "retained artifact imports completely"); cnet_capsule_core_close(core);
    inject = 0; root_parent_synced = 0;
    check(!teach(10, args), "identical retry succeeds");
    check(root_parent_synced > 0, "trailing slash root parent synced on retry");
    inject = 1;
    check(teach(10, args) != 0, "reused artifact cannot skip durability failure");
    printf("CAPSULE_PUBLISH_RECOVERY_%s failures=%d artifacts=%s\n", failures ? "RED" : "PASS", failures, root);
    return failures ? 1 : 0;
}
