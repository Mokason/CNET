#ifndef CNET_COMPETE_CLIENT_IDENTITY_H
#define CNET_COMPETE_CLIENT_IDENTITY_H

#include <stddef.h>

enum {
    CNET_COMPETE_CLIENT_FIXTURE = 0,
    CNET_COMPETE_CLIENT_BASELINE = 1,
    CNET_COMPETE_CLIENT_SCORER = 2
};

#define CNET_COMPETE_FIXTURE_RUNTIME_SET_SHA256 \
    "324acc2f2463ff0cff222eee0c56b902d8655861a26e986c386e6eb29c6a7920"
#define CNET_COMPETE_BASELINE_CLIENT_RUNTIME_SET_SHA256 \
    "4efc0a04bf8c9239738ee6f6ccedde5084ed179b10d4cc3c234d03f35b495892"
#define CNET_COMPETE_SCORER_RUNTIME_SET_SHA256 \
    "cf7eaf722d058a58e657dcb3941c7802e55548101b1b48b8055cce34113d6640"

int cnet_compete_client_environment_matches(void);
int cnet_compete_client_release_lock_acquire(void);
int cnet_compete_client_runtime_matches(int runtime_kind);
int cnet_compete_client_runtime_shape_matches(int runtime_kind);

#endif
