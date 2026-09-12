#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
cc -std=c11 -O3 -march=native -Wall -Wextra -Werror -fPIC -shared -fstack-usage -Iinclude \
  -o experiments/hdc_structure/native.so experiments/hdc_structure/native.c \
  src/cnet_vsa.c src/cnet_vsa_bsc.c src/cnet_vsa_text.c src/cnet_vsa_memory.c \
  src/cnet_vsa_ngram.c src/cnet_vsa_gen_capsule.c src/cnet_vsa_reason.c -lm -pthread
