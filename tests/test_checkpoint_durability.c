#define main capsule_fixture_main
#include "test_knowledge_capsule.c"
#undef main
#include <errno.h>
#include <sys/syscall.h>

static int fail_kind, sync_files, sync_dirs;
/* Linux-only fault injection into real shared-library persistence calls. */
int fsync(int fd) {
    struct stat st;
    if (fstat(fd, &st)) return -1;
    int dir = S_ISDIR(st.st_mode);
    if (dir) sync_dirs++; else sync_files++;
    char link[64], path[1024], manifest[1100];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof path - 1);
    if (n < 0) return -1;
    path[n] = 0;
    snprintf(manifest, sizeof manifest, "%s/manifest.cknow", path);
    if ((fail_kind == 3 && !dir && strstr(path, "manifest.cknow.tmp-")) ||
        (fail_kind == 4 && dir && access(manifest, F_OK) == 0)) { errno = EIO; return -1; }
    if (fail_kind == (dir ? 2 : 1)) { errno = EIO; return -1; }
    return (int)syscall(SYS_fsync, fd);
}

int main(void) {
    char root[] = "/tmp/cnet-durability-XXXXXX", path[512], tmp[520], victim[512], pack[512];
    if (!mkdtemp(root)) return 2;
    snprintf(path, sizeof path, "%s/guard", root);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    snprintf(victim, sizeof victim, "%s/victim", root);
    snprintf(pack, sizeof pack, "%s/capsule", root);
    CnetBase base; HybridAi h, loaded; CnetCapsuleReport report;
    cnb_init(&base); hybrid_ai_init(&h); hybrid_ai_init(&loaded);
    check(!build_unit(&base, &h, "hyb_struct_durable"), "fixture");
    FILE *fp = fopen(victim, "w"); if (!fp) return 2;
    fputs("preserve", fp); fclose(fp);
    check(!symlink(victim, tmp), "plant old predictable temporary symlink");
    check(!hybrid_coverage_save(&h, path), "normal save");
    char value[32] = {0}; fp = fopen(victim, "r"); if (!fp) return 2;
    size_t read_bytes = fread(value, 1, sizeof value - 1, fp); fclose(fp);
    check(read_bytes > 0, "read protected file");
    check(!strcmp(value, "preserve"), "temporary symlink cannot truncate unrelated file");
    fail_kind = 1;
    check(hybrid_coverage_save(&h, path) != 0, "file sync failure reported");
    fail_kind = 2;
    check(hybrid_coverage_save(&h, path) != 0, "directory sync failure reported");
    fail_kind = 0;
    check(!hybrid_coverage_load(&loaded, path), "failed sync leaves a complete readable checkpoint");
    check(!hybrid_coverage_save(&h, path), "retry repairs durability");
    sync_files = sync_dirs = 0;
    check(!cnet_capsule_export(&base, &h, "hyb_struct_durable", pack, &report), "export");
    check(sync_files >= 2 && sync_dirs >= 3, "payload manifest capsule and root directory synced");
    fail_kind = 2;
    check(cnet_capsule_export(&base, &h, "hyb_struct_durable", pack, &report) != 0, "export refuses failed directory sync");
    fail_kind = 0;
    snprintf(pack, sizeof pack, "%s/manifest-file-failure", root);
    fail_kind = 3;
    check(cnet_capsule_export(&base, &h, "hyb_struct_durable", pack, &report) != 0,
          "manifest file-sync failure refuses publication");
    snprintf(pack, sizeof pack, "%s/manifest-directory-failure", root);
    fail_kind = 4;
    check(cnet_capsule_export(&base, &h, "hyb_struct_durable", pack, &report) != 0,
          "post-manifest rename directory-sync failure is reported");
    fail_kind = 0;
    CnetBase imported; HybridAi imported_guard;
    cnb_init(&imported); hybrid_ai_init(&imported_guard);
    check(!cnet_capsule_import(&imported, &imported_guard, pack, &report),
          "post-manifest sync failure retains complete certified artifact");
    cnb_free(&imported); hybrid_ai_free(&imported_guard);
    hybrid_ai_free(&loaded); hybrid_ai_free(&h); cnb_free(&base);
    printf("CHECKPOINT_DURABILITY_%s checks=%d failures=%d artifacts=%s\n", failures ? "RED" : "PASS", checks, failures, root);
    return failures ? 1 : 0;
}
