#!/bin/sh
# Real Git indexes + a tiny check stub exercise hook behavior without rebuilding.
set -eu
capi_source=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
for capi_git_variable in $(git rev-parse --local-env-vars); do unset "$capi_git_variable"; done
GIT_CONFIG_NOSYSTEM=1
GIT_CONFIG_GLOBAL=/dev/null
export GIT_CONFIG_NOSYSTEM GIT_CONFIG_GLOBAL
capi_scratch=$(mktemp -d "${TMPDIR:-/tmp}/capi-hooks.XXXXXX")
trap 'rm -rf "$capi_scratch"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
capi_fixture="$capi_scratch/repo with spaces"
git init --quiet --template= "$capi_fixture"
cd "$capi_fixture"
mkdir tools .githooks
cp "$capi_source/.githooks/pre-commit" .githooks/pre-commit
cp "$capi_source/tools/install-hooks.sh" tools/install-hooks.sh
printf '/build*/\n' > .gitignore
printf 'staged\n' > marker
cat > tools/check.sh <<'CHECK'
#!/bin/sh
set -eu
test "$(cat "$(dirname "$0")/../marker")" = staged
test ! -e "$(dirname "$0")/../untracked"
test -z "${GIT_INDEX_FILE:-}"
printf 'checked mode: %s\n' "$1"
CHECK
chmod +x .githooks/pre-commit tools/check.sh tools/install-hooks.sh
git add .
capi_before=$(git write-tree)
printf 'unstaged\n' > marker
printf 'untracked\n' > untracked
printf '#!/bin/sh\nexit 91\n' > tools/check.sh
git diff --binary > "$capi_scratch/before.diff"
CAPI_CHECK_MODE=quick .githooks/pre-commit
grep -q 'checked mode: quick' build-pre-commit/last-run.log
test "$(git write-tree)" = "$capi_before"
test "$(cat marker)" = unstaged
test "$(cat untracked)" = untracked
git diff --binary > "$capi_scratch/after.diff"
cmp "$capi_scratch/before.diff" "$capi_scratch/after.diff"
# The commit's alternate index is honored for export, then cleared for checks.
GIT_INDEX_FILE="$capi_scratch/index" git read-tree "$capi_before"
GIT_INDEX_FILE="$capi_scratch/index" CAPI_CHECK_MODE=full .githooks/pre-commit
grep -q 'checked mode: full' build-pre-commit/last-run.log
# A failed staged checker blocks the hook even when its working copy succeeds.
printf '#!/bin/sh\nexit 23\n' > tools/check.sh
git add tools/check.sh
printf '#!/bin/sh\nexit 0\n' > tools/check.sh
if CAPI_CHECK_MODE=quick .githooks/pre-commit; then echo 'failed checks did not block' >&2; exit 1; fi
test ! -d build-pre-commit/lock
# A successful check that changes the index must also block the commit.
cat > tools/check.sh <<'CHECK'
#!/bin/sh
set -eu
printf 'changed by test\n' > "$CAPI_HOOK_TEST_REPO/marker"
git -C "$CAPI_HOOK_TEST_REPO" add marker
CHECK
git add tools/check.sh
if CAPI_CHECK_MODE=quick CAPI_HOOK_TEST_REPO="$capi_fixture" .githooks/pre-commit; then echo 'changed index did not block' >&2; exit 1; fi
# Lock contention and hook installation conflicts fail without replacement.
mkdir build-pre-commit/lock
if .githooks/pre-commit; then echo 'concurrent hook was allowed' >&2; exit 1; fi
rmdir build-pre-commit/lock
git config --local core.hooksPath other-hooks
if tools/install-hooks.sh; then echo 'existing hooksPath was replaced' >&2; exit 1; fi
test "$(git config --get core.hooksPath)" = other-hooks
git config --local --unset core.hooksPath
mkdir -p .git/hooks
printf '#!/bin/sh\nexit 0\n' > .git/hooks/pre-commit
chmod +x .git/hooks/pre-commit
if tools/install-hooks.sh; then echo 'existing hook was shadowed' >&2; exit 1; fi
rm .git/hooks/pre-commit
tools/install-hooks.sh
tools/install-hooks.sh
test "$(git config --get core.hooksPath)" = .githooks
printf 'Git hooks: staged snapshot, alternate index, failure, index changes, locking, and safe installation passed\n'
