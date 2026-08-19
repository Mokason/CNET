#!/bin/sh
# curl_guard.sh -- the regression gate for the optional-libcurl contract.
#
# WHY THIS EXISTS
# ---------------
# The Makefile probes for libcurl and always passes -DCNET_HAVE_CURL=0 or =1.
# The macro is therefore ALWAYS defined, and `#ifdef CNET_HAVE_CURL` is always
# true. Two sites in cce_safetensors.c spelled it that way while fifteen others
# in the same file spelled it `#if`, so <curl/curl.h> was included even on boxes
# without libcurl -- and cce_dll, which is `verify`'s third prerequisite, could
# not build. Two more files included curl with no guard at all.
#
# The degraded no-curl path was designed for, commented, and unreachable. This
# gate keeps it reachable by refusing the two spellings that broke it.
#
# Run: sh tests/curl_guard.sh   (or `make curl_guard`)
set -u
fail=0

# 1. #ifdef is always true here. It must never appear.
ifdefs=$(grep -rn '#ifdef  *CNET_HAVE_CURL' src include 2>/dev/null || true)
if [ -n "$ifdefs" ]; then
    printf 'CURL_GUARD: FAIL - #ifdef CNET_HAVE_CURL is always true (the macro is\n'
    printf '  always defined, to 0 or 1). Use #if CNET_HAVE_CURL:\n%s\n' "$ifdefs"
    fail=1
fi

# 2. Every <curl/curl.h> must sit directly under a #if CNET_HAVE_CURL.
for f in $(grep -rl 'curl/curl\.h' src include 2>/dev/null || true); do
    bad=$(awk '
        /#include[ \t]*<curl\/curl\.h>/ {
            if (prev !~ /^#if[ \t]+CNET_HAVE_CURL/) printf "%s:%d\n", FILENAME, NR
        }
        { prev = $0 }
    ' "$f")
    if [ -n "$bad" ]; then
        printf 'CURL_GUARD: FAIL - unguarded curl include:\n%s\n' "$bad"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    printf 'CURL_GUARD: FAIL\n'
    exit 1
fi
printf 'CURL_GUARD_PASS\n'
exit 0
