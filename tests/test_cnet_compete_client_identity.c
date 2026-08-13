#include "cnet_compete_client_identity.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <unistd.h>

#ifndef CNET_COMPETE_CLIENT_TEST_ROLE
#error "CNET_COMPETE_CLIENT_TEST_ROLE must select the tested executable role"
#endif

int main(void) {
    int competing_lock;
    if (!cnet_compete_client_release_lock_acquire() ||
        !cnet_compete_client_release_lock_acquire() ||
        !cnet_compete_client_environment_matches() ||
        !cnet_compete_client_environment_matches() ||
        !cnet_compete_client_runtime_matches(CNET_COMPETE_CLIENT_TEST_ROLE) ||
        !cnet_compete_client_runtime_shape_matches(
            CNET_COMPETE_CLIENT_TEST_ROLE) ||
        !cnet_compete_client_runtime_matches(CNET_COMPETE_CLIENT_TEST_ROLE)) {
        printf("CNET_7B_CLIENT_IDENTITY_RED role=%d\n",
               CNET_COMPETE_CLIENT_TEST_ROLE);
        return 1;
    }
    competing_lock = open("/home/marble/.local/state/cnet/.release.lock",
                          O_RDONLY | O_NOFOLLOW);
    if (competing_lock < 0 ||
        flock(competing_lock, LOCK_EX | LOCK_NB) == 0 ||
        (errno != EWOULDBLOCK && errno != EAGAIN)) {
        if (competing_lock >= 0) (void)close(competing_lock);
        printf("CNET_7B_CLIENT_IDENTITY_RED role=%d\n",
               CNET_COMPETE_CLIENT_TEST_ROLE);
        return 1;
    }
    (void)close(competing_lock);
    printf("CNET_7B_CLIENT_IDENTITY_PASS role=%d\n",
           CNET_COMPETE_CLIENT_TEST_ROLE);
    return 0;
}
