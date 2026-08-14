#!/bin/bash

set -euo pipefail

if [ "$#" -ne 6 ]; then
    echo "usage: $0 SNAPSHOT_ROOT WORKSPACE_ROOT STAGING_ROOT RELEASE_ROOT COMMIT TREE" >&2
    exit 2
fi

snapshot_root=$1
workspace_root=$2
staging_root=$3
release_root=$4
build_commit=$5
build_tree=$6

case "$snapshot_root" in
    /*) ;;
    *) echo "snapshot root must be absolute" >&2; exit 2 ;;
esac
case "$workspace_root" in
    /*) ;;
    *) echo "workspace root must be absolute" >&2; exit 2 ;;
esac
case "$release_root" in
    /home/marble/.local/state/cnet/cnet_asi5_v4) ;;
    *) echo "release root must be canonical" >&2; exit 2 ;;
esac
case "$staging_root" in
    /home/marble/.local/state/cnet/.cnet-asi5-v4-stage-??????) ;;
    *) echo "staging root must be canonical private storage" >&2; exit 2 ;;
esac
case "$build_commit:$build_tree" in
    *[!0-9a-f:]*)
        echo "invalid build identity" >&2
        exit 2
        ;;
esac
if [ "${#build_commit}" -ne 40 ] || [ "${#build_tree}" -ne 40 ] ||
   [ "$snapshot_root" = "$workspace_root" ] ||
   [ "$snapshot_root" = "$release_root" ] ||
   [ "$snapshot_root" = "$staging_root" ]; then
    echo "invalid immutable build roots or identity" >&2
    exit 2
fi

cc=/usr/bin/gcc
make=/usr/bin/make
pkg_config=/usr/bin/pkg-config
sha256sum=/usr/bin/sha256sum
cmp=/usr/bin/cmp
readlink=/usr/bin/readlink
install=/usr/bin/install
toolchain_set_expected=6b48233c6d4900f94ab2f0e9651757cbadbd52340bb6675990aff2906658e50f
system_input_set_expected=ceab6860dfdde66c10cf500586bcd16ae9668f05fa493def912b8d6224182844

if [ -e /etc/ld.so.preload ]; then
    echo "system preload configuration refused" >&2
    exit 1
fi

toolchain_set=$(
    "$sha256sum" \
        /usr/bin/x86_64-linux-gnu-gcc-13 \
        /usr/libexec/gcc/x86_64-linux-gnu/13/cc1 \
        /usr/libexec/gcc/x86_64-linux-gnu/13/collect2 \
        /usr/bin/as \
        /usr/bin/ld \
        /usr/bin/make \
        /usr/bin/pkg-config \
        /usr/bin/bash \
        /usr/bin/sh \
        /usr/bin/sha256sum \
        /usr/bin/cmp \
        /usr/bin/install \
        /usr/bin/readlink \
        /usr/bin/tee \
        /usr/bin/grep \
        /usr/bin/mkdir \
        /usr/bin/rm \
        /usr/bin/mv \
        /usr/bin/env \
        /usr/bin/tar \
        /usr/bin/git \
        /usr/bin/mktemp \
        /usr/bin/wc \
        /usr/bin/flock \
        /usr/bin/sync \
        /usr/bin/chmod \
        /usr/bin/find \
        /usr/bin/sort \
        /usr/bin/xargs | "$sha256sum"
)
toolchain_set=${toolchain_set%% *}
if [ "$toolchain_set" != "$toolchain_set_expected" ] ||
   [ "$("$readlink" -f "$cc")" != "/usr/bin/x86_64-linux-gnu-gcc-13" ]; then
    echo "pinned build toolchain refused" >&2
    exit 1
fi

system_input_digest() {
    {
        /usr/bin/find /usr/include /usr/local/include \
            /usr/lib/gcc/x86_64-linux-gnu/13 \
            /usr/lib/x86_64-linux-gnu -xdev -type f -print0 |
            /usr/bin/sort -z |
            /usr/bin/xargs -0 -r /usr/bin/sha256sum
        /usr/bin/find /usr/include /usr/local/include \
            /usr/lib/gcc/x86_64-linux-gnu/13 \
            /usr/lib/x86_64-linux-gnu -xdev -type l \
            -printf 'link %p -> %l\n' | /usr/bin/sort
    } | /usr/bin/sha256sum
}

system_input_set=$(system_input_digest)
system_input_set=${system_input_set%% *}
if [ "$system_input_set" != "$system_input_set_expected" ]; then
    echo "pinned build headers/libraries refused" >&2
    exit 1
fi

umask 077
"$install" -d -m 0700 "$staging_root/bin" "$staging_root/evidence" \
    "$staging_root/inputs" "$staging_root/inputs/artifacts" \
    "$staging_root/results"
cd "$snapshot_root"

"$make" CC="$cc" \
    PYTHON=/bin/false \
    CNET_COMPETE_BUILD_COMMIT="$build_commit" \
    CNET_COMPETE_BUILD_TREE="$build_tree" \
    CNET_COMPETE_SUITE_DEFINE='-DCNET_COMPETE_SUITE_DATA_HEADER="cnet_compete_suite_data_v4.h"' \
    cnet_7b_v4_fixture_audit >&2

"$make" CC="$cc" \
    PYTHON=/bin/false \
    CNET_COMPETE_BUILD_COMMIT="$build_commit" \
    CNET_COMPETE_BUILD_TREE="$build_tree" \
    CNET_COMPETE_SUITE_DEFINE='-DCNET_COMPETE_SUITE_DATA_HEADER="cnet_compete_suite_data_v4.h"' \
    cnet_7b_artifact_manifest >&2

artifact_sha=$("$sha256sum" artifacts/cnet_asi5_v4/artifacts.sha256)
artifact_sha=${artifact_sha%% *}
if [ "$artifact_sha" != \
     "df0f7131aaa75627d5c542b375a39615ab16b93388878490037ad76b79a2d661" ]; then
    echo "immutable artifact manifest digest mismatch" >&2
    exit 1
fi

"$make" CC="$cc" \
    PYTHON=/bin/false \
    CNET_COMPETE_BUILD_COMMIT="$build_commit" \
    CNET_COMPETE_BUILD_TREE="$build_tree" \
    CNET_COMPETE_SUITE_DEFINE='-DCNET_COMPETE_SUITE_DATA_HEADER="cnet_compete_suite_data_v4.h"' \
    cnet_7b_capsules_san >&2
"$install" -m 0600 logs/cnet_7b_capsules_san.log \
    "$staging_root/evidence/cnet_7b_capsules_san.log"
"$make" CC="$cc" \
    PYTHON=/bin/false \
    CNET_COMPETE_BUILD_COMMIT="$build_commit" \
    CNET_COMPETE_BUILD_TREE="$build_tree" \
    CNET_COMPETE_SUITE_DEFINE='-DCNET_COMPETE_SUITE_DATA_HEADER="cnet_compete_suite_data_v4.h"' \
    cnet_7b_runtime_san >&2
"$make" CC="$cc" \
    PYTHON=/bin/false \
    CNET_COMPETE_BUILD_COMMIT="$build_commit" \
    CNET_COMPETE_BUILD_TREE="$build_tree" \
    CNET_COMPETE_SUITE_DEFINE='-DCNET_COMPETE_SUITE_DATA_HEADER="cnet_compete_suite_data_v4.h"' \
    cnet_7b_eval_san >&2

"$install" -m 0600 logs/cnet_7b_runtime_san.log \
    "$staging_root/evidence/cnet_7b_runtime_san.log"
"$install" -m 0600 logs/cnet_7b_eval_san.log \
    "$staging_root/evidence/cnet_7b_eval_san.log"
"$install" -m 0600 logs/cnet_7b_score_san.log \
    "$staging_root/evidence/cnet_7b_score_san.log"

"$install" -m 0600 benchmarks/cnet_asi5_v4/heldout.tsv \
    "$staging_root/inputs/heldout.tsv"
"$install" -m 0600 benchmarks/cnet_asi5_v4/baseline_system.txt \
    "$staging_root/inputs/baseline_system.txt"
"$install" -m 0600 tools/cnet_compete_fixture_v4.c \
    "$staging_root/inputs/fixture_generator.c"
for member in \
    intent.wlm \
    intent.meta \
    capsules/.complete \
    capsules/access_policy_v1/manifest.cknow \
    capsules/access_policy_v1/unit.cnb \
    capsules/add3_mod256/manifest.cknow \
    capsules/add3_mod256/unit.cnb \
    capsules/crc8_atm/manifest.cknow \
    capsules/crc8_atm/unit.cnb \
    capsules/double_mod256/manifest.cknow \
    capsules/double_mod256/unit.cnb \
    capsules/increment_mod256/manifest.cknow \
    capsules/increment_mod256/unit.cnb \
    capsules/minutes_to_seconds/manifest.cknow \
    capsules/minutes_to_seconds/unit.cnb \
    artifacts.sha256
do
    destination="$staging_root/inputs/artifacts/$member"
    "$install" -D -m 0600 "artifacts/cnet_asi5_v4/$member" "$destination"
done

common_flags="-std=c11 -Wall -Wextra -pedantic -Werror -O3 -march=znver3 -D_DEFAULT_SOURCE"
identity_flags="-DCNET_COMPETE_BUILD_COMMIT=\"$build_commit\" -DCNET_COMPETE_BUILD_TREE=\"$build_tree\""
release_flags="-DCNET_COMPETE_SUITE_DATA_HEADER=\"cnet_compete_suite_data_v4.h\" -DCNET_COMPETE_RELEASE_ARTIFACT_ROOT=\"$release_root/inputs/artifacts\" -DCNET_COMPETE_FROZEN_FIXTURE_PATH=\"$release_root/inputs/heldout.tsv\" -DCNET_COMPETE_FROZEN_SYSTEM_PATH=\"$release_root/inputs/baseline_system.txt\" -DCNET_COMPETE_FROZEN_GENERATOR_PATH=\"$release_root/inputs/fixture_generator.c\" -DCNET_COMPETE_FROZEN_ARTIFACT_ROOT=\"$release_root/inputs/artifacts\""
eval_core="src/cnet_compete_eval.c src/cnet_compete.c src/cce/cce_campaign_provenance.c"
capsule_core="src/cnet_capsule.c src/hybrid_ai.c src/base.c src/nn.c src/contract/contract.c src/contract/unit.c src/contract/coverage.c src/acquire.c src/runtime_identity.c src/plan_table.c"
router="src/router/dag_full.c src/router/registry.c src/router/route.c"
specialist="src/specialist.c src/specialist_health.c"

# The word lists above are fixed repository paths from this exact Git archive.
# shellcheck disable=SC2086
"$cc" $common_flags -DCNET_HAVE_CURL=0 $identity_flags $release_flags \
    -ffunction-sections -fdata-sections -Iinclude \
    -o "$staging_root/bin/cnet_compete_run_fixture" \
    tools/cnet_compete_run_fixture.c src/cnet_compete_runtime.c \
    src/cnet_compete_intent.c src/cnet_compete_capsules.c \
    src/cce/cce_wordlm.c src/cnet_compete_client_identity.c \
    $eval_core $capsule_core $router $specialist \
    -Wl,--gc-sections -lm -lpthread

curl_cflags=$("$pkg_config" --cflags libcurl)
curl_libs=$("$pkg_config" --libs libcurl)
# shellcheck disable=SC2086
"$cc" $common_flags -DCNET_HAVE_CURL=1 $identity_flags $release_flags \
    $curl_cflags -Iinclude -o "$staging_root/bin/cnet_compete_run_baseline" \
    tools/cnet_compete_run_baseline.c src/cnet_compete_client_identity.c \
    $eval_core $curl_libs -lm -lpthread

# shellcheck disable=SC2086
"$cc" $common_flags -DCNET_HAVE_CURL=0 $identity_flags $release_flags \
    -Iinclude -o "$staging_root/bin/cnet_compete_score" \
    tools/cnet_compete_score.c src/cnet_compete_score.c \
    src/cnet_compete_client_identity.c $eval_core \
    -lm -lpthread

cnet_runner_sha=$("$sha256sum" "$staging_root/bin/cnet_compete_run_fixture")
cnet_runner_sha=${cnet_runner_sha%% *}
baseline_runner_sha=$("$sha256sum" "$staging_root/bin/cnet_compete_run_baseline")
baseline_runner_sha=${baseline_runner_sha%% *}
scorer_sha=$("$sha256sum" "$staging_root/bin/cnet_compete_score")
scorer_sha=${scorer_sha%% *}
system_input_set_after=$(system_input_digest)
system_input_set_after=${system_input_set_after%% *}
if [ "$system_input_set_after" != "$system_input_set" ]; then
    echo "build headers/libraries changed during compilation" >&2
    exit 1
fi
compiler_path=$("$readlink" -f "$cc")
compiler_sha=$("$sha256sum" "$compiler_path")
compiler_sha=${compiler_sha%% *}
compiler_version=$($cc -dumpfullversion -dumpversion)
compiler_target=$($cc -dumpmachine)

# GNU install applies -m only to the final directory operand.  Normalize and
# verify every directory created below inputs before this private tree can be
# atomically published as the canonical release.
/usr/bin/find "$staging_root" -type d -exec /usr/bin/chmod 0700 {} +
bad_directory=$(/usr/bin/find "$staging_root" -type d ! -perm 0700 \
    -print -quit)
if [ -n "$bad_directory" ]; then
    echo "private release directory mode refused: $bad_directory" >&2
    exit 1
fi

/usr/bin/sync -f "$staging_root"

printf '%s\n' \
    "CNET_7B_EVAL_BUILD_PASS commit=$build_commit tree=$build_tree artifact_sha256=$artifact_sha cnet_runner_sha256=$cnet_runner_sha baseline_runner_sha256=$baseline_runner_sha scorer_sha256=$scorer_sha toolchain_set_sha256=$toolchain_set system_input_set_sha256=$system_input_set compiler=$compiler_path compiler_sha256=$compiler_sha compiler_version=$compiler_version compiler_target=$compiler_target cpu_target=znver3"
