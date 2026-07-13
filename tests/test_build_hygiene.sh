#!/usr/bin/env bash
# Fresh-clone/build-hygiene regression gate. No network or model/GPU work.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
fail() { printf 'BUILD_HYGIENE_FAIL: %s\n' "$1" >&2; exit 1; }

# Scripts executed directly by acceptance targets must be executable in Git.
for script in scripts/run_cnet_ds4_dual.sh; do
    mode=$(git ls-files --stage -- "$script" | awk '{print $1}')
    [[ "$mode" == 100755 ]] ||
        fail "directly executed script has git mode ${mode:-untracked}, expected 100755: $script"
done

# Validate the current clean recipe in a disposable worktree. Copy tracked
# working-tree edits first so RED/GREEN works before the implementation commit.
TMP=$(mktemp -d)
WT="$TMP/wt"
cleanup() {
    git worktree remove --force "$WT" >/dev/null 2>&1 || true
    rm -rf "$TMP"
}
trap cleanup EXIT
git worktree add --detach "$WT" HEAD >/dev/null 2>&1 ||
    fail "could not create disposable clean-test worktree"
while IFS= read -r file; do
    [[ -n "$file" && -f "$file" ]] || continue
    mkdir -p "$WT/$(dirname "$file")"
    cp -p "$file" "$WT/$file"
done < <(git diff HEAD --name-only)
before=$(cd "$WT" && git status --short)
(cd "$WT" && make clean >/dev/null)
after=$(cd "$WT" && git status --short)
[[ "$after" == "$before" ]] ||
    fail "make clean changed tracked files; before=[$before] after=[$after]"

# Managed dependency selection must be exact, not NuGet's warning-producing
# substitution from unavailable 0.1.0 to another version.
grep -q 'PackageReference Include="ModelContextProtocol" Version="1.0.0"' \
    dotnet/CnetMcpServer/CnetMcpServer.csproj ||
    fail "ModelContextProtocol must be pinned to the verified 1.0.0 package"

# Dotnet discovery must be portable and the restore gate must cover every
# project consumed by `make unified --no-restore`.
if grep -qE '/home/marble[^[:space:]]*/dotnet' Makefile; then
    fail "Makefile contains a machine-specific dotnet path"
fi
for token in 'DOTNET ?=' 'DOTNET_ROOT' '\$\$HOME/dotnet/dotnet' \
             'dotnet_restore:' 'dotnet/CceHost/CceHost.csproj' \
             'dotnet/CnetMcpServer/CnetMcpServer.csproj' \
             'dotnet/Cce.Tests/Cce.Tests.csproj'; do
    grep -q "$token" Makefile || fail "managed restore/discovery token missing: $token"
done

printf 'BUILD_HYGIENE_PASS\n'
