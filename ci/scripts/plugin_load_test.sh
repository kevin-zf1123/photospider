#!/usr/bin/env bash

set -Eeuo pipefail

# @file plugin_load_test.sh
# @brief Validate operation plugins plus the configured runtime extension model.
# @note Runtime capability comes only from the configured CMake target inventory;
#   legacy scheduler and policy/execution checks are mutually exclusive.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

ensure_ci_configured cmake_configure
capture_ci_target_inventory
runtime_contract=$(ci_runtime_contract)
plugin_targets=(
  graph_cli
  test_plugin_manager
  lifecycle_op_plugin
  override_lifecycle_op_plugin
  invert_op_custom_example
  save_op
  threshold_op
)
case "$runtime_contract" in
  legacy_scheduler)
    plugin_targets+=(
      test_scheduler_plugin_loader
      destroy_count_scheduler_plugin
      cpu_work_stealing_example_plugin
      gpu_pipeline_example_plugin
      serial_debug_example_plugin
    )
    ;;
  policy_execution)
    plugin_targets+=(
      fifo_policy
      test_policy_plugin
      test_policy_registry
      test_policy_execution
      test_cli_policy_execution_config
    )
    ;;
esac
run_logged validate_plugin_targets require_ci_targets "${plugin_targets[@]}"
ensure_ci_targets build_plugin_targets "${plugin_targets[@]}"

find "$BUILD_DIR/plugins" -maxdepth 1 -type f | sort | tee "$CI_ARTIFACT_DIR/op_plugins.txt"
find "$BUILD_DIR/test_plugins" -type f | sort | tee "$CI_ARTIFACT_DIR/test_op_plugins.txt"

test -s "$CI_ARTIFACT_DIR/op_plugins.txt"
test -s "$CI_ARTIFACT_DIR/test_op_plugins.txt"

run_gtest_checked plugin_manager_gtest \
  "$BUILD_DIR/tests/test_plugin_manager" \
  "PluginManagerLifecycleTest.*"

case "$runtime_contract" in
  legacy_scheduler)
    find "$BUILD_DIR/schedulers" -maxdepth 1 -type f |
      sort | tee "$CI_ARTIFACT_DIR/scheduler_plugins.txt"
    find "$BUILD_DIR/test_schedulers" -maxdepth 1 -type f |
      sort | tee "$CI_ARTIFACT_DIR/test_scheduler_plugins.txt"
    test -s "$CI_ARTIFACT_DIR/scheduler_plugins.txt"
    test -s "$CI_ARTIFACT_DIR/test_scheduler_plugins.txt"
    run_gtest_checked scheduler_plugin_loader_gtest \
      "$BUILD_DIR/tests/test_scheduler_plugin_loader" \
      "SchedulerPluginLoaderTest.*"
    ;;
  policy_execution)
    find "$BUILD_DIR/policies" -maxdepth 1 -type f |
      sort | tee "$CI_ARTIFACT_DIR/policy_plugins.txt"
    find "$BUILD_DIR/test_policies" -maxdepth 1 -type f |
      sort | tee "$CI_ARTIFACT_DIR/test_policy_plugins.txt"
    test -s "$CI_ARTIFACT_DIR/policy_plugins.txt"
    test -s "$CI_ARTIFACT_DIR/test_policy_plugins.txt"
    run_gtest_checked policy_registry_gtest \
      "$BUILD_DIR/tests/test_policy_registry" ""
    run_gtest_checked policy_execution_gtest \
      "$BUILD_DIR/tests/test_policy_execution" ""
    run_gtest_checked policy_cli_config_gtest \
      "$BUILD_DIR/tests/test_cli_policy_execution_config" ""
    ;;
esac

config_path="$CI_ARTIFACT_DIR/plugin_cli_config.yaml"
mkdir -p "$CI_ARTIFACT_DIR/cache"
write_cli_config "$config_path" "$CI_ARTIFACT_DIR/cache" "$runtime_contract"

cli_script="$CI_ARTIFACT_DIR/plugin_cli.repl"
case "$runtime_contract" in
  legacy_scheduler)
    cat > "$cli_script" <<EOF
scheduler scan $BUILD_DIR/schedulers
scheduler plugins
exit
EOF
    ;;
  policy_execution)
    cat > "$cli_script" <<EOF
policy scan $BUILD_DIR/test_policies
policy plugins
policy list
execution list
exit
EOF
    ;;
esac

cli_log="$CI_ARTIFACT_DIR/plugin_cli.log"
printf 'source %s\n' "$cli_script" |
  "$BUILD_DIR/bin/graph_cli" --config "$config_path" --repl > "$cli_log" 2>&1

case "$runtime_contract" in
  legacy_scheduler)
    require_grep \
      "Scanned '.*/schedulers': loaded [0-9][0-9]* scheduler" "$cli_log"
    require_grep "Loaded scheduler plugins" "$cli_log"
    require_grep "cpu_work_stealing_example" "$cli_log"
    require_grep "gpu_pipeline_example" "$cli_log"
    require_grep "serial_debug_example" "$cli_log"
    ;;
  policy_execution)
    require_grep "Loaded [1-9][0-9]* policy plugin[(]s[)]" "$cli_log"
    require_grep "Loaded policy plugins" "$cli_log"
    require_grep "fifo_policy" "$cli_log"
    require_grep "test_policy_plugin" "$cli_log"
    require_grep "Available policy types" "$cli_log"
    require_grep "fixture_policy" "$cli_log"
    require_grep "Available execution types" "$cli_log"
    require_grep "gpu_pipeline" "$cli_log"
    require_grep "serial_debug" "$cli_log"
    ;;
esac

echo "$runtime_contract plugin loading checks passed." |
  tee "$CI_ARTIFACT_DIR/summary.log"
