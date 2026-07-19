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

# @brief Extract one named top-level job from a maintained workflow.
# @param $1 Workflow YAML path.
# @param $2 Exact job identifier below the jobs mapping.
# @param $3 Destination YAML fragment.
# @return Zero when the job is found and written, otherwise nonzero.
# @throws Nothing; a missing job is represented by the return status.
# @note Scoping later assertions to this fragment prevents another job from
#   satisfying a local-image contract accidentally.
extract_job_block() {
  local workflow=$1
  local job_name=$2
  local destination=$3
  awk -v target="  ${job_name}:" '
    $0 == target {
      capture = 1
    }
    capture && $0 != target && $0 ~ /^  [[:alnum:]_-]+:$/ {
      exit
    }
    capture {
      print
    }
    END {
      if (!capture) {
        exit 1
      }
    }
  ' "$workflow" > "$destination"
}

# @brief Extract one exact named step from one previously scoped job fragment.
# @param $1 Extracted top-level job YAML fragment.
# @param $2 Exact step name within that job.
# @param $3 Destination YAML fragment.
# @return Zero only when the named step occurs exactly once and is written.
# @throws Nothing; missing or duplicate steps are represented by nonzero status.
# @note The next same-indentation step closes the fragment, so metadata from a
#   neighboring step cannot satisfy assertions on the selected production step.
extract_step_block() {
  local job_file=$1
  local step_name=$2
  local destination=$3
  awk -v target="      - name: $step_name" '
    $0 == target {
      match_count++
      capture = (match_count == 1)
    }
    capture && $0 != target && $0 ~ /^      - / {
      capture = 0
    }
    capture {
      print
    }
    END {
      if (match_count != 1) {
        exit 1
      }
    }
  ' "$job_file" > "$destination"
}

# @brief Extract the one folded top-level condition from a workflow job block.
# @param $1 Extracted top-level job YAML fragment.
# @param $2 Destination containing the condition block with original spacing.
# @return Zero only when exactly one complete top-level condition is written.
# @throws Nothing; missing, duplicate, or unterminated blocks return nonzero.
# @note Step-level conditions have deeper indentation and are not candidates.
extract_job_if_block() {
  local job_file=$1
  local destination=$2
  awk '
    $0 == "    if: >-" {
      condition_count++
      capture = 1
    }
    capture {
      print
    }
    capture && $0 == "      }}" {
      capture = 0
    }
    END {
      if (condition_count != 1 || capture) {
        exit 1
      }
    }
  ' "$job_file" > "$destination"
}

# @brief Require the local-image healthcheck to materialize the exact PR base.
# @param $1 Healthcheck workflow YAML path.
# @return Zero after the job-scoped fetch, verification, and order checks pass.
# @throws Nothing; missing or misordered contracts exit through fail.
# @note The base-repository fetch must be the job's first shell execution so no
#   fork-head script or image build runs before the exact base object exists.
validate_local_image_base_fetch() {
  local workflow=$1
  local job_file="$TEST_ROOT/healthcheck-local-image-job.yml"
  local fetch_script="$TEST_ROOT/healthcheck-local-image-base-fetch.sh"
  local checkout_line
  local fetch_step_line
  local first_run_line
  local verify_line
  local build_line
  local healthcheck_line

  extract_job_block "$workflow" healthcheck-local-image "$job_file" ||
    fail "healthcheck-local-image job could not be extracted"
  extract_step_run_block "$job_file" "Fetch pull request base history" \
    "$fetch_script" ||
    fail "healthcheck-local-image base-fetch run block could not be extracted"

  assert_file_contains "$job_file" \
    'CI_BASE_REPOSITORY: ${{ github.repository }}'
  assert_file_contains "$job_file" \
    'CI_BASE_BRANCH: ${{ github.base_ref }}'
  assert_file_contains "$job_file" \
    'CI_BASE_SHA: ${{ github.event.pull_request.base.sha }}'
  assert_file_contains "$job_file" \
    "if: github.event_name == 'pull_request' || github.event_name == 'pull_request_target'"
  assert_file_contains "$job_file" \
    "CI_BASE_REF: \${{ (github.event_name == 'pull_request' || github.event_name == 'pull_request_target') && github.event.pull_request.base.sha"
  assert_file_contains "$job_file" \
    '-v "${{ github.workspace }}:/workspace" \'
  assert_file_contains "$job_file" '-e CI_BASE_REF \'
  assert_file_contains "$fetch_script" \
    'git fetch --no-tags --no-recurse-submodules'
  assert_file_contains "$fetch_script" \
    '"https://github.com/${CI_BASE_REPOSITORY}.git" \'
  assert_file_contains "$fetch_script" \
    '"+refs/heads/${CI_BASE_BRANCH}:refs/remotes/photospider-base/${CI_BASE_BRANCH}"'
  assert_file_contains "$fetch_script" \
    'git rev-parse --verify "$CI_BASE_SHA^{commit}" >/dev/null'
  bash -n "$fetch_script"

  checkout_line=$(grep -nF -- "- uses: actions/checkout@v4" "$job_file" |
    head -n 1 | cut -d: -f1)
  fetch_step_line=$(grep -nF -- \
    "- name: Fetch pull request base history" "$job_file" |
    head -n 1 | cut -d: -f1)
  first_run_line=$(grep -nE -- '^[[:space:]]+run:' "$job_file" |
    head -n 1 | cut -d: -f1)
  verify_line=$(grep -nF -- \
    'git rev-parse --verify "$CI_BASE_SHA^{commit}" >/dev/null' "$job_file" |
    head -n 1 | cut -d: -f1)
  build_line=$(grep -nF -- "- name: Build local CI image" "$job_file" |
    head -n 1 | cut -d: -f1)
  healthcheck_line=$(grep -nF -- \
    "- name: Run healthcheck in local CI image" "$job_file" |
    head -n 1 | cut -d: -f1)
  [[ -n "$checkout_line" && -n "$fetch_step_line" &&
    -n "$first_run_line" && -n "$verify_line" && -n "$build_line" &&
    -n "$healthcheck_line" ]] ||
    fail "healthcheck-local-image lacks a complete exact-base execution order"
  ((checkout_line < fetch_step_line &&
    fetch_step_line < first_run_line &&
    first_run_line < verify_line &&
    verify_line < build_line &&
    build_line < healthcheck_line)) ||
    fail "healthcheck-local-image executes fork-head work before exact-base verification"

  pass healthcheck-local-image-exact-base-before-execution
}

# @brief Require the published-image healthcheck to materialize the exact PR base.
# @param $1 Healthcheck workflow YAML path.
# @return Zero after job-scoped fetch, verification, and order checks pass.
# @throws Nothing; missing or misordered contracts exit through fail.
# @note Assertions use only the published job so similarly named detector or
#   local-image steps cannot satisfy the contract accidentally.
validate_published_image_base_fetch() {
  local workflow=$1
  local job_file="$TEST_ROOT/healthcheck-published-image-job.yml"
  local fetch_script="$TEST_ROOT/healthcheck-published-image-base-fetch.sh"
  local checkout_line
  local fetch_step_line
  local fetch_command_line
  local verify_line
  local healthcheck_step_line
  local healthcheck_run_line

  extract_job_block "$workflow" healthcheck-published-image "$job_file" ||
    fail "healthcheck-published-image job could not be extracted"
  extract_step_run_block "$job_file" "Fetch pull request base history" \
    "$fetch_script" ||
    fail "healthcheck-published-image base-fetch run block could not be extracted"

  assert_file_contains "$job_file" \
    'CI_BASE_REPOSITORY: ${{ github.repository }}'
  assert_file_contains "$job_file" \
    'CI_BASE_BRANCH: ${{ github.base_ref }}'
  assert_file_contains "$job_file" \
    'CI_BASE_SHA: ${{ github.event.pull_request.base.sha }}'
  assert_file_contains "$job_file" \
    "if: github.event_name == 'pull_request' || github.event_name == 'pull_request_target'"
  assert_file_contains "$job_file" \
    "CI_BASE_REF: \${{ (github.event_name == 'pull_request' || github.event_name == 'pull_request_target') && github.event.pull_request.base.sha"
  assert_file_contains "$job_file" \
    'run: bash ci/scripts/healthcheck.sh'
  assert_file_contains "$fetch_script" \
    'git fetch --no-tags --no-recurse-submodules'
  assert_file_contains "$fetch_script" \
    '"https://github.com/${CI_BASE_REPOSITORY}.git" \'
  assert_file_contains "$fetch_script" \
    '"+refs/heads/${CI_BASE_BRANCH}:refs/remotes/photospider-base/${CI_BASE_BRANCH}"'
  assert_file_contains "$fetch_script" \
    'git rev-parse --verify "$CI_BASE_SHA^{commit}" >/dev/null'
  bash -n "$fetch_script"

  checkout_line=$(grep -nF -- "- uses: actions/checkout@v4" "$job_file" |
    head -n 1 | cut -d: -f1)
  fetch_step_line=$(grep -nF -- \
    "- name: Fetch pull request base history" "$job_file" |
    head -n 1 | cut -d: -f1)
  fetch_command_line=$(grep -nF -- \
    '"https://github.com/${CI_BASE_REPOSITORY}.git" \' "$job_file" |
    head -n 1 | cut -d: -f1)
  verify_line=$(grep -nF -- \
    'git rev-parse --verify "$CI_BASE_SHA^{commit}" >/dev/null' "$job_file" |
    head -n 1 | cut -d: -f1)
  healthcheck_step_line=$(grep -nF -- "- name: Run healthcheck" "$job_file" |
    head -n 1 | cut -d: -f1)
  healthcheck_run_line=$(grep -nF -- \
    'run: bash ci/scripts/healthcheck.sh' "$job_file" |
    head -n 1 | cut -d: -f1)
  [[ -n "$checkout_line" && -n "$fetch_step_line" &&
    -n "$fetch_command_line" && -n "$verify_line" &&
    -n "$healthcheck_step_line" && -n "$healthcheck_run_line" ]] ||
    fail "healthcheck-published-image lacks a complete exact-base execution order"
  ((checkout_line < fetch_step_line &&
    fetch_step_line < fetch_command_line &&
    fetch_command_line < verify_line &&
    verify_line < healthcheck_step_line &&
    healthcheck_step_line < healthcheck_run_line)) ||
    fail "healthcheck-published-image runs healthcheck before exact-base verification"

  pass healthcheck-published-image-exact-base-before-execution
}

# @brief Require both published-image history fetches to select Bash explicitly.
# @param $1 Healthcheck workflow YAML path.
# @return Zero after both exact production steps bind one top-level Bash shell.
# @throws Nothing; missing, duplicate, misplaced, or non-Bash shell metadata
#   exits through fail.
# @note Job and step extraction prevents a shell key in another job or sibling
#   step from satisfying the container fetch contract accidentally.
validate_published_image_fetch_shells() {
  local workflow=$1
  local job_file="$TEST_ROOT/healthcheck-published-image-shell-job.yml"
  local pull_request_step_file
  local ci_branch_step_file
  local container_count
  local shell_key_count
  local shell_bash_count
  local step_index
  local -a step_files=()
  local -a step_names=(
    "Fetch pull request base history"
    "Fetch CI branch main history"
  )

  pull_request_step_file="$TEST_ROOT/healthcheck-published-image-pr-fetch-step.yml"
  ci_branch_step_file="$TEST_ROOT/healthcheck-published-image-ci-main-fetch-step.yml"
  step_files=("$pull_request_step_file" "$ci_branch_step_file")

  extract_job_block "$workflow" healthcheck-published-image "$job_file" ||
    fail "healthcheck-published-image job could not be extracted for shell validation"
  container_count=$(grep -Fxc -- "    container:" "$job_file" || true)
  ((container_count == 1)) ||
    fail "healthcheck-published-image must remain exactly one container job"

  extract_step_block "$job_file" "${step_names[0]}" \
    "$pull_request_step_file" ||
    fail "healthcheck-published-image pull-request fetch step is not unique"
  extract_step_block "$job_file" "${step_names[1]}" \
    "$ci_branch_step_file" ||
    fail "healthcheck-published-image CI-main fetch step is not unique"

  for step_index in "${!step_files[@]}"; do
    shell_key_count=$(grep -Ec -- '^        shell:' \
      "${step_files[$step_index]}" || true)
    ((shell_key_count == 1)) ||
      fail "healthcheck-published-image ${step_names[$step_index]} must bind exactly one step shell"
    shell_bash_count=$(grep -Fxc -- '        shell: bash' \
      "${step_files[$step_index]}" || true)
    ((shell_bash_count == 1)) ||
      fail "healthcheck-published-image ${step_names[$step_index]} must bind shell: bash"
  done

  pass healthcheck-published-image-fetch-steps-explicit-bash
}

# @brief Require the published container to trust only its exact workspace.
# @param $1 Healthcheck workflow YAML path.
# @return Zero after the scoped source, order, and isolated Git checks pass.
# @throws Nothing; missing, broadened, or misordered trust exits through fail.
# @note The extracted production block runs with an isolated HOME and Git
#   repository. This proves its exact global entry and HEAD access, but does not
#   simulate cross-UID ownership or the hosted container runtime.
validate_published_image_workspace_trust() {
  local workflow=$1
  local job_file="$TEST_ROOT/healthcheck-published-image-trust-job.yml"
  local trust_step_file="$TEST_ROOT/healthcheck-published-image-trust-step.yml"
  local trust_script="$TEST_ROOT/healthcheck-published-image-trust.sh"
  local trust_repository="$TEST_ROOT/published-image-trust-repository"
  local trust_home="$TEST_ROOT/published-image-trust-home"
  local trusted_directories_file="$TEST_ROOT/published-image-safe-directories.txt"
  local shell_key_count
  local shell_bash_count
  local run_key_count
  local literal_run_count
  local executable_line_count
  local strict_mode_count
  local job_safe_directory_count
  local step_safe_directory_count
  local exact_safe_directory_count
  local head_verify_count
  local checkout_line
  local trust_step_line
  local first_run_line
  local trust_config_line
  local trust_verify_line
  local pull_request_fetch_line
  local ci_branch_fetch_line
  local healthcheck_step_line
  local healthcheck_run_line
  local -a trusted_directories=()

  extract_job_block "$workflow" healthcheck-published-image "$job_file" ||
    fail "healthcheck-published-image job could not be extracted for workspace trust validation"
  extract_step_block "$job_file" "Trust checked-out workspace" \
    "$trust_step_file" ||
    fail "healthcheck-published-image workspace-trust step is not unique"
  extract_step_run_block "$trust_step_file" "Trust checked-out workspace" \
    "$trust_script" ||
    fail "healthcheck-published-image workspace-trust run block could not be extracted"

  shell_key_count=$(grep -Ec -- '^        shell:' "$trust_step_file" || true)
  ((shell_key_count == 1)) ||
    fail "healthcheck-published-image workspace trust must bind exactly one step shell"
  shell_bash_count=$(grep -Fxc -- '        shell: bash' \
    "$trust_step_file" || true)
  ((shell_bash_count == 1)) ||
    fail "healthcheck-published-image workspace trust must bind shell: bash"
  run_key_count=$(grep -Ec -- '^        run:' "$trust_step_file" || true)
  ((run_key_count == 1)) ||
    fail "healthcheck-published-image workspace trust must contain exactly one run block"
  literal_run_count=$(grep -Fxc -- '        run: |' \
    "$trust_step_file" || true)
  ((literal_run_count == 1)) ||
    fail "healthcheck-published-image workspace trust must use one literal run block"

  executable_line_count=$(grep -Ec -- \
    '^[[:space:]]*[^[:space:]#]' "$trust_script" || true)
  strict_mode_count=$(grep -Fxc -- 'set -Eeuo pipefail' \
    "$trust_script" || true)
  job_safe_directory_count=$(grep -Fc -- 'safe.directory' "$job_file" || true)
  step_safe_directory_count=$(grep -Fc -- \
    'safe.directory' "$trust_script" || true)
  exact_safe_directory_count=$(grep -Fxc -- \
    'git config --global --add safe.directory "$GITHUB_WORKSPACE"' \
    "$trust_script" || true)
  head_verify_count=$(grep -Fxc -- \
    'git -C "$GITHUB_WORKSPACE" rev-parse --verify "HEAD^{commit}" >/dev/null' \
    "$trust_script" || true)
  ((executable_line_count == 3 && strict_mode_count == 1)) ||
    fail "healthcheck-published-image workspace trust may execute only its three locked commands"
  ((job_safe_directory_count == 1 &&
    step_safe_directory_count == 1 &&
    exact_safe_directory_count == 1)) ||
    fail "healthcheck-published-image must trust only the exact GITHUB_WORKSPACE without a wildcard"
  ((head_verify_count == 1)) ||
    fail "healthcheck-published-image workspace trust must verify HEAD read-only"
  bash -n "$trust_script"

  checkout_line=$(grep -nF -- "- uses: actions/checkout@v4" "$job_file" |
    head -n 1 | cut -d: -f1)
  trust_step_line=$(grep -nF -- "- name: Trust checked-out workspace" \
    "$job_file" | head -n 1 | cut -d: -f1)
  first_run_line=$(grep -nE -- '^[[:space:]]+run:' "$job_file" |
    head -n 1 | cut -d: -f1)
  trust_config_line=$(grep -nF -- \
    'git config --global --add safe.directory "$GITHUB_WORKSPACE"' \
    "$job_file" | head -n 1 | cut -d: -f1)
  trust_verify_line=$(grep -nF -- \
    'git -C "$GITHUB_WORKSPACE" rev-parse --verify "HEAD^{commit}" >/dev/null' \
    "$job_file" | head -n 1 | cut -d: -f1)
  pull_request_fetch_line=$(grep -nF -- \
    "- name: Fetch pull request base history" "$job_file" |
    head -n 1 | cut -d: -f1)
  ci_branch_fetch_line=$(grep -nF -- \
    "- name: Fetch CI branch main history" "$job_file" |
    head -n 1 | cut -d: -f1)
  healthcheck_step_line=$(grep -nF -- "- name: Run healthcheck" "$job_file" |
    head -n 1 | cut -d: -f1)
  healthcheck_run_line=$(grep -nF -- \
    'run: bash ci/scripts/healthcheck.sh' "$job_file" |
    head -n 1 | cut -d: -f1)
  [[ -n "$checkout_line" && -n "$trust_step_line" &&
    -n "$first_run_line" && -n "$trust_config_line" &&
    -n "$trust_verify_line" && -n "$pull_request_fetch_line" &&
    -n "$ci_branch_fetch_line" && -n "$healthcheck_step_line" &&
    -n "$healthcheck_run_line" ]] ||
    fail "healthcheck-published-image lacks a complete workspace-trust execution order"
  ((checkout_line < trust_step_line &&
    trust_step_line < first_run_line &&
    first_run_line < trust_config_line &&
    trust_config_line < trust_verify_line &&
    trust_verify_line < pull_request_fetch_line &&
    trust_verify_line < ci_branch_fetch_line &&
    trust_verify_line < healthcheck_step_line &&
    healthcheck_step_line < healthcheck_run_line)) ||
    fail "healthcheck-published-image does not establish trust before fetch and healthcheck"

  mkdir -p "$trust_home"
  git init -q -b main "$trust_repository"
  git -C "$trust_repository" config user.name "CI Routing Test"
  git -C "$trust_repository" config user.email "ci-routing@example.invalid"
  printf 'baseline\n' > "$trust_repository/seed.txt"
  git -C "$trust_repository" add seed.txt
  git -C "$trust_repository" commit -qm baseline
  if ! env HOME="$trust_home" \
    GIT_CONFIG_GLOBAL="$trust_home/gitconfig" GIT_CONFIG_NOSYSTEM=1 \
    GITHUB_WORKSPACE="$trust_repository" \
    bash --noprofile --norc -e -o pipefail "$trust_script"; then
    fail "healthcheck-published-image workspace-trust block failed in isolation"
  fi
  if ! env HOME="$trust_home" \
    GIT_CONFIG_GLOBAL="$trust_home/gitconfig" GIT_CONFIG_NOSYSTEM=1 \
    "$REAL_GIT" config --global --get-all safe.directory \
    > "$trusted_directories_file"; then
    fail "isolated workspace-trust global configuration could not be read"
  fi
  mapfile -t trusted_directories < "$trusted_directories_file"
  ((${#trusted_directories[@]} == 1)) ||
    fail "isolated workspace trust did not add exactly one global directory"
  [[ "${trusted_directories[0]}" == "$trust_repository" ]] ||
    fail "isolated workspace trust broadened the configured directory"

  pass healthcheck-published-image-exact-workspace-trust-before-execution
}

# @brief Require one healthcheck job to use cumulative main on CI/** pushes.
# @param $1 Healthcheck workflow YAML path.
# @param $2 Exact published-image or local-image healthcheck job identifier.
# @return Zero after job-scoped fetch, route, syntax, and order checks pass.
# @throws Nothing; missing, duplicated, or misordered contracts exit through
#   fail.
# @note This exact-locks production source without claiming to evaluate GitHub
#   expressions; the extracted production fetch block is executed separately.
validate_ci_branch_healthcheck_base() {
  local workflow=$1
  local job_name=$2
  local job_file="$TEST_ROOT/${job_name}-ci-main-job.yml"
  local fetch_script="$TEST_ROOT/${job_name}-ci-main-fetch.sh"
  local expected_route
  local fetch_step_count
  local base_ref_binding_count
  local route_count
  local checkout_line
  local fetch_step_line
  local verify_line
  local build_line
  local healthcheck_step_line
  local healthcheck_run_line
  local mount_line

  expected_route="          CI_BASE_REF: \${{ (github.event_name == 'pull_request' || github.event_name == 'pull_request_target') && github.event.pull_request.base.sha || (github.event_name == 'push' && startsWith(github.ref_name, 'CI/')) && 'origin/main' || github.event_name == 'push' && github.event.before || 'origin/main' }}"

  extract_job_block "$workflow" "$job_name" "$job_file" ||
    fail "$job_name job could not be extracted for CI branch routing"
  extract_step_run_block "$job_file" "Fetch CI branch main history" \
    "$fetch_script" ||
    fail "$job_name CI branch main-fetch run block could not be extracted"

  fetch_step_count=$(grep -Fxc -- \
    "      - name: Fetch CI branch main history" "$job_file" || true)
  ((fetch_step_count == 1)) ||
    fail "$job_name must contain exactly one CI branch main-fetch step"
  assert_file_contains "$job_file" \
    "        if: github.event_name == 'push' && startsWith(github.ref_name, 'CI/')"
  base_ref_binding_count=$(grep -Fc -- "CI_BASE_REF:" "$job_file" || true)
  ((base_ref_binding_count == 1)) ||
    fail "$job_name must contain exactly one CI_BASE_REF binding"
  route_count=$(grep -Fxc -- "$expected_route" "$job_file" || true)
  ((route_count == 1)) ||
    fail "$job_name does not contain the exact PR/CI-branch/main-push base route"
  assert_file_contains "$fetch_script" \
    'git fetch --no-tags --no-recurse-submodules origin \'
  assert_file_contains "$fetch_script" \
    '"+refs/heads/main:refs/remotes/origin/main"'
  assert_file_contains "$fetch_script" \
    'git rev-parse --verify "origin/main^{commit}" >/dev/null'
  bash -n "$fetch_script"

  checkout_line=$(grep -nF -- "- uses: actions/checkout@v4" "$job_file" |
    head -n 1 | cut -d: -f1)
  fetch_step_line=$(grep -nF -- \
    "- name: Fetch CI branch main history" "$job_file" |
    head -n 1 | cut -d: -f1)
  verify_line=$(grep -nF -- \
    'git rev-parse --verify "origin/main^{commit}" >/dev/null' "$job_file" |
    head -n 1 | cut -d: -f1)
  [[ -n "$checkout_line" && -n "$fetch_step_line" && -n "$verify_line" ]] ||
    fail "$job_name lacks a complete CI branch main-fetch order"

  case "$job_name" in
    healthcheck-published-image)
      healthcheck_step_line=$(grep -nF -- "- name: Run healthcheck" \
        "$job_file" | head -n 1 | cut -d: -f1)
      healthcheck_run_line=$(grep -nF -- \
        'run: bash ci/scripts/healthcheck.sh' "$job_file" |
        head -n 1 | cut -d: -f1)
      [[ -n "$healthcheck_step_line" && -n "$healthcheck_run_line" ]] ||
        fail "$job_name lacks the production healthcheck execution"
      ((checkout_line < fetch_step_line &&
        fetch_step_line < verify_line &&
        verify_line < healthcheck_step_line &&
        healthcheck_step_line < healthcheck_run_line)) ||
        fail "$job_name runs healthcheck before CI main verification"
      ;;
    healthcheck-local-image)
      build_line=$(grep -nF -- "- name: Build local CI image" "$job_file" |
        head -n 1 | cut -d: -f1)
      healthcheck_step_line=$(grep -nF -- \
        "- name: Run healthcheck in local CI image" "$job_file" |
        head -n 1 | cut -d: -f1)
      mount_line=$(grep -nF -- \
        '-v "${{ github.workspace }}:/workspace" \' "$job_file" |
        head -n 1 | cut -d: -f1)
      [[ -n "$build_line" && -n "$healthcheck_step_line" &&
        -n "$mount_line" ]] ||
        fail "$job_name lacks the Docker build and mounted healthcheck order"
      ((checkout_line < fetch_step_line &&
        fetch_step_line < verify_line &&
        verify_line < build_line &&
        build_line < healthcheck_step_line &&
        healthcheck_step_line < mount_line)) ||
        fail "$job_name builds or mounts head content before CI main verification"
      ;;
    *)
      fail "unsupported healthcheck job for CI branch routing: $job_name"
      ;;
  esac

  pass "$job_name-ci-branch-cumulative-main-before-execution"
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
        build-integrity-dependency-disabled
        local-image-integration
        full-ctest
        static-product-consumer-smoke
        ipc-disabled-install-smoke
        dependency-disabled-install-smoke
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

# @brief Validate the canonical protected condition and executable gate contracts.
# @param $1 Workflow YAML path.
# @param $2 Exact stable gate step name.
# @return Zero after writing condition, guard, gate, and reject fixtures.
# @throws Nothing; missing contracts exit through fail.
# @note The canonical source assertion does not emulate GitHub's expression
#   evaluator; real shell run blocks are extracted for dynamic coverage.
validate_workflow_contract() {
  local workflow=$1
  local gate_step=$2
  local workflow_name
  local protected_job_file
  local condition_file
  local normalized_condition
  local canonical_condition
  local reject_line
  local checkout_line
  local mismatch_count
  local identity_output_count
  workflow_name=$(basename "$workflow" .yml)
  protected_job_file="$TEST_ROOT/${workflow_name}-protected-job.yml"
  condition_file="$TEST_ROOT/${workflow_name}-protected-condition.yml"
  canonical_condition="if:>-\${{github.event_name!='pull_request_target'||"
  canonical_condition+="!startsWith(github.head_ref,'CI/')||"
  canonical_condition+="github.event.pull_request.head.repo.full_name!="
  canonical_condition+="github.repository}}"

  extract_job_block "$workflow" protected-ci-paths "$protected_job_file" ||
    fail "$workflow_name protected-ci-paths job could not be extracted"
  extract_job_if_block "$protected_job_file" "$condition_file" ||
    fail "$workflow_name protected-ci-paths condition could not be extracted"
  normalized_condition=$(tr -d '[:space:]' < "$condition_file") ||
    fail "$workflow_name protected-ci-paths condition normalization failed"
  [[ "$normalized_condition" == "$canonical_condition" ]] ||
    fail "$workflow_name protected-ci-paths condition differs from the canonical expression"

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
  extract_step_run_block "$workflow" "Protect CI workflow paths" \
    "$TEST_ROOT/${workflow_name}-protected.sh" ||
    fail "$workflow_name protected-path run block could not be extracted"
  assert_file_contains "$TEST_ROOT/${workflow_name}-protected.sh" \
    "git diff --no-renames --name-only -z"
  assert_file_contains "$TEST_ROOT/${workflow_name}-protected.sh" \
    "if ! mapfile -d '' -t changed_files"
  assert_file_contains "$TEST_ROOT/${workflow_name}-protected.sh" \
    "Protected-path changed-path inventory read failed."
  assert_file_not_contains "$TEST_ROOT/${workflow_name}-protected.sh" \
    "awk '"
  bash -n "$TEST_ROOT/${workflow_name}-gate.sh"
  bash -n "$TEST_ROOT/${workflow_name}-fork-reject.sh"
  bash -n "$TEST_ROOT/${workflow_name}-protected.sh"
  assert_gate_checks_all_results "$TEST_ROOT/${workflow_name}-gate.sh" \
    "$gate_step"
  pass "$workflow_name-canonical-protected-contract"
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

# @brief Create a feature history whose only change is a protected newline path.
# @param $1 Destination work repository.
# @param $2 Destination bare origin repository.
# @param $3 Exact newline-containing path below ci/.
# @return Zero after main is published and the feature commit is checked out.
# @throws Nothing; Git and filesystem failures terminate through set -e.
# @note The production guard can fetch origin/main without network access.
create_protected_path_history() {
  local repository=$1
  local origin_repository=$2
  local newline_path=$3
  git init -q -b main "$repository"
  git -C "$repository" config user.name "CI Routing Test"
  git -C "$repository" config user.email "ci-routing@example.invalid"
  printf 'baseline\n' > "$repository/seed.txt"
  git -C "$repository" add seed.txt
  git -C "$repository" commit -qm baseline

  git init -q --bare "$origin_repository"
  git -C "$repository" remote add origin "$origin_repository"
  git -C "$repository" push -q -u origin main

  git -C "$repository" checkout -qb feature/newline-path
  mkdir -p "$repository/ci"
  printf 'protected input\n' > "$repository/$newline_path"
  git -C "$repository" add -- "$newline_path"
  git -C "$repository" commit -qm "protected newline path"
}

# @brief Run one extracted production protected-path block in a local repository.
# @param $1 Extracted production guard shell script.
# @param $2 Repository containing origin/main and the feature commit.
# @param $3 Fresh artifact directory for the guard invocation.
# @param $4 PATH value, optionally prefixed with a failure-injection shim.
# @return The production block's status.
# @throws Nothing; failures are returned to the calling regression assertion.
# @note Event variables select the non-authorized push path without evaluating
#   or executing any script from the synthetic head commit.
run_protected_path_guard() {
  local guard_script=$1
  local repository=$2
  local artifact_dir=$3
  local command_path=${4:-$PATH}
  rm -rf -- "$artifact_dir"
  mkdir -p "$artifact_dir"
  (
    cd "$repository"
    env PATH="$command_path" CI_ROUTING_TEST_REAL_GIT="$REAL_GIT" \
      CI_BRANCH_NAME=feature/newline-path CI_EVENT_NAME=push \
      CI_BASE_REPOSITORY=owner/repository CI_HEAD_REPOSITORY= \
      CI_HEAD_IS_BASE_REPOSITORY=false CI_BASE_BRANCH=main \
      CI_BASE_SHA=origin/main CI_HEAD_SHA=HEAD \
      CI_ARTIFACT_DIR="$artifact_dir" bash "$guard_script"
  )
}

# @brief Require one NUL-delimited inventory to contain one exact path record.
# @param $1 NUL-delimited inventory file.
# @param $2 Exact expected path, including any embedded newline.
# @return Zero when the single record matches; otherwise exits through fail.
# @throws Nothing; mapfile failures become regression diagnostics.
# @note Array comparison proves the path was neither quoted nor line-split.
assert_single_nul_path() {
  local inventory=$1
  local expected_path=$2
  local -a paths=()
  [[ -f "$inventory" ]] || fail "NUL inventory is missing: $inventory"
  if ! mapfile -d '' -t paths < "$inventory"; then
    fail "NUL inventory could not be read: $inventory"
  fi
  ((${#paths[@]} == 1)) ||
    fail "NUL inventory did not contain exactly one path: $inventory"
  [[ "${paths[0]}" == "$expected_path" ]] ||
    fail "NUL inventory path mismatch: $inventory"
}

# @brief Exercise one real guard against newline, producer, and reader failures.
# @param $1 Extracted production protected-path shell script.
# @param $2 Stable workflow label used for case and artifact names.
# @param $3 Isolated repository containing the protected newline-path change.
# @param $4 Exact newline-containing protected path.
# @return Zero after all failures close and artifacts retain safe path identity.
# @throws Nothing; unexpected status or artifact contents exit through fail.
# @note Git shims separately fail production and unlink the readable inventory.
exercise_protected_path_guard() {
  local guard_script=$1
  local workflow_label=$2
  local repository=$3
  local newline_path=$4
  local artifact_dir="$TEST_ROOT/${workflow_label}-newline-protected"
  local output_log="$TEST_ROOT/${workflow_label}-newline-protected.log"
  local failure_artifact_dir="$TEST_ROOT/${workflow_label}-producer-failure"
  local failure_log="$TEST_ROOT/${workflow_label}-producer-failure.log"
  local shim_dir="$TEST_ROOT/protected-failing-git"
  local read_failure_artifact_dir="$TEST_ROOT/${workflow_label}-read-failure"
  local read_failure_log="$TEST_ROOT/${workflow_label}-read-failure.log"
  local read_failure_shim_dir="$TEST_ROOT/protected-read-failing-git"
  local quoted_path

  run_expect_failure "$workflow_label-newline-protected-path-rejected" \
    "$output_log" \
    "Only trusted base-repository CI/** runs may change" \
    "No protected CI workflow paths changed" \
    run_protected_path_guard "$guard_script" "$repository" "$artifact_dir"
  printf -v quoted_path '%q' "$newline_path"
  grep -Fqx -- "$quoted_path" "$artifact_dir/changed-files.txt" ||
    fail "$workflow_label changed-path log did not safely quote the newline path"
  grep -Fqx -- "$quoted_path" "$artifact_dir/protected-files.txt" ||
    fail "$workflow_label protected artifact omitted the newline path"
  assert_single_nul_path "$artifact_dir/changed-files.z" "$newline_path"

  create_failing_git_shim "$shim_dir"
  run_expect_failure "$workflow_label-protected-producer-fails-closed" \
    "$failure_log" "Protected-path changed-path detection failed." \
    "No protected CI workflow paths changed" \
    run_protected_path_guard "$guard_script" "$repository" \
    "$failure_artifact_dir" "$shim_dir:$PATH"

  create_unlinking_git_inventory_shim "$read_failure_shim_dir"
  run_expect_failure "$workflow_label-protected-reader-fails-closed" \
    "$read_failure_log" \
    "Protected-path changed-path inventory read failed." \
    "No protected CI workflow paths changed" \
    run_protected_path_guard "$guard_script" "$repository" \
    "$read_failure_artifact_dir" "$read_failure_shim_dir:$PATH"
}

# @brief Create a CI branch with an early C++ commit and later docs-only push.
# @param $1 Destination work repository.
# @param $2 Destination bare origin repository containing the main baseline.
# @return Zero after printing main, event-before, and head SHAs in order.
# @throws Nothing; Git and filesystem failures terminate through set -e.
# @note The deliberately unformatted C++ file remains present at the final
#   head even though the final push increment contains only documentation.
create_healthcheck_scope_history() {
  local repository=$1
  local origin_repository=$2
  local main_sha
  local before_sha
  local head_sha

  git init -q -b main "$repository"
  git -C "$repository" config user.name "CI Routing Test"
  git -C "$repository" config user.email "ci-routing@example.invalid"
  printf 'baseline\n' > "$repository/seed.txt"
  git -C "$repository" add seed.txt
  git -C "$repository" commit -qm baseline
  main_sha=$(git -C "$repository" rev-parse HEAD)

  git init -q --bare "$origin_repository"
  git -C "$repository" remote add origin "$origin_repository"
  git -C "$repository" push -q -u origin main

  git -C "$repository" checkout -qb CI/cumulative-healthcheck
  mkdir -p "$repository/src"
  printf 'int badly_formatted( ){return 0;}\n' \
    > "$repository/src/bad.cpp"
  git -C "$repository" add src/bad.cpp
  git -C "$repository" commit -qm "early unformatted C++"
  before_sha=$(git -C "$repository" rev-parse HEAD)

  mkdir -p "$repository/docs"
  printf '# later documentation push\n' > "$repository/docs/note.md"
  git -C "$repository" add docs/note.md
  git -C "$repository" commit -qm "later documentation push"
  head_sha=$(git -C "$repository" rev-parse HEAD)

  printf '%s\n%s\n%s\n' "$main_sha" "$before_sha" "$head_sha"
}

# @brief Execute both production main-fetch blocks and compare static scopes.
# @param $1 Extracted published-image CI branch main-fetch shell block.
# @param $2 Extracted local-image CI branch main-fetch shell block.
# @return Zero after both fetches resolve main and both inventories are exact.
# @throws Nothing; production fetch or inventory mismatches exit through fail.
# @note This proves Git range behavior in isolation; GitHub expression
#   evaluation remains covered only by the exact production-source assertion.
exercise_healthcheck_scope_history() {
  local published_fetch_script=$1
  local local_fetch_script=$2
  local repository="$TEST_ROOT/healthcheck-scope-repository"
  local origin_repository="$TEST_ROOT/healthcheck-scope-origin.git"
  local history_file="$TEST_ROOT/healthcheck-scope-history.txt"
  local cumulative_inventory="$TEST_ROOT/healthcheck-cumulative-paths.z"
  local incremental_inventory="$TEST_ROOT/healthcheck-incremental-paths.z"
  local main_sha
  local before_sha
  local head_sha
  local fetch_script
  local fetched_main
  local -a history=()
  local -a cumulative_paths=()
  local -a incremental_paths=()

  create_healthcheck_scope_history "$repository" "$origin_repository" \
    > "$history_file"
  mapfile -t history < "$history_file"
  ((${#history[@]} == 3)) ||
    fail "healthcheck scope history did not expose three revisions"
  main_sha=${history[0]}
  before_sha=${history[1]}
  head_sha=${history[2]}

  grep -Fqx -- 'int badly_formatted( ){return 0;}' \
    "$repository/src/bad.cpp" ||
    fail "healthcheck scope fixture lost its unformatted C++ source"
  for fetch_script in "$published_fetch_script" "$local_fetch_script"; do
    git -C "$repository" update-ref -d refs/remotes/origin/main
    if ! (cd "$repository" && bash "$fetch_script"); then
      fail "production CI branch main-fetch block failed in isolated history"
    fi
    fetched_main=$(git -C "$repository" rev-parse origin/main)
    [[ "$fetched_main" == "$main_sha" ]] ||
      fail "production CI branch main-fetch block resolved the wrong main commit"
  done

  git -C "$repository" diff --no-renames --name-only -z \
    "origin/main...$head_sha" > "$cumulative_inventory"
  git -C "$repository" diff --no-renames --name-only -z \
    "$before_sha...$head_sha" > "$incremental_inventory"
  mapfile -d '' -t cumulative_paths < "$cumulative_inventory"
  mapfile -d '' -t incremental_paths < "$incremental_inventory"

  ((${#cumulative_paths[@]} == 2)) ||
    fail "cumulative main healthcheck scope did not contain two paths"
  [[ "${cumulative_paths[0]}" == docs/note.md &&
    "${cumulative_paths[1]}" == src/bad.cpp ]] ||
    fail "cumulative main healthcheck scope omitted the early C++ path"
  ((${#incremental_paths[@]} == 1)) ||
    fail "event-before healthcheck scope did not contain exactly one path"
  [[ "${incremental_paths[0]}" == docs/note.md ]] ||
    fail "event-before healthcheck scope was not docs-only"

  pass healthcheck-ci-branch-cumulative-main-retains-earlier-cpp
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

# @brief Create a Git shim that removes a completed protected-path inventory.
# @param $1 Destination directory added to the front of PATH.
# @return Zero after writing an executable shim.
# @throws Nothing; filesystem failures terminate through set -e.
# @note The real diff succeeds before unlinking the parent-visible file, which
#   forces the production mapfile read boundary to prove it fails closed.
create_unlinking_git_inventory_shim() {
  local shim_dir=$1
  mkdir -p "$shim_dir"
  cat > "$shim_dir/git" <<'EOF'
#!/usr/bin/env bash
set -Eeuo pipefail

# @file git
# @brief Remove the protected-path inventory after a successful real Git diff.
if [[ "${1:-}" == diff ]]; then
  for argument in "$@"; do
    if [[ "$argument" == --name-only ]]; then
      "$CI_ROUTING_TEST_REAL_GIT" "$@"
      status=$?
      if ((status == 0)); then
        rm -f -- "$CI_ARTIFACT_DIR/changed-files.z"
      fi
      exit "$status"
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
#   healthcheck; this test exact-locks workflow source and runs maintained shell
#   blocks without claiming to emulate the GitHub expression evaluator.
main() {
  local health_workflow="$REPO_ROOT/.github/workflows/ci-healthcheck.yml"
  local integration_workflow="$REPO_ROOT/.github/workflows/ci-integration.yml"
  local protected_repository="$TEST_ROOT/protected-repository"
  local protected_origin="$TEST_ROOT/protected-origin.git"
  local protected_newline_path=$'ci/hidden\nscript.sh'

  validate_workflow_contract "$health_workflow" "Report healthcheck gate"
  validate_workflow_contract "$integration_workflow" "Report integration gate"
  validate_published_image_base_fetch "$health_workflow"
  validate_published_image_fetch_shells "$health_workflow"
  validate_published_image_workspace_trust "$health_workflow"
  validate_local_image_base_fetch "$health_workflow"
  validate_ci_branch_healthcheck_base "$health_workflow" \
    healthcheck-published-image
  validate_ci_branch_healthcheck_base "$health_workflow" \
    healthcheck-local-image
  exercise_healthcheck_scope_history \
    "$TEST_ROOT/healthcheck-published-image-ci-main-fetch.sh" \
    "$TEST_ROOT/healthcheck-local-image-ci-main-fetch.sh"
  cmp -s "$TEST_ROOT/ci-healthcheck-protected.sh" \
    "$TEST_ROOT/ci-integration-protected.sh" ||
    fail "healthcheck and integration protected-path blocks diverged"
  pass protected-path-production-blocks-identical

  exercise_gate_identity "$TEST_ROOT/ci-healthcheck-gate.sh" healthcheck
  exercise_gate_identity "$TEST_ROOT/ci-integration-gate.sh" integration
  exercise_fork_rejection "$TEST_ROOT/ci-healthcheck-fork-reject.sh" \
    healthcheck
  exercise_fork_rejection "$TEST_ROOT/ci-integration-fork-reject.sh" \
    integration
  create_protected_path_history "$protected_repository" "$protected_origin" \
    "$protected_newline_path"
  exercise_protected_path_guard \
    "$TEST_ROOT/ci-healthcheck-protected.sh" healthcheck \
    "$protected_repository" "$protected_newline_path"
  exercise_protected_path_guard \
    "$TEST_ROOT/ci-integration-protected.sh" integration \
    "$protected_repository" "$protected_newline_path"
  exercise_changed_path_contracts

  printf 'All %d CI routing cases passed.\n' "$pass_count"
}

main "$@"
