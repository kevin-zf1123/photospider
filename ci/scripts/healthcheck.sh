#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

changed_files() {
  local base=${CI_BASE_REF:-}
  if [[ -n "$base" ]] && git rev-parse --verify "$base" >/dev/null 2>&1; then
    git diff --name-only --diff-filter=ACMR "$base"...HEAD
    return
  fi
  if git rev-parse --verify origin/main >/dev/null 2>&1; then
    git diff --name-only --diff-filter=ACMR origin/main...HEAD
    return
  fi
  if git rev-parse --verify HEAD~1 >/dev/null 2>&1; then
    git diff --name-only --diff-filter=ACMR HEAD~1...HEAD
    return
  fi
  git diff --name-only --diff-filter=ACMR HEAD
}

mapfile -t cpp_files < <(
  changed_files |
    awk '/\.(c|cc|cpp|cxx|h|hpp|hh|mm)$/ && !/^extern\// { print }' |
    sort -u
)

run_logged git_diff_check git diff --check

if ((${#cpp_files[@]} == 0)); then
  echo "No changed C++ files; clang-format and cpplint skipped." |
    tee "$CI_ARTIFACT_DIR/static-summary.log"
  exit 0
fi

printf '%s\n' "${cpp_files[@]}" | tee "$CI_ARTIFACT_DIR/changed-cpp-files.txt"

run_logged clang_format_check clang-format --dry-run --Werror "${cpp_files[@]}"
run_logged cpplint_check python3 -m cpplint --extensions=mm,cpp,cxx,cc,h,hpp "${cpp_files[@]}"
