#!/usr/bin/env bash

set -Eeuo pipefail

# @file runtime_capability_test.sh
# @brief Regress fail-closed runtime target discovery and CLI config emission.
# @note The test uses only disposable target-help fixtures and writes no source
#   or build state.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/photospider-runtime-capability.XXXXXX")
CI_ARTIFACT_DIR="$TEST_ROOT/artifacts"
BUILD_DIR="$TEST_ROOT/build"
mkdir -p "$CI_ARTIFACT_DIR" "$BUILD_DIR"
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

pass_count=0

# @brief Remove the disposable test root.
# @return Nothing.
# @throws Nothing; cleanup is best effort after a recorded test result.
cleanup() {
  rm -rf -- "$TEST_ROOT"
}
trap cleanup EXIT

# @brief Stop the regression with one diagnostic.
# @param $1 Failure description.
# @return Does not return.
# @throws Nothing; exits the script with status one.
fail() {
  echo "FAIL: $1" >&2
  exit 1
}

# @brief Record one passing case.
# @param $1 Stable case label.
# @return Zero.
# @throws Nothing.
pass() {
  pass_count=$((pass_count + 1))
  echo "PASS: $1"
}

# @brief Replace the parser input with exact target-help lines.
# @param $@ Target-help lines.
# @return Zero after writing the fixture.
# @throws Nothing; filesystem failure terminates through strict shell mode.
write_inventory() {
  printf '%s\n' "$@" > "$CI_TARGET_INVENTORY_FILE"
}

# @brief Require an exact string equality.
# @param $1 Actual value.
# @param $2 Expected value.
# @param $3 Case label.
# @return Zero when equal.
# @throws Nothing; mismatch exits through fail.
assert_equal() {
  local actual=$1
  local expected=$2
  local label=$3
  [[ "$actual" == "$expected" ]] ||
    fail "$label: expected '$expected', got '$actual'"
}

# @brief Require one fixed string in a file.
# @param $1 File path.
# @param $2 Fixed expected text.
# @param $3 Case label.
# @return Zero when present.
# @throws Nothing; mismatch exits through fail.
assert_contains() {
  local file=$1
  local text=$2
  local label=$3
  grep -Fq -- "$text" "$file" || fail "$label: missing '$text'"
}

# @brief Require one fixed string to be absent from a file.
# @param $1 File path.
# @param $2 Forbidden fixed text.
# @param $3 Case label.
# @return Zero when absent.
# @throws Nothing; mismatch exits through fail.
assert_not_contains() {
  local file=$1
  local text=$2
  local label=$3
  if grep -Fq -- "$text" "$file"; then
    fail "$label: unexpected '$text'"
  fi
}

# @brief Require runtime-contract detection to fail.
# @param $1 Case label.
# @return Zero after a nonzero classifier result.
# @throws Nothing; unexpected success exits through fail.
expect_contract_failure() {
  local label=$1
  local output
  local status
  set +e
  output=$(ci_runtime_contract 2>&1)
  status=$?
  set -e
  ((status != 0)) || fail "$label: invalid inventory was accepted"
  [[ "$output" == *"Invalid runtime capability inventory:"* ]] ||
    fail "$label: missing fail-closed diagnostic"
  pass "$label"
}

# @brief Exercise Make-style exact legacy-scheduler discovery.
# @return Zero after recording the case.
# @throws Nothing; assertion failures exit through fail.
test_legacy_make_inventory() {
  write_inventory \
    "The following are some of the valid targets for this Makefile:" \
    "... test_scheduler" \
    "... test_scheduler_plugin_loader" \
    "... destroy_count_scheduler_plugin" \
    "... test_scheduler_extra"
  local contract
  contract=$(ci_runtime_contract)
  assert_equal "$contract" legacy_scheduler legacy-make-contract
  ci_target_exists test_scheduler ||
    fail "legacy-make-contract: exact target was not found"
  if ci_target_exists test_policy_execution; then
    fail "legacy-make-contract: absent policy target was found"
  fi
  pass legacy-make-contract
}

# @brief Exercise Ninja-style exact policy/execution discovery.
# @return Zero after recording the case.
# @throws Nothing; assertion failures exit through fail.
test_policy_ninja_inventory() {
  write_inventory \
    "test_policy_execution: phony" \
    "test_policy_registry: phony" \
    "test_policy_plugin: phony" \
    "test_policy_execution_extra: phony"
  local contract
  contract=$(ci_runtime_contract)
  assert_equal "$contract" policy_execution policy-ninja-contract
  ci_target_exists test_policy_execution ||
    fail "policy-ninja-contract: exact target was not found"
  if ci_target_exists test_scheduler; then
    fail "policy-ninja-contract: absent scheduler target was found"
  fi
  pass policy-ninja-contract
}

# @brief Exercise partial, mixed, and empty fail-closed inventories.
# @return Zero after recording all cases.
# @throws Nothing; assertion failures exit through fail.
test_invalid_inventories() {
  write_inventory \
    "... test_scheduler" \
    "... test_scheduler_plugin_loader"
  expect_contract_failure partial-legacy-contract

  write_inventory \
    "... test_scheduler" \
    "... test_scheduler_plugin_loader" \
    "... destroy_count_scheduler_plugin" \
    "... test_policy_execution" \
    "... test_policy_registry" \
    "... test_policy_plugin"
  expect_contract_failure mixed-runtime-contract

  write_inventory "... graph_cli"
  expect_contract_failure absent-runtime-contract
}

# @brief Exercise complete required-target validation.
# @return Zero after recording the case.
# @throws Nothing; assertion failures exit through fail.
test_required_targets() {
  local failure_log="$TEST_ROOT/required-targets-failure.log"
  local status
  write_inventory \
    "... photospider_kernel" \
    "... graph_cli"
  require_ci_targets photospider_kernel graph_cli ||
    fail "required-targets: complete inventory was rejected"
  set +e
  require_ci_targets photospider_kernel missing_target \
    > "$failure_log" 2>&1
  status=$?
  set -e
  ((status != 0)) ||
    fail "required-targets: absent target was accepted"
  assert_contains "$failure_log" \
    "Required configured CMake target is missing: missing_target" \
    required-targets
  pass required-targets
}

# @brief Exercise mutually exclusive legacy and policy CLI configurations.
# @return Zero after recording both cases.
# @throws Nothing; assertion failures exit through fail.
test_cli_configs() {
  local legacy_config="$TEST_ROOT/legacy.yaml"
  local policy_config="$TEST_ROOT/policy.yaml"
  local invalid_log="$TEST_ROOT/invalid-config.log"
  local status

  write_cli_config "$legacy_config" "$TEST_ROOT/cache-legacy" \
    legacy_scheduler
  assert_contains "$legacy_config" \
    "scheduler_hp_type: cpu_work_stealing" legacy-cli-config
  assert_not_contains "$legacy_config" "policy_dirs:" legacy-cli-config
  assert_not_contains "$legacy_config" \
    "execution_worker_count:" legacy-cli-config
  pass legacy-cli-config

  write_cli_config "$policy_config" "$TEST_ROOT/cache-policy" \
    policy_execution
  assert_contains "$policy_config" "policy_dirs:" policy-cli-config
  assert_contains "$policy_config" \
    "policy_interactive_type: fifo" policy-cli-config
  assert_contains "$policy_config" \
    "policy_throughput_type: fifo" policy-cli-config
  assert_contains "$policy_config" \
    "execution_worker_count: 0" policy-cli-config
  assert_not_contains "$policy_config" "scheduler_dirs:" policy-cli-config
  assert_not_contains "$policy_config" \
    "scheduler_worker_count:" policy-cli-config
  pass policy-cli-config

  set +e
  write_cli_config "$TEST_ROOT/invalid.yaml" "$TEST_ROOT/cache-invalid" \
    unsupported_contract > "$invalid_log" 2>&1
  status=$?
  set -e
  ((status != 0)) ||
    fail "invalid-cli-config: unsupported contract was accepted"
  assert_contains "$invalid_log" \
    "Unsupported CI runtime contract: unsupported_contract" \
    invalid-cli-config
  pass invalid-cli-config
}

# @brief Run every runtime capability regression.
# @return Zero after printing the exact pass count.
# @throws Nothing; a child assertion exits the script.
main() {
  test_legacy_make_inventory
  test_policy_ninja_inventory
  test_invalid_inventories
  test_required_targets
  test_cli_configs
  printf 'All %d runtime capability cases passed.\n' "$pass_count"
}

main "$@"
