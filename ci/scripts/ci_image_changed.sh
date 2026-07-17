#!/usr/bin/env bash

set -Eeuo pipefail

# @file ci_image_changed.sh
# @brief Detect whether the current comparison changes a maintained CI image input.
# @note CI workflows provide the exact pull-request base SHA after fetching it
#   from the base repository. Local callers may use the documented fallbacks or
#   CI_IMAGE_REPO_ROOT to exercise the detector in an isolated repository.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

REPO_ROOT=${CI_IMAGE_REPO_ROOT:-$REPO_ROOT}
cd "$REPO_ROOT"

# @brief Check whether a supplied object ID is Git's all-zero sentinel.
# @param $1 Candidate object ID.
# @return Zero only for a nonempty string consisting entirely of zeroes.
# @throws Nothing; invalid input returns nonzero.
# @note GitHub uses this sentinel for pushes without a usable before commit.
is_zero_sha() {
  local value=${1:-}
  [[ -n "$value" && "$value" =~ ^0+$ ]]
}

# @brief Write every changed path for CI image input detection as NUL records.
# @return Git's diff status for the first usable comparison.
# @throws Nothing; Git failures propagate through the returned status.
# @note The explicit base is preferred, followed by origin/main, HEAD~1, and the
#   working tree. Rename and status filtering stay disabled so deletions, type
#   changes, unknown statuses, and paths containing newlines remain visible.
changed_files() {
  local base=${CI_IMAGE_BASE_REF:-${CI_BASE_REF:-}}
  if [[ -n "$base" ]] && ! is_zero_sha "$base" &&
    git rev-parse --verify "$base^{commit}" >/dev/null 2>&1; then
    git diff --no-renames --name-only -z "$base"...HEAD
    return
  fi
  if git rev-parse --verify origin/main >/dev/null 2>&1; then
    git diff --no-renames --name-only -z origin/main...HEAD
    return
  fi
  if git rev-parse --verify HEAD~1 >/dev/null 2>&1; then
    git diff --no-renames --name-only -z HEAD~1...HEAD
    return
  fi
  git diff --no-renames --name-only -z HEAD
}

changed_path_file=$(mktemp "$CI_ARTIFACT_DIR/.changed-files.XXXXXX")
trap 'rm -f -- "$changed_path_file"' EXIT
if ! changed_files > "$changed_path_file"; then
  echo "CI image changed-path detection failed." >&2
  exit 1
fi
mapfile -d '' -t changed_paths < "$changed_path_file"
changed_file_log="$CI_ARTIFACT_DIR/changed-files.txt"
: > "$changed_file_log"
if ((${#changed_paths[@]} > 0)); then
  printf '%q\n' "${changed_paths[@]}" > "$changed_file_log"
fi

image_changed=false
for path in "${changed_paths[@]}"; do
  case "$path" in
    Dockerfile.ci | .dockerignore | .github/workflows/build-ci-image.yml)
      image_changed=true
      ;;
  esac
done

printf 'changed=%s\n' "$image_changed" | tee "$CI_ARTIFACT_DIR/ci-image-change.env"
if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
  printf 'changed=%s\n' "$image_changed" >> "$GITHUB_OUTPUT"
fi
