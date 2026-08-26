#!/usr/bin/env bash

set -Eeuo pipefail

# @file ci_image_changed.sh
# @brief Detect whether the current comparison changes a maintained CI image input.
# @note CI workflows provide the exact pull-request base SHA after fetching it
#   from the base repository. Local callers may use the documented fallbacks or
#   CI_IMAGE_REPO_ROOT to exercise the detector in an isolated repository. The
#   sole path authority is the strictly parsed canonical lock at both comparison
#   revisions; this script intentionally owns no parallel path list.

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

# @brief Select the first usable comparison without classifying any path.
# @return Prints `base<TAB>head` or `base<TAB>worktree` for the manifest helper.
# @throws Nothing; Git reference probes return false and continue to the next
#   documented fallback.
# @note The Python reader resolves the merge base, strictly parses both lock
#   revisions, creates the NUL-safe diff, and classifies it from their union.
select_comparison() {
  local base=${CI_IMAGE_BASE_REF:-${CI_BASE_REF:-}}
  if [[ -n "$base" ]] && ! is_zero_sha "$base"; then
    if git rev-parse --verify "$base^{commit}" >/dev/null 2>&1; then
      printf '%s\t%s\n' "$base" HEAD
      return
    fi
    echo "Explicit CI image comparison base is unavailable: $base" >&2
    return 1
  fi
  if git rev-parse --verify origin/main >/dev/null 2>&1; then
    printf '%s\t%s\n' origin/main HEAD
    return
  fi
  if git rev-parse --verify HEAD~1 >/dev/null 2>&1; then
    printf '%s\t%s\n' HEAD~1 HEAD
    return
  fi
  printf '%s\t%s\n' HEAD worktree
}

if ! comparison=$(select_comparison); then
  echo "CI image comparison selection failed." >&2
  echo "CI image changed-path detection failed." >&2
  exit 1
fi
IFS=$'\t' read -r comparison_base comparison_head <<<"$comparison"
if [[ -z "$comparison_base" ||
  ("$comparison_head" != HEAD && "$comparison_head" != worktree) ]]; then
  echo "CI image comparison selection is malformed." >&2
  exit 1
fi
changed_file_log=$CI_ARTIFACT_DIR/changed-files.txt
detector_arguments=(
  --repo-root "$REPO_ROOT"
  detect-changed
  --base "$comparison_base"
  --changed-files-output "$changed_file_log"
)
if [[ "$comparison_head" == worktree ]]; then
  detector_arguments+=(--worktree)
else
  detector_arguments+=(--head "$comparison_head")
fi
if ! image_changed=$(python3 "$SCRIPT_DIR/ci_image_manifest.py" \
  "${detector_arguments[@]}"); then
  echo "CI image changed-path detection failed." >&2
  exit 1
fi
case "$image_changed" in
  true | false) ;;
  *)
    echo "CI image manifest detector returned an invalid route value." >&2
    exit 1
    ;;
esac

printf 'changed=%s\n' "$image_changed" | tee "$CI_ARTIFACT_DIR/ci-image-change.env"
if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
  printf 'changed=%s\n' "$image_changed" >> "$GITHUB_OUTPUT"
fi
