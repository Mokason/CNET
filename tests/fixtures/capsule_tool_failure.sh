#!/usr/bin/env bash
# Fault-injection oracle: planning succeeds but the evidence process fails.
case ${1:-} in
    validate) exit 0 ;;
    plan) printf 'alpha delta 1 1 mul 1 0 0 0 0\n' ;;
    *) exit 2 ;;
esac
