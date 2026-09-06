#include "cnet_capsule_core.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"CAPSULE_DEMAND_SECURITY_RED line=%d\n",__LINE__); failures++; } } while (0)
int main(void) {
    char root[] = "/tmp/cnet-demand-security-XXXXXX", path[256], query[96];
    int failures = 0;
    if (!mkdtemp(root)) return 2;
    CHECK(cnet_capsule_demand_note(root, "capsule a b 00012") == 0);
    CHECK(cnet_capsule_demand_note(root, "capsule a b 12") == 1);
    const char *invalid[] = {"capsule ../a b 1", "capsule a b -1", "capsule a b 1.5", "capsule a b 65536", "capsule a b 1 extra"};
    for (size_t i=0; i<sizeof invalid/sizeof *invalid; i++) CHECK(cnet_capsule_demand_note(root, invalid[i]) == -1);
    chmod(root, 0755);
    CHECK(cnet_capsule_demand_note(root, "capsule a b 12") == -1);
    chmod(root, 0700);
    snprintf(path, sizeof path, "%s/.lock", root);
    int lock = open(path, O_RDWR);
    CHECK(lock >= 0 && !flock(lock, LOCK_EX));
    CHECK(cnet_capsule_demand_note(root, "capsule a b 13") == -1);
    close(lock);
    snprintf(path, sizeof path, "%s/i1_a_o1_b_v13.req", root);
    CHECK(symlink(".lock", path) == 0);
    CHECK(cnet_capsule_demand_note(root, "capsule a b 13") == -1);
    unlink(path);
    /* Concurrent producers must publish one complete canonical record. */
    for (int i=0; i<8; i++) {
        pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) { int rc = cnet_capsule_demand_note(root, "capsule a b 13"); _exit(rc < -1 || rc > 1); }
    }
    for (int i=0; i<8; i++) { int status; CHECK(wait(&status) > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0); }
    CHECK(cnet_capsule_demand_note(root, "capsule a b 13") == 1);
    for (unsigned x=0; x<256; x++) {
        snprintf(query,sizeof query,"capsule a b %u",x);
        CHECK(cnet_capsule_demand_note(root,query) >= 0);
    }
    CHECK(cnet_capsule_demand_note(root, "capsule a b 256") == -1);
    CHECK(cnet_capsule_demand_note(root, "capsule a b 12") == 1);
    DIR *dir = opendir(root);
    struct dirent *entry;
    while (dir && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        unlinkat(dirfd(dir), entry->d_name, 0);
    }
    if (dir) closedir(dir);
    rmdir(root);
    if (!failures) puts("CAPSULE_DEMAND_SECURITY_PASS pending_limit=256 concurrency=8");
    return failures ? 1 : 0;
}
