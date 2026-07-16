#!/usr/bin/env bash

set -Eeuo pipefail

# @file ci_routing_test.sh
# @brief Regress trusted CI-branch routing and changed-path failure propagation.
# @note The test reads maintained workflows, executes their stable gate blocks,
#   and uses isolated Git repositories. It is a long-lived healthcheck helper,
#   not a CTest entry or an issue-specific replay script.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
IMAGE_DETECTOR="$SCRIPT_DIR/ci_image_changed.sh"
HEALTHCHECK="$SCRIPT_DIR/healthcheck.sh"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/photospider-ci-routing.XXXXXX")
REAL_GIT=$(command -v git)
pass_count=0

# @brief Remove all temporary workflows, artifacts, shims, and repositories.
# @return Zero after successful removal, or rm's failure status.
# @throws Nothing; cleanup status is returned to the EXIT trap.
# @note No repository-owned output survives this regression.
cleanup() {
  rm -rf -- "$TEST_ROOT"
}
trap cleanup EXIT

# @brief Fail the regression with one stable diagnostic.
# @param $@ Human-readable failure detail.
# @return Does not return; exits the test with status one.
# @throws Nothing.
# @note Temporary evidence remains available until the EXIT trap runs.
fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

# @brief Record one passing contract assertion.
# @param $1 Stable case label.
# @return Zero after incrementing the aggregate pass count.
# @throws Nothing.
# @note The final count describes cases, not individual grep operations.
pass() {
  local case_label=$1
  printf 'PASS: %s\n' "$case_label"
  pass_count=$((pass_count + 1))
}

# @brief Require a fixed string to occur in a file.
# @param $1 File to inspect.
# @param $2 Exact string required at least once.
# @return Zero when present; otherwise exits through fail.
# @throws Nothing; grep failures are converted to regression diagnostics.
# @note Fixed matching keeps GitHub expressions and shell tokens literal.
assert_file_contains() {
  local file=$1
  local expected=$2
  grep -Fq -- "$expected" "$file" ||
    fail "$(basename "$file") is missing: $expected"
}

# @brief Require a fixed string to be absent from a file.
# @param $1 File to inspect.
# @param $2 Exact forbidden string.
# @return Zero when absent; otherwise exits through fail.
# @throws Nothing; grep matches are converted to regression diagnostics.
# @note Used to prevent reintroduction of branch-name-only gate messages.
assert_file_not_contains() {
  local file=$1
  local forbidden=$2
  if grep -Fq -- "$forbidden" "$file"; then
    fail "$(basename "$file") still contains: $forbidden"
  fi
}

# @brief Extract one named GitHub Actions step's literal shell run block.
# @param $1 Workflow YAML path.
# @param $2 Exact step name.
# @param $3 Destination shell file.
# @return Zero when a run block is found and written, otherwise nonzero.
# @throws Nothing; malformed indentation is represented by the return status.
# @note Maintained workflow run blocks use ten-space literal indentation.
extract_step_run_block() {
  local workflow=$1
  local step_name=$2
  local destination=$3
  awk -v target="      - name: $step_name" '
    $0 == target {
      in_step = 1
      next
    }
    in_step && $0 == "        run: |" {
      capture = 1
      next
    }
    capture && ($0 ~ /^      - / || $0 ~ /^  [^ ]/) {
      exit
    }
    capture {
      sub(/^          /, "")
      print
    }
    END {
      if (!capture) {
        exit 1
      }
    }
  ' "$workflow" > "$destination"
}

# @brief Model whether the protected-path job must run for event identity.
# @param $1 Event name.
# @param $2 Head branch name.
# @param $3 Head repository full name, or an empty value when unavailable.
# @param $4 Base repository full name.
# @return Zero when the job must run; one only for a same-repository CI/** PR.
# @throws Nothing; all identity uncertainty deliberately requires the job.
# @note The workflow expression is statically tied to this matrix below.
protected_job_should_run() {
  local event_name=$1
  local branch_name=$2
  local head_repository=$3
  local base_repository=$4
  [[ "$event_name" != pull_request_target ||
    "$branch_name" != CI/* ||
    "$head_repository" != "$base_repository" ]]
}

# @brief Require one command to succeed and capture its combined output.
# @param $1 Stable case label.
# @param $2 Output log path.
# @param $@ Remaining arguments form the command.
# @return Zero after recording a pass; otherwise exits through fail.
# @throws Nothing; command failure is converted to a regression diagnostic.
# @note The caller owns semantic assertions on the captured output.
run_expect_success() {
  local case_label=$1
  local output_log=$2
  shift 2
  if ! "$@" > "$output_log" 2>&1; then
    sed -n '1,120p' "$output_log" >&2 || true
    fail "$case_label unexpectedly failed"
  fi
  pass "$case_label"
}

# @brief Require one command to fail without a forbidden success message.
# @param $1 Stable case label.
# @param $2 Output log path.
# @param $3 Exact diagnostic required in combined output.
# @param $4 Exact success text that must remain absent, or an empty value.
# @param $@ Remaining arguments form the command.
# @return Zero after recording a pass; otherwise exits through fail.
# @throws Nothing; command status is captured with errexit temporarily disabled.
# @note Artifacts require separate assertions when they can carry a false route.
run_expect_failure() {
  local case_label=$1
  local output_log=$2
  local expected_diagnostic=$3
  local forbidden_success=$4
  shift 4
  local status
  set +e
  "$@" > "$output_log" 2>&1
  status=$?
  set -e
  ((status != 0)) || fail "$case_label unexpectedly succeeded"
  assert_file_contains "$output_log" "$expected_diagnostic"
  if [[ -n "$forbidden_success" ]]; then
    assert_file_not_contains "$output_log" "$forbidden_success"
  fi
  pass "$case_label"
}

# @brief Require a stable gate to assert every declared upstream conclusion.
# @param $1 Extracted gate shell script.
# @param $2 Exact gate step name selecting the expected result inventory.
# @return Zero when every stable assertion label is present.
# @throws Nothing; missing assertions exit through fail.
# @note This is a source contract; positive route execution is covered by the
#   identity and image fixtures in the same regression.
assert_gate_checks_all_results() {
  local gate_script=$1
  local gate_step=$2
  local assertion_label
  local -a assertion_labels=()
  case "$gate_step" in
    "Report healthcheck gate")
      assertion_labels=(
        protected-ci-paths
        ci-image-change
        healthcheck-published-image
        healthcheck-local-image
      )
      ;;
    "Report integration gate")
      assertion_labels=(
        protected-ci-paths
        change-classification
        ci-image-change
        integration-plan
        build-integrity-default
        build-integrity-ipc-disabled
        local-image-integration
        full-ctest
        static-product-consumer-smoke
        ipc-disabled-install-smoke
        scripted-cli
        propagation-script
        plugin-load
        scheduler-repeat
      )
      ;;
    *)
      fail "unknown stable gate step: $gate_step"
      ;;
  esac
  for assertion_label in "${assertion_labels[@]}"; do
    assert_file_contains "$gate_script" "require_value $assertion_label"
  done
}

# @brief Validate static repository-identity and stable-gate workflow contracts.
# @param $1 Workflow YAML path.
# @param $2 Exact stable gate step name.
# @return Zero after writing executable gate/reject fixtures.
# @throws Nothing; missing contracts exit through fail.
# @note The reject step must precede checkout so fork code is never executed.
validate_workflow_contract() {
  local workflow=$1
  local gate_step=$2
  local workflow_name
  local reject_line
  local checkout_line
  local mismatch_count
  local identity_output_count
  workflow_name=$(basename "$workflow" .yml)

  mismatch_count=$(grep -Fc -- \
    "github.event.pull_request.head.repo.full_name != github.repository" \
    "$workflow")
  ((mismatch_count >= 2)) ||
    fail "$workflow_name does not run and reject fork CI/** pull requests"
  identity_output_count=$(grep -Fc -- \
    "CI_HEAD_IS_BASE_REPOSITORY:" "$workflow")
  ((identity_output_count >= 2)) ||
    fail "$workflow_name does not bind protected authorization and gate deduplication to repository identity"
  assert_file_contains "$workflow" \
    '"$CI_HEAD_IS_BASE_REPOSITORY" == true'

  reject_line=$(grep -nF -- "- name: Reject fork CI branch pull request" \
    "$workflow" | head -n 1 | cut -d: -f1)
  checkout_line=$(grep -nF -- "- uses: actions/checkout@v4" \
    "$workflow" | head -n 1 | cut -d: -f1)
  [[ -n "$reject_line" && -n "$checkout_line" ]] ||
    fail "$workflow_name lacks the reject-before-checkout boundary"
  ((reject_line < checkout_line)) ||
    fail "$workflow_name checks out fork code before the rejection step"

  assert_file_contains "$workflow" \
    "Same-repository CI/** pull request intentionally uses the push-triggered workflow."
  assert_file_not_contains "$workflow" \
    "CI/** pull request run intentionally uses the push-triggered workflow."
  assert_file_contains "$workflow" "git merge-base --all"
  assert_file_contains "$workflow" "CI_IMAGE_BASE_REF:"
  assert_file_contains "$workflow" "github.event.pull_request.base.sha"
  assert_file_contains "$workflow" "'origin/main'"
  assert_file_not_contains "$workflow" "--diff-filter"

  extract_step_run_block "$workflow" "$gate_step" \
    "$TEST_ROOT/${workflow_name}-gate.sh" ||
    fail "$workflow_name gate run block could not be extracted"
  extract_step_run_block "$workflow" "Reject fork CI branch pull request" \
    "$TEST_ROOT/${workflow_name}-fork-reject.sh" ||
    fail "$workflow_name fork rejection run block could not be extracted"
  bash -n "$TEST_ROOT/${workflow_name}-gate.sh"
  bash -n "$TEST_ROOT/${workflow_name}-fork-reject.sh"
  assert_gate_checks_all_results "$TEST_ROOT/${workflow_name}-gate.sh" \
    "$gate_step"
  pass "$workflow_name-static-identity-contract"
}

# @brief Exercise same-repository, fork, and missing-repository gate identities.
# @param $1 Extracted stable gate shell script.
# @param $2 Stable workflow label used for artifacts.
# @return Zero after all three identity cases match their expected status.
# @throws Nothing; unexpected gate behavior exits through fail.
# @note Fork and missing identity receive an upstream failure to prove no bypass.
exercise_gate_identity() {
  local gate_script=$1
  local workflow_label=$2
  local summary_file="$TEST_ROOT/${workflow_label}-summary.md"
  local output_log="$TEST_ROOT/${workflow_label}-gate.log"

  run_expect_success "$workflow_label-same-repository-dedup" "$output_log" \
    env CI_EVENT_NAME=pull_request_target CI_BRANCH_NAME=CI/example \
    CI_BASE_REPOSITORY=owner/repository \
    CI_HEAD_REPOSITORY=owner/repository \
    CI_HEAD_IS_BASE_REPOSITORY=true \
    GITHUB_STEP_SUMMARY="$summary_file" bash "$gate_script"
  assert_file_contains "$output_log" "Same-repository CI/** pull request"

  run_expect_failure "$workflow_label-fork-fails-closed" "$output_log" \
    "protected-ci-paths was 'failure'; expected 'success'." \
    "Same-repository CI/** pull request" \
    env CI_EVENT_NAME=pull_request_target CI_BRANCH_NAME=CI/example \
    CI_BASE_REPOSITORY=owner/repository \
    CI_HEAD_REPOSITORY=fork/repository CI_HEAD_IS_BASE_REPOSITORY=false \
    PROTECTED_RESULT=failure \
    GITHUB_STEP_SUMMARY="$summary_file" bash "$gate_script"

  run_expect_failure "$workflow_label-missing-head-repository-fails-closed" \
    "$output_log" "protected-ci-paths was 'failure'; expected 'success'." \
    "Same-repository CI/** pull request" \
    env CI_EVENT_NAME=pull_request_target CI_BRANCH_NAME=CI/example \
    CI_BASE_REPOSITORY=owner/repository CI_HEAD_REPOSITORY= \
    CI_HEAD_IS_BASE_REPOSITORY=false PROTECTED_RESULT=failure \
    GITHUB_STEP_SUMMARY="$summary_file" \
    bash "$gate_script"
}

# @brief Execute the pre-checkout fork rejection block and require failure.
# @param $1 Extracted rejection shell script.
# @param $2 Stable workflow label used for artifacts.
# @return Zero when rejection occurs before any checkout-dependent behavior.
# @throws Nothing; an unexpected success exits through fail.
# @note The workflow step condition limits this block to fork CI/** PR events.
exercise_fork_rejection() {
  local reject_script=$1
  local workflow_label=$2
  local artifact_dir="$TEST_ROOT/${workflow_label}-protected"
  local output_log="$TEST_ROOT/${workflow_label}-reject.log"
  run_expect_failure "$workflow_label-fork-rejected-before-checkout" \
    "$output_log" "Fork CI/** pull requests are rejected before checkout." "" \
    env CI_ARTIFACT_DIR="$artifact_dir" \
    CI_BASE_REPOSITORY=owner/repository \
    CI_HEAD_REPOSITORY=fork/repository bash "$reject_script"
  assert_file_contains "$artifact_dir/summary.log" \
    "Received head repository: 'fork/repository'."
}

# @brief Create an isolated image-input history with cumulative and exact bases.
# @param $1 Destination repository path.
# @return Zero after printing base, image, docs, and newline-path SHAs.
# @throws Nothing; Git and filesystem failures terminate through set -e.
# @note The newline-path case would be split into a false Dockerfile match by a
#   newline-delimited inventory, so it locks the NUL-record contract.
create_image_history() {
  local repository=$1
  local base_sha
  local image_sha
  local docs_sha
  local newline_sha
  local newline_path=$'docs/note\nDockerfile.ci'
  mkdir -p "$repository"
  git -C "$repository" init -q
  git -C "$repository" config user.name "CI Routing Test"
  git -C "$repository" config user.email "ci-routing@example.invalid"
  printf 'baseline\n' > "$repository/seed.txt"
  git -C "$repository" add seed.txt
  git -C "$repository" commit -qm baseline
  base_sha=$(git -C "$repository" rev-parse HEAD)

  printf 'FROM scratch\n' > "$repository/Dockerfile.ci"
  git -C "$repository" add Dockerfile.ci
  git -C "$repository" commit -qm "image input"
  image_sha=$(git -C "$repository" rev-parse HEAD)

  mkdir -p "$repository/docs"
  printf '# documentation\n' > "$repository/docs/note.md"
  git -C "$repository" add docs/note.md
  git -C "$repository" commit -qm documentation
  docs_sha=$(git -C "$repository" rev-parse HEAD)

  git -C "$repository" checkout -q --detach "$base_sha"
  mkdir -p "$repository/docs"
  printf 'not an image input\n' > "$repository/$newline_path"
  git -C "$repository" add -- "$newline_path"
  git -C "$repository" commit -qm "newline path"
  newline_sha=$(git -C "$repository" rev-parse HEAD)

  printf '%s\n%s\n%s\n%s\n' \
    "$base_sha" "$image_sha" "$docs_sha" "$newline_sha"
}

# @brief Run the production CI-image detector against an isolated repository.
# @param $1 Repository path.
# @param $2 Explicit comparison base.
# @param $3 Artifact directory.
# @param $4 Combined output log.
# @return The production detector process status.
# @throws Nothing; detector failure is returned to the caller.
# @note GITHUB_OUTPUT is isolated with the other per-case artifacts.
run_image_detector() {
  local repository=$1
  local base_ref=$2
  local artifact_dir=$3
  local output_log=$4
  rm -rf -- "$artifact_dir"
  mkdir -p "$artifact_dir"
  CI_IMAGE_REPO_ROOT="$repository" \
    CI_IMAGE_BASE_REF="$base_ref" \
    CI_ARTIFACT_DIR="$artifact_dir" \
    GITHUB_OUTPUT="$artifact_dir/github-output.txt" \
    bash "$IMAGE_DETECTOR" > "$output_log" 2>&1
}

# @brief Require one successful image-detector route value.
# @param $1 Stable case label.
# @param $2 Expected changed value.
# @param $3 Artifact directory.
# @param $4 Combined output log.
# @return Zero after recording a pass; otherwise exits through fail.
# @throws Nothing; missing or mismatched output exits through fail.
# @note Both the diagnostic artifact and GitHub output must agree exactly.
assert_image_route() {
  local case_label=$1
  local expected=$2
  local artifact_dir=$3
  local output_log=$4
  assert_file_contains "$output_log" "changed=$expected"
  grep -Fqx "changed=$expected" "$artifact_dir/ci-image-change.env" ||
    fail "$case_label artifact route mismatch"
  grep -Fqx "changed=$expected" "$artifact_dir/github-output.txt" ||
    fail "$case_label GitHub output mismatch"
  pass "$case_label"
}

# @brief Create a Git shim that fails only changed-path diff inventory calls.
# @param $1 Destination directory added to the front of PATH.
# @return Zero after writing an executable shim.
# @throws Nothing; filesystem failures terminate through set -e.
# @note All non-name-only Git calls delegate to CI_ROUTING_TEST_REAL_GIT.
create_failing_git_shim() {
  local shim_dir=$1
  mkdir -p "$shim_dir"
  cat > "$shim_dir/git" <<'EOF'
#!/usr/bin/env bash
set -Eeuo pipefail

# @file git
# @brief Inject a deterministic failure into changed-path Git diff calls.
if [[ "${1:-}" == diff ]]; then
  for argument in "$@"; do
    if [[ "$argument" == --name-only ]]; then
      echo "injected git diff failure" >&2
      exit 73
    fi
  done
fi
exec "$CI_ROUTING_TEST_REAL_GIT" "$@"
EOF
  chmod +x "$shim_dir/git"
}

# @brief Exercise cumulative/exact image bases and both failure-propagation seams.
# @return Zero after every detector and healthcheck case matches the contract.
# @throws Nothing; unexpected behavior exits through fail.
# @note The nested healthcheck fails before reaching its own regression calls,
#   so invoking it here cannot recurse.
exercise_changed_path_contracts() {
  local image_repository="$TEST_ROOT/image-repository"
  local history_file="$TEST_ROOT/image-history.txt"
  local artifact_dir="$TEST_ROOT/image-artifacts"
  local output_log="$TEST_ROOT/image-detector.log"
  local shim_dir="$TEST_ROOT/failing-git"
  local base_sha
  local image_sha
  local docs_sha
  local newline_sha
  local health_artifacts="$TEST_ROOT/healthcheck-artifacts"
  local health_log="$TEST_ROOT/healthcheck-failure.log"
  local health_base

  create_image_history "$image_repository" > "$history_file"
  mapfile -t history < "$history_file"
  ((${#history[@]} == 4)) || fail "image history did not expose four revisions"
  base_sha=${history[0]}
  image_sha=${history[1]}
  docs_sha=${history[2]}
  newline_sha=${history[3]}

  git -C "$image_repository" checkout -q --detach "$docs_sha"
  run_image_detector "$image_repository" "$base_sha" "$artifact_dir" \
    "$output_log"
  assert_image_route image-cumulative-base true "$artifact_dir" "$output_log"

  run_image_detector "$image_repository" "$image_sha" "$artifact_dir" \
    "$output_log"
  assert_image_route image-exact-incremental-base false "$artifact_dir" \
    "$output_log"

  run_image_detector "$image_repository" "$docs_sha" "$artifact_dir" \
    "$output_log"
  assert_image_route image-empty-comparison false "$artifact_dir" "$output_log"

  git -C "$image_repository" checkout -q --detach "$newline_sha"
  run_image_detector "$image_repository" "$base_sha" "$artifact_dir" \
    "$output_log"
  assert_image_route image-newline-path-not-split false "$artifact_dir" \
    "$output_log"
  assert_file_contains "$artifact_dir/changed-files.txt" \
    "docs/note\\nDockerfile.ci"

  create_failing_git_shim "$shim_dir"
  rm -rf -- "$artifact_dir"
  mkdir -p "$artifact_dir"
  run_expect_failure image-git-diff-failure-propagates "$output_log" \
    "CI image changed-path detection failed." "changed=false" \
    env PATH="$shim_dir:$PATH" CI_ROUTING_TEST_REAL_GIT="$REAL_GIT" \
    CI_IMAGE_REPO_ROOT="$image_repository" CI_IMAGE_BASE_REF="$base_sha" \
    CI_ARTIFACT_DIR="$artifact_dir" \
    GITHUB_OUTPUT="$artifact_dir/github-output.txt" bash "$IMAGE_DETECTOR"
  [[ ! -f "$artifact_dir/ci-image-change.env" ]] ||
    fail "image diff failure wrote a route artifact"
  [[ ! -s "$artifact_dir/github-output.txt" ]] ||
    fail "image diff failure wrote a GitHub route output"

  health_base=$(git -C "$REPO_ROOT" rev-parse HEAD~1)
  rm -rf -- "$health_artifacts"
  mkdir -p "$health_artifacts"
  run_expect_failure healthcheck-git-diff-failure-propagates "$health_log" \
    "Healthcheck changed-path detection failed." \
    "No changed C++ files; clang-format and cpplint skipped." \
    env PATH="$shim_dir:$PATH" CI_ROUTING_TEST_REAL_GIT="$REAL_GIT" \
    CI_BASE_REF="$health_base" CI_ARTIFACT_DIR="$health_artifacts" \
    bash "$HEALTHCHECK"
  if [[ -f "$health_artifacts/static-summary.log" ]]; then
    assert_file_not_contains "$health_artifacts/static-summary.log" \
      "No changed C++ files"
  fi
}

# @brief Exercise all durable workflow-routing and failure-propagation cases.
# @return Zero when every case passes.
# @throws Nothing; command failures terminate through set -e or fail.
# @note Existing change-classification coverage is invoked separately by the
#   healthcheck so this test focuses on workflow identity and detector seams.
main() {
  local health_workflow="$REPO_ROOT/.github/workflows/ci-healthcheck.yml"
  local integration_workflow="$REPO_ROOT/.github/workflows/ci-integration.yml"

  validate_workflow_contract "$health_workflow" "Report healthcheck gate"
  validate_workflow_contract "$integration_workflow" "Report integration gate"

  if protected_job_should_run pull_request_target CI/example \
    owner/repository owner/repository; then
    fail "same-repository CI/** pull request was not deduplicated"
  fi
  protected_job_should_run pull_request_target CI/example \
    fork/repository owner/repository ||
    fail "fork CI/** pull request did not run the protected job"
  protected_job_should_run pull_request_target CI/example '' owner/repository ||
    fail "missing head repository identity did not fail closed"
  protected_job_should_run pull_request_target feature/example \
    fork/repository owner/repository ||
    fail "ordinary fork pull request did not run the protected job"
  pass protected-job-identity-matrix

  exercise_gate_identity "$TEST_ROOT/ci-healthcheck-gate.sh" healthcheck
  exercise_gate_identity "$TEST_ROOT/ci-integration-gate.sh" integration
  exercise_fork_rejection "$TEST_ROOT/ci-healthcheck-fork-reject.sh" \
    healthcheck
  exercise_fork_rejection "$TEST_ROOT/ci-integration-fork-reject.sh" \
    integration
  exercise_changed_path_contracts

  printf 'All %d CI routing cases passed.\n' "$pass_count"
}

main "$@"
