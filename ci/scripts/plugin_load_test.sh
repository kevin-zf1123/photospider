#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

run_logged cmake_configure configure_ci_build
run_logged build_plugin_targets build_ci_targets \
  graph_cli \
  test_plugin_manager \
  test_scheduler_plugin_loader \
  lifecycle_op_plugin \
  override_lifecycle_op_plugin \
  destroy_count_scheduler_plugin \
  invert_op_custom_example \
  save_op \
  threshold_op \
  cpu_work_stealing_example_plugin \
  gpu_pipeline_example_plugin \
  serial_debug_example_plugin

find "$BUILD_DIR/plugins" -maxdepth 1 -type f | sort | tee "$CI_ARTIFACT_DIR/op_plugins.txt"
find "$BUILD_DIR/test_plugins" -type f | sort | tee "$CI_ARTIFACT_DIR/test_op_plugins.txt"
find "$BUILD_DIR/schedulers" -maxdepth 1 -type f | sort | tee "$CI_ARTIFACT_DIR/scheduler_plugins.txt"
find "$BUILD_DIR/test_schedulers" -maxdepth 1 -type f | sort | tee "$CI_ARTIFACT_DIR/test_scheduler_plugins.txt"

test -s "$CI_ARTIFACT_DIR/op_plugins.txt"
test -s "$CI_ARTIFACT_DIR/test_op_plugins.txt"
test -s "$CI_ARTIFACT_DIR/scheduler_plugins.txt"
test -s "$CI_ARTIFACT_DIR/test_scheduler_plugins.txt"

run_logged plugin_manager_gtest "$BUILD_DIR/tests/test_plugin_manager" --gtest_filter=PluginManagerLifecycleTest.*
run_logged scheduler_plugin_loader_gtest "$BUILD_DIR/tests/test_scheduler_plugin_loader" --gtest_filter=SchedulerPluginLoaderTest.*

config_path="$CI_ARTIFACT_DIR/plugin_cli_config.yaml"
mkdir -p "$CI_ARTIFACT_DIR/cache"
write_cli_config "$config_path" "$CI_ARTIFACT_DIR/cache"

cli_script="$CI_ARTIFACT_DIR/plugin_cli.repl"
cat > "$cli_script" <<EOF
scheduler scan $BUILD_DIR/schedulers
scheduler plugins
exit
EOF

cli_log="$CI_ARTIFACT_DIR/plugin_cli.log"
printf 'source %s\n' "$cli_script" |
  "$BUILD_DIR/bin/graph_cli" --config "$config_path" --repl > "$cli_log" 2>&1

require_grep "Scanned '.*/schedulers': loaded [0-9][0-9]* scheduler" "$cli_log"
require_grep "Loaded scheduler plugins" "$cli_log"
require_grep "cpu_work_stealing_example" "$cli_log"
require_grep "gpu_pipeline_example" "$cli_log"
require_grep "serial_debug_example" "$cli_log"

echo "plugin loading checks passed." | tee "$CI_ARTIFACT_DIR/summary.log"
