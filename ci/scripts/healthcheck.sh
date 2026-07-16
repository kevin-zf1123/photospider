#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

# @brief Check whether a candidate ref resolves to a commit.
# @param $1 Candidate Git ref.
# @return Zero only when the nonempty ref resolves to a commit.
# @throws Nothing; invalid input returns nonzero.
# @note This helper is for static-check scope, not CI routing classification.
is_valid_ref() {
  local ref=${1:-}
  [[ -n "$ref" ]] && git rev-parse --verify "$ref^{commit}" >/dev/null 2>&1
}

# @brief Select the best available baseline for changed-file static checks.
# @return The chosen ref on stdout, or nonzero when no baseline exists.
# @throws Nothing; unavailable refs are represented by the return status.
# @note CI_BASE_REF is preferred, followed by origin/main and HEAD~1.
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

# @brief Write every nondeleted changed path for static checking as NUL records.
# @return Git's diff status.
# @throws Nothing; Git failures propagate to the caller.
# @note Deletions alone are excluded because formatters require a current file;
#   type changes, uncommon statuses, and paths containing newlines remain
#   visible. A working-tree diff is used only when no commit baseline exists.
changed_files() {
  local base
  if base=$(diff_base_ref); then
    git diff --no-renames --name-only --diff-filter=d -z "$base"...HEAD
    return
  fi
  git diff --no-renames --name-only --diff-filter=d -z HEAD
}

changed_path_file=$(mktemp "$CI_ARTIFACT_DIR/.changed-files.XXXXXX")
trap 'rm -f -- "$changed_path_file"' EXIT
if ! changed_files > "$changed_path_file"; then
  echo "Healthcheck changed-path detection failed." >&2
  exit 1
fi
mapfile -d '' -t changed_paths < "$changed_path_file"
cpp_files=()
for path in "${changed_paths[@]}"; do
  case "$path" in
    extern/*) ;;
    *.c | *.cc | *.cpp | *.cxx | *.h | *.hpp | *.hh | *.mm)
      cpp_files+=("$path")
      ;;
  esac
done

if diff_base=$(diff_base_ref); then
  run_logged git_diff_check git diff --check "$diff_base"...HEAD
else
  run_logged git_diff_check git diff --check HEAD
fi

run_logged change_classification_test \
  bash "$SCRIPT_DIR/change_classification_test.sh"
run_logged ci_routing_test bash "$SCRIPT_DIR/ci_routing_test.sh"

if ((${#cpp_files[@]} == 0)); then
  echo "No changed C++ files; clang-format and cpplint skipped." |
    tee "$CI_ARTIFACT_DIR/static-summary.log"
  exit 0
fi

printf '%q\n' "${cpp_files[@]}" |
  tee "$CI_ARTIFACT_DIR/changed-cpp-files.txt"

run_logged clang_format_check clang-format --dry-run --Werror "${cpp_files[@]}"
run_logged cpplint_check python3 -m cpplint --extensions=mm,cpp,cxx,cc,h,hpp "${cpp_files[@]}"
