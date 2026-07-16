#!/usr/bin/env bash

set -Eeuo pipefail

# @file change_classification.sh
# @brief Classify exact GitHub event revisions for documentation-only CI routing.
# @note CI_CHANGE_EVENT, CI_CHANGE_BASE_SHA, and CI_CHANGE_HEAD_SHA are required
#   for automatic events. Every uncertainty emits run_integration=true.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=${CI_CHANGE_REPO_ROOT:-$(cd -- "$SCRIPT_DIR/../.." && pwd)}
CI_ARTIFACT_DIR=${CI_ARTIFACT_DIR:-"$REPO_ROOT/CI-results/change-classification"}

mkdir -p "$CI_ARTIFACT_DIR"

# @brief Emit one stable classifier value to its artifact and GitHub outputs.
# @param $1 Output key consumed by the workflow.
# @param $2 Single-line output value controlled by this script.
# @return The first failed write status, or zero.
# @throws Nothing; write failures terminate through set -e.
# @note Local callers without GITHUB_OUTPUT still receive classification.env.
emit_output() {
  local key=$1
  local value=$2
  printf '%s=%s\n' "$key" "$value" |
    tee -a "$CI_ARTIFACT_DIR/classification.env"
  if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    printf '%s=%s\n' "$key" "$value" >> "$GITHUB_OUTPUT"
  fi
}

# @brief Persist one final routing decision and its fail-closed reason.
# @param $1 Whether every changed path is documentation.
# @param $2 Stable reason token describing the decision.
# @return The first failed artifact or output write status, or zero.
# @throws Nothing; write failures terminate through set -e.
# @note run_integration is always the inverse of docs_only.
record_classification() {
  local docs_only=$1
  local reason=$2
  local run_integration=true
  if [[ "$docs_only" == true ]]; then
    run_integration=false
  fi

  : > "$CI_ARTIFACT_DIR/classification.env"
  emit_output docs_only "$docs_only"
  emit_output run_integration "$run_integration"
  emit_output reason "$reason"
  {
    printf 'Documentation-only change: %s\n' "$docs_only"
    printf 'Run build, CTest, and integration: %s\n' "$run_integration"
    printf 'Reason: %s\n' "$reason"
  } | tee "$CI_ARTIFACT_DIR/summary.log"
}

# @brief Check whether a supplied object ID is Git's all-zero sentinel.
# @param $1 Candidate object ID.
# @return Zero only for a nonempty string consisting entirely of zeroes.
# @throws Nothing; invalid input returns nonzero.
# @note GitHub uses this sentinel for pushes without a usable before commit.
is_zero_sha() {
  local value=${1:-}
  [[ -n "$value" && "$value" =~ ^0+$ ]]
}

# @brief Check whether a value is an explicit SHA-1 or SHA-256 object ID.
# @param $1 Candidate object ID.
# @return Zero only for a 40- or 64-digit hexadecimal value.
# @throws Nothing; invalid input returns nonzero.
# @note Symbolic refs are rejected so event payload identity stays authoritative.
is_explicit_sha() {
  local value=${1:-}
  [[ "$value" =~ ^([0-9a-fA-F]{40}|[0-9a-fA-F]{64})$ ]]
}

# @brief Decide whether one repository path is documentation-only content.
# @param $1 Repository-relative path emitted by git diff.
# @return Zero for docs/**, root Markdown, or a documented root text contract.
# @throws Nothing; malformed and unknown paths return nonzero.
# @note Nested Markdown outside docs/** remains build-relevant by default.
is_documentation_path() {
  local path=${1:-}
  local normalized
  [[ -n "$path" && "$path" != /* && "$path" != ../* &&
    "$path" != *$'\n'* ]] || return 1

  if [[ "$path" == docs/* ]]; then
    return 0
  fi
  if [[ "$path" == */* ]]; then
    return 1
  fi

  normalized=$(printf '%s' "$path" | LC_ALL=C tr '[:upper:]' '[:lower:]')
  case "$normalized" in
    *.md | *.markdown | readme | license | notice | changelog | contributing | \
      code_of_conduct | security)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

# @brief Route an uncertain or build-relevant change through full integration.
# @param $1 Stable reason token describing why the optimization is disabled.
# @return Zero after writing docs_only=false and run_integration=true.
# @throws Nothing; artifact write failures terminate through set -e.
# @note Detection uncertainty is a successful classification, not a skipped job.
require_full_integration() {
  local reason=$1
  record_classification false "$reason"
}

# @brief Classify a complete changed-path inventory against the docs whitelist.
# @param $@ Repository-relative paths, including both sides of renames.
# @return Zero after persisting the final routing decision.
# @throws Nothing; artifact write failures terminate through set -e.
# @note An empty inventory fails closed because it cannot prove docs-only scope.
classify_changed_paths() {
  local path
  local -a non_documentation_paths=()

  : > "$CI_ARTIFACT_DIR/changed-files.txt"
  : > "$CI_ARTIFACT_DIR/non-documentation-files.txt"
  if (($# == 0)); then
    require_full_integration empty-change-set
    return
  fi

  for path in "$@"; do
    printf '%s\n' "$path" >> "$CI_ARTIFACT_DIR/changed-files.txt"
    if ! is_documentation_path "$path"; then
      non_documentation_paths+=("$path")
      printf '%s\n' "$path" >> \
        "$CI_ARTIFACT_DIR/non-documentation-files.txt"
    fi
  done

  if ((${#non_documentation_paths[@]} == 0)); then
    record_classification true documentation-only
  else
    require_full_integration non-documentation-files
  fi
}

# @brief Select the exact tree base for the current GitHub event.
# @param $1 Event name: pull_request, pull_request_target, or push.
# @param $2 Explicit base or before SHA from the event payload.
# @param $3 Explicit head SHA from the event payload.
# @return The selected base SHA on stdout, or nonzero when it is unsafe.
# @throws Nothing; Git failures are represented by the return status.
# @note Pull requests require exactly one merge base; pushes compare before/head.
resolve_diff_base() {
  local event_name=$1
  local base_sha=$2
  local head_sha=$3
  local merge_base_file="$CI_ARTIFACT_DIR/merge-bases.txt"
  local -a merge_bases=()

  case "$event_name" in
    pull_request | pull_request_target)
      if ! git -C "$REPO_ROOT" merge-base --all "$base_sha" "$head_sha" \
        > "$merge_base_file"; then
        return 1
      fi
      mapfile -t merge_bases < "$merge_base_file"
      if ((${#merge_bases[@]} != 1)); then
        return 1
      fi
      printf '%s\n' "${merge_bases[0]}"
      ;;
    push)
      printf '%s\n' "$base_sha"
      ;;
    *)
      return 1
      ;;
  esac
}

# @brief Classify event payload commits without guessing a fallback revision.
# @return Zero after recording a docs-only or full-integration route.
# @throws Nothing; artifact write failures terminate through set -e.
# @note Manual dispatch and every Git uncertainty deliberately require full CI.
main() {
  local event_name=${CI_CHANGE_EVENT:-}
  local base_sha=${CI_CHANGE_BASE_SHA:-}
  local head_sha=${CI_CHANGE_HEAD_SHA:-}
  local shallow_state
  local diff_base
  local changed_path_file="$CI_ARTIFACT_DIR/changed-files.zlist"
  local -a changed_paths=()

  : > "$CI_ARTIFACT_DIR/changed-files.txt"
  : > "$CI_ARTIFACT_DIR/non-documentation-files.txt"
  : > "$CI_ARTIFACT_DIR/merge-bases.txt"

  if [[ "$event_name" == workflow_dispatch ]]; then
    require_full_integration workflow-dispatch
    return
  fi
  case "$event_name" in
    pull_request | pull_request_target | push) ;;
    *)
      require_full_integration unsupported-event
      return
      ;;
  esac
  if ! is_explicit_sha "$base_sha" || ! is_explicit_sha "$head_sha"; then
    require_full_integration missing-or-invalid-sha
    return
  fi
  if is_zero_sha "$base_sha" || is_zero_sha "$head_sha"; then
    require_full_integration zero-sha
    return
  fi
  if ! shallow_state=$(
    git -C "$REPO_ROOT" rev-parse --is-shallow-repository 2>/dev/null
  ); then
    require_full_integration repository-state-unavailable
    return
  fi
  if [[ "$shallow_state" != false ]]; then
    require_full_integration shallow-repository
    return
  fi
  if ! git -C "$REPO_ROOT" rev-parse --verify "$base_sha^{commit}" \
    >/dev/null 2>&1 ||
    ! git -C "$REPO_ROOT" rev-parse --verify "$head_sha^{commit}" \
      >/dev/null 2>&1; then
    require_full_integration commit-unavailable
    return
  fi
  if ! diff_base=$(resolve_diff_base "$event_name" "$base_sha" "$head_sha"); then
    require_full_integration merge-base-unavailable
    return
  fi
  if ! git -C "$REPO_ROOT" diff --no-renames --name-only -z \
    --diff-filter=ACMRD "$diff_base" "$head_sha" > "$changed_path_file"; then
    require_full_integration changed-path-detection-failed
    return
  fi

  mapfile -d '' -t changed_paths < "$changed_path_file"
  rm -f -- "$changed_path_file"
  classify_changed_paths "${changed_paths[@]}"
}

main "$@"
