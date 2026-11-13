#!/bin/sh
set -eu
capi_repo=$(git rev-parse --show-toplevel)
cd "$capi_repo"
capi_existing=$(git config --get core.hooksPath || :)
if [ -n "$capi_existing" ] && [ "$capi_existing" != .githooks ]; then
    printf 'capi: existing core.hooksPath is %s; keep it and integrate .githooks/pre-commit manually.\n' "$capi_existing" >&2
    exit 1
fi
capi_default=$(git rev-parse --git-path hooks/pre-commit)
if [ -z "$capi_existing" ] && [ -x "$capi_default" ]; then
    printf 'capi: an executable hook already exists at %s; integrate .githooks/pre-commit without replacing it.\n' "$capi_default" >&2
    exit 1
fi
if [ ! -x .githooks/pre-commit ] || [ ! -x tools/check.sh ]; then
    echo 'capi: missing executable hook/check script' >&2; exit 1
fi
git config --local core.hooksPath .githooks
printf 'capi: enabled local pre-commit checks (default: full).\n'
printf 'Quick mode: git config --local capi.preCommitMode quick\n'
printf 'Remove hook: git config --local --unset core.hooksPath\n'
