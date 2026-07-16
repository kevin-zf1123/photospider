#!/usr/bin/env bash

set -Eeuo pipefail

# @file change_classification_test.sh
# @brief Validate the durable event and path contract of change classification.
# @note All repositories and artifacts are isolated below a temporary root.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
CLASSIFIER="$SCRIPT_DIR/change_classification.sh"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/photospider-change-classification.XXXXXX")
CASE_REPO=
CASE_ARTIFACT_DIR=
BASE_SHA=
HEAD_SHA=
pass_count=0

# @brief Remove all temporary Git repositories created by this regression.
# @return Zero after successful removal, or rm's failure status.
# @throws Nothing; cleanup status is returned to the EXIT trap.
# @note No repository-owned files are created or modified by the test.
cleanup() {
  rm -rf -- "$TEST_ROOT"
}
trap cleanup EXIT

# @brief Fail the current regression with a stable diagnostic.
# @param $@ Human-readable failure detail.
# @return Does not return; exits the test with status one.
# @throws Nothing.
# @note The caller's case artifact remains until the EXIT cleanup runs.
fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

# @brief Create an isolated Git repository with one neutral baseline commit.
# @param $1 Stable case name used for temporary paths.
# @return Zero after exporting CASE_REPO, CASE_ARTIFACT_DIR, and BASE_SHA.
# @throws Nothing; Git and filesystem failures terminate through set -e.
# @note The neutral baseline file is unchanged by every classification case.
new_case_repo() {
  local case_name=$1
  CASE_REPO="$TEST_ROOT/$case_name/repo"
  CASE_ARTIFACT_DIR="$TEST_ROOT/$case_name/artifacts"
  mkdir -p "$CASE_REPO" "$CASE_ARTIFACT_DIR"
  git -C "$CASE_REPO" init -q
  git -C "$CASE_REPO" config user.name "CI Classification Test"
  git -C "$CASE_REPO" config user.email "ci-classification@example.invalid"
  printf 'baseline\n' > "$CASE_REPO/seed.txt"
  git -C "$CASE_REPO" add seed.txt
  git -C "$CASE_REPO" commit -qm "baseline"
  BASE_SHA=$(git -C "$CASE_REPO" rev-parse HEAD)
  HEAD_SHA=$BASE_SHA
}

# @brief Commit all current case changes and update HEAD_SHA.
# @param $1 Stable commit summary for regression diagnostics.
# @return Zero after creating the commit and exporting HEAD_SHA.
# @throws Nothing; Git failures terminate through set -e.
# @note Deletions and both sides of renames are included through git add -A.
commit_case() {
  local summary=$1
  git -C "$CASE_REPO" add -A
  git -C "$CASE_REPO" commit -qm "$summary"
  HEAD_SHA=$(git -C "$CASE_REPO" rev-parse HEAD)
}

# @brief Run the production classifier for one synthetic GitHub event.
# @param $1 Event name.
# @param $2 Base or before SHA.
# @param $3 Head SHA.
# @param $4 Branch name for push events, or an empty value for other events.
# @return The classifier process status.
# @throws Nothing; classifier failure propagates through set -e.
# @note Each run starts with an empty artifact directory.
run_classifier() {
  local event_name=$1
  local base_sha=$2
  local head_sha=$3
  local branch_name=${4-}
  rm -rf -- "$CASE_ARTIFACT_DIR"
  mkdir -p "$CASE_ARTIFACT_DIR"
  CI_CHANGE_REPO_ROOT="$CASE_REPO" \
    CI_ARTIFACT_DIR="$CASE_ARTIFACT_DIR" \
    CI_CHANGE_EVENT="$event_name" \
    CI_CHANGE_BRANCH="$branch_name" \
    CI_CHANGE_BASE_SHA="$base_sha" \
    CI_CHANGE_HEAD_SHA="$head_sha" \
    bash "$CLASSIFIER" >/dev/null
}

# @brief Require exact classifier outputs and a stable reason token.
# @param $1 Human-readable case name.
# @param $2 Expected docs_only value.
# @param $3 Expected run_integration value.
# @param $4 Expected reason token.
# @return Zero after recording a passing case.
# @throws Nothing; mismatches exit through fail.
# @note Assertions read the durable artifact consumed during CI diagnosis.
assert_classification() {
  local case_name=$1
  local expected_docs_only=$2
  local expected_run_integration=$3
  local expected_reason=$4
  local output_file="$CASE_ARTIFACT_DIR/classification.env"

  [[ -f "$output_file" ]] || fail "$case_name did not write classification.env"
  grep -Fqx "docs_only=$expected_docs_only" "$output_file" ||
    fail "$case_name docs_only mismatch"
  grep -Fqx "run_integration=$expected_run_integration" "$output_file" ||
    fail "$case_name run_integration mismatch"
  grep -Fqx "reason=$expected_reason" "$output_file" ||
    fail "$case_name reason mismatch"
  printf 'PASS: %s\n' "$case_name"
  pass_count=$((pass_count + 1))
}

# @brief Exercise the durable path and Git-event routing contract.
# @return Zero when every classification case passes.
# @throws Nothing; command failures terminate through set -e.
# @note The matrix covers docs, mixed and type changes, deletes, renames,
#   CI-branch push routing, merge-base behavior, and uncertainty.
main() {
  local common_sha
  local missing_sha=ffffffffffffffffffffffffffffffffffffffff
  local shallow_source
  local -a second_push_paths=()

  new_case_repo documentation_paths
  mkdir -p "$CASE_REPO/docs/CI/zh"
  printf 'example: true\n' > "$CASE_REPO/docs/CI/zh/example.yaml"
  printf '# readme\n' > "$CASE_REPO/readme.md"
  printf '# context\n' > "$CASE_REPO/CONTEXT.md"
  printf 'readme\n' > "$CASE_REPO/README"
  printf 'license\n' > "$CASE_REPO/LICENSE"
  commit_case "documentation only"
  run_classifier push "$BASE_SHA" "$HEAD_SHA" main
  assert_classification documentation-paths true false documentation-only

  new_case_repo source_change
  mkdir -p "$CASE_REPO/src"
  printf 'int main() {}\n' > "$CASE_REPO/src/main.cpp"
  commit_case "source change"
  run_classifier push "$BASE_SHA" "$HEAD_SHA" main
  assert_classification source-change false true non-documentation-files

  new_case_repo mixed_change
  mkdir -p "$CASE_REPO/docs" "$CASE_REPO/tests"
  printf '# guide\n' > "$CASE_REPO/docs/guide.md"
  printf 'test\n' > "$CASE_REPO/tests/input.txt"
  commit_case "mixed change"
  run_classifier push "$BASE_SHA" "$HEAD_SHA" main
  assert_classification mixed-change false true non-documentation-files

  new_case_repo mixed_type_change
  mkdir -p "$CASE_REPO/docs" "$CASE_REPO/src"
  printf '# guide\n' > "$CASE_REPO/docs/guide.md"
  printf 'build input\n' > "$CASE_REPO/src/build-input.txt"
  commit_case "add type-change inputs"
  BASE_SHA=$HEAD_SHA
  rm -- "$CASE_REPO/src/build-input.txt"
  ln -s ../docs/guide.md "$CASE_REPO/src/build-input.txt"
  printf '# updated guide\n' > "$CASE_REPO/docs/guide.md"
  commit_case "documentation and source type change"
  git -C "$CASE_REPO" diff --no-renames --name-status \
    "$BASE_SHA" "$HEAD_SHA" |
    grep -Fqx $'T\tsrc/build-input.txt' ||
    fail "mixed-type-change did not create a Git T status"
  run_classifier push "$BASE_SHA" "$HEAD_SHA" main
  assert_classification mixed-type-change false true non-documentation-files
  grep -Fqx 'src/build-input.txt' "$CASE_ARTIFACT_DIR/changed-files.txt" ||
    fail "mixed-type-change omitted the type-changed source path"

  new_case_repo workflow_change
  mkdir -p "$CASE_REPO/.github/workflows"
  printf 'name: test\n' > "$CASE_REPO/.github/workflows/test.yml"
  commit_case "workflow change"
  run_classifier push "$BASE_SHA" "$HEAD_SHA" main
  assert_classification workflow-change false true non-documentation-files

  new_case_repo ci_branch_second_push
  mkdir -p "$CASE_REPO/src"
  printf 'first push\n' > "$CASE_REPO/src/first-push.cpp"
  commit_case "first CI branch push changes source"
  BASE_SHA=$HEAD_SHA
  mkdir -p "$CASE_REPO/docs"
  printf '# second push\n' > "$CASE_REPO/docs/second-push.md"
  commit_case "second CI branch push changes only docs"
  mapfile -t second_push_paths < <(
    git -C "$CASE_REPO" diff --no-renames --name-only "$BASE_SHA" "$HEAD_SHA"
  )
  ((${#second_push_paths[@]} == 1)) &&
    [[ "${second_push_paths[0]}" == docs/second-push.md ]] ||
    fail "ci-branch-second-push fixture is not documentation-only"
  run_classifier push "$BASE_SHA" "$HEAD_SHA" CI/repeated-push
  assert_classification ci-branch-second-push false true ci-branch-push

  new_case_repo renamed_source
  mkdir -p "$CASE_REPO/src"
  printf 'int value;\n' > "$CASE_REPO/src/value.cpp"
  commit_case "add source"
  BASE_SHA=$HEAD_SHA
  mkdir -p "$CASE_REPO/docs"
  git -C "$CASE_REPO" mv src/value.cpp docs/value.cpp
  commit_case "rename source into docs"
  run_classifier push "$BASE_SHA" "$HEAD_SHA" main
  assert_classification renamed-source false true non-documentation-files
  grep -Fqx 'src/value.cpp' "$CASE_ARTIFACT_DIR/non-documentation-files.txt" ||
    fail "renamed-source did not expose the deleted source path"

  new_case_repo deleted_source
  mkdir -p "$CASE_REPO/plugins"
  printf 'plugin\n' > "$CASE_REPO/plugins/provider.cpp"
  commit_case "add plugin"
  BASE_SHA=$HEAD_SHA
  rm -- "$CASE_REPO/plugins/provider.cpp"
  commit_case "delete plugin"
  run_classifier push "$BASE_SHA" "$HEAD_SHA" main
  assert_classification deleted-source false true non-documentation-files

  new_case_repo pull_request_merge_base
  common_sha=$BASE_SHA
  git -C "$CASE_REPO" checkout -qb pr-head
  mkdir -p "$CASE_REPO/docs"
  printf '# pull request\n' > "$CASE_REPO/docs/pull-request.md"
  commit_case "pull request docs"
  local pr_head_sha=$HEAD_SHA
  git -C "$CASE_REPO" checkout -qb target "$common_sha"
  mkdir -p "$CASE_REPO/src"
  printf 'target change\n' > "$CASE_REPO/src/target.cpp"
  commit_case "target branch source"
  local target_sha=$HEAD_SHA
  run_classifier pull_request_target "$target_sha" "$pr_head_sha"
  assert_classification pull-request-merge-base true false documentation-only

  new_case_repo zero_before
  mkdir -p "$CASE_REPO/docs"
  printf '# new branch\n' > "$CASE_REPO/docs/new-branch.md"
  commit_case "new branch docs"
  run_classifier push 0000000000000000000000000000000000000000 \
    "$HEAD_SHA" main
  assert_classification zero-before false true zero-sha

  new_case_repo zero_after
  run_classifier push "$BASE_SHA" \
    0000000000000000000000000000000000000000 main
  assert_classification zero-after false true zero-sha

  new_case_repo missing_before
  run_classifier push '' "$HEAD_SHA" main
  assert_classification missing-before false true missing-or-invalid-sha

  new_case_repo missing_push_branch
  run_classifier push "$BASE_SHA" "$HEAD_SHA" ''
  assert_classification missing-push-branch false true \
    missing-or-invalid-branch

  new_case_repo unavailable_commit
  run_classifier push "$missing_sha" "$HEAD_SHA" main
  assert_classification unavailable-commit false true commit-unavailable

  new_case_repo manual_dispatch
  run_classifier workflow_dispatch '' "$HEAD_SHA"
  assert_classification workflow-dispatch false true workflow-dispatch

  new_case_repo empty_change_set
  run_classifier push "$BASE_SHA" "$BASE_SHA" main
  assert_classification empty-change-set false true empty-change-set

  new_case_repo shallow_repository
  mkdir -p "$CASE_REPO/docs"
  printf '# shallow\n' > "$CASE_REPO/docs/shallow.md"
  commit_case "shallow docs"
  shallow_source=$CASE_REPO
  CASE_REPO="$TEST_ROOT/shallow_repository/clone"
  git clone -q --depth 1 "file://$shallow_source" "$CASE_REPO"
  CASE_ARTIFACT_DIR="$TEST_ROOT/shallow_repository/shallow-artifacts"
  run_classifier push "$BASE_SHA" "$HEAD_SHA" main
  assert_classification shallow-repository false true shallow-repository

  printf 'All %d change-classification cases passed.\n' "$pass_count"
}

main "$@"
