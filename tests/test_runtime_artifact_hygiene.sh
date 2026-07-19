#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

runtime_tracked=(
  cnet_knowledge_base.bin
  cnet_mcp_facts.bin
  soul_gemma4v2_final.cnb.gaps.txt
  soul_gemma4v2.int8head.gaps.bak
)
for path in "${runtime_tracked[@]}"; do
  if git ls-files --error-unmatch -- "$path" >/dev/null 2>&1; then
    echo "RUNTIME_ARTIFACT_HYGIENE_FAIL tracked=$path" >&2
    exit 1
  fi
done

# Any tracked path matched by .gitignore is a policy contradiction except the
# the intentionally versioned root Linux release library.
while IFS= read -r path; do
  case "$path" in
    cnet.so) ;;
    *) echo "RUNTIME_ARTIFACT_HYGIENE_FAIL tracked_ignored=$path" >&2; exit 1 ;;
  esac
done < <(git ls-files -ci --exclude-standard)

# Release policy intentionally versions the deterministic default shared
# library; build-only clones must receive the accepted ABI artifact.
if ! git ls-files --error-unmatch -- cnet.so >/dev/null 2>&1; then
  echo "RUNTIME_ARTIFACT_HYGIENE_FAIL cnet.so must remain tracked" >&2
  exit 1
fi

ignored_runtime=(
  cnet.so
  cnet_knowledge_base.bin
  cnet_mcp_facts.bin
  soul_gemma4v2_final.cnb.gaps.txt
  cnet_knowledge_base.bin.prev
  kv_archive/ledger.tsv
  arbitrary.pre-oracle-fix-20260718.bak
)
for path in "${ignored_runtime[@]}"; do
  if ! git check-ignore --no-index -q -- "$path"; then
    echo "RUNTIME_ARTIFACT_HYGIENE_FAIL not_ignored=$path" >&2
    exit 1
  fi
done

required_source=(
  include/cce/cce_gguf_tok.h
  src/cce/cce_gguf_tok.c
  tests/test_gguf_tok.c
)
for path in "${required_source[@]}"; do
  if [ ! -f "$path" ]; then
    echo "RUNTIME_ARTIFACT_HYGIENE_FAIL missing_source=$path" >&2
    exit 1
  fi
done

echo "RUNTIME_ARTIFACT_HYGIENE_PASS"
