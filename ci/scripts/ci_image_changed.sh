#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

is_zero_sha() {
  local value=${1:-}
  [[ -n "$value" && "$value" =~ ^0+$ ]]
}

changed_files() {
  local base=${CI_IMAGE_BASE_REF:-${CI_BASE_REF:-}}
  if [[ -n "$base" ]] && ! is_zero_sha "$base" &&
    git rev-parse --verify "$base^{commit}" >/dev/null 2>&1; then
    git diff --name-only --diff-filter=ACMRD "$base"...HEAD
    return
  fi
  if git rev-parse --verify origin/main >/dev/null 2>&1; then
    git diff --name-only --diff-filter=ACMRD origin/main...HEAD
    return
  fi
  if git rev-parse --verify HEAD~1 >/dev/null 2>&1; then
    git diff --name-only --diff-filter=ACMRD HEAD~1...HEAD
    return
  fi
  git diff --name-only --diff-filter=ACMRD HEAD
}

mapfile -t changed_paths < <(changed_files | sort -u)
changed_file_log="$CI_ARTIFACT_DIR/changed-files.txt"
: > "$changed_file_log"
if ((${#changed_paths[@]} > 0)); then
  printf '%s\n' "${changed_paths[@]}" > "$changed_file_log"
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
