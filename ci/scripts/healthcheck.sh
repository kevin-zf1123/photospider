#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

is_valid_ref() {
  local ref=${1:-}
  [[ -n "$ref" ]] && git rev-parse --verify "$ref^{commit}" >/dev/null 2>&1
}

diff_base_ref() {
  local base=${CI_BASE_REF:-}
  if is_valid_ref "$base"; then
    printf '%s\n' "$base"
    return
  fi
  if is_valid_ref origin/main; then
    printf '%s\n' origin/main
    return
  fi
  if is_valid_ref HEAD~1; then
    printf '%s\n' HEAD~1
    return
  fi
  return 1
}

changed_files() {
  local base
  if base=$(diff_base_ref); then
    git diff --name-only --diff-filter=ACMR "$base"...HEAD
    return
  fi
  git diff --name-only --diff-filter=ACMR HEAD
}

mapfile -t cpp_files < <(
  changed_files |
    awk '/\.(c|cc|cpp|cxx|h|hpp|hh|mm)$/ && !/^extern\// { print }' |
    sort -u
)

if diff_base=$(diff_base_ref); then
  run_logged git_diff_check git diff --check "$diff_base"...HEAD
else
  run_logged git_diff_check git diff --check HEAD
fi

if ((${#cpp_files[@]} == 0)); then
  echo "No changed C++ files; clang-format and cpplint skipped." |
    tee "$CI_ARTIFACT_DIR/static-summary.log"
  exit 0
fi

printf '%s\n' "${cpp_files[@]}" | tee "$CI_ARTIFACT_DIR/changed-cpp-files.txt"

run_logged clang_format_check clang-format --dry-run --Werror "${cpp_files[@]}"
run_logged cpplint_check python3 -m cpplint --extensions=mm,cpp,cxx,cc,h,hpp "${cpp_files[@]}"
