#ifndef CNET_COMPETE_SUITE_DATA_H
#define CNET_COMPETE_SUITE_DATA_H

/* Data-only suite identity. The candidate-freeze protocol permits F4 to
   replace only these literal values after the v4 fixture is authored. */
#define CNET_COMPETE_SUITE_ID "CNET-ASI-5-v3"
#define CNET_COMPETE_CANDIDATE_FREEZE_COMMIT \
    "b50e453e7cc6277f5c391d01c3bf71ea20b59a8d"
#define CNET_COMPETE_FIXTURE_PATH "benchmarks/cnet_asi5_v3/heldout.tsv"
#define CNET_COMPETE_SYSTEM_PATH "benchmarks/cnet_asi5_v3/baseline_system.txt"
#define CNET_COMPETE_GENERATOR_PATH "tools/cnet_compete_fixture_v3.c"
#define CNET_COMPETE_FIXTURE_PROVENANCE "verified_spec_v3"
#define CNET_COMPETE_FIXTURE_SHA256 \
    "ea1ee9ff33a3f1fea632e758659a315a20e24eefb2fc803d25c44a513afcaf8c"
#define CNET_COMPETE_SYSTEM_SHA256 \
    "f3ef4c33535a007728e13dd9b1c89bcc124a94261d6814136a62735d34a8bf0a"
#define CNET_COMPETE_GENERATOR_SHA256 \
    "c265a2743f81f66483c110cb77fd1d87afda57da4ffab8e5bb41e54f75fd8f44"
#define CNET_COMPETE_TOTAL_ROWS 448u
#define CNET_COMPETE_COVERED_ROWS 320u
#define CNET_COMPETE_OOD_ROWS 128u
#define CNET_COMPETE_STATE_ROOT \
    "/home/marble/.local/state/cnet/cnet_asi5_v3"

#endif
