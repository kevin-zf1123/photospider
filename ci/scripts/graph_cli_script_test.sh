#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

run_logged cmake_configure configure_ci_build
run_logged build_graph_cli build_ci_targets graph_cli cpu_work_stealing_example_plugin serial_debug_example_plugin

mkdir -p "$CI_ARTIFACT_DIR/cache" "$CI_ARTIFACT_DIR/sessions"
config_path="$CI_ARTIFACT_DIR/graph_cli_config.yaml"
write_cli_config "$config_path" "$CI_ARTIFACT_DIR/cache"

positive_script="$CI_ARTIFACT_DIR/graph_cli_positive.repl"
cat > "$positive_script" <<EOF
load ci_graph util/testcases/propagation_linear_test.yaml
compute all parallel nosave m
traversal
exit
EOF

positive_log="$CI_ARTIFACT_DIR/graph_cli_positive.log"
printf 'source %s\n' "$positive_script" |
  "$BUILD_DIR/bin/graph_cli" --config "$config_path" --repl \
    > "$positive_log" 2>&1

require_grep "Loaded session 'ci_graph'" "$positive_log"
require_grep "Computation finished" "$positive_log"
require_grep "Post-order|Dependency Tree" "$positive_log"

negative_script="$CI_ARTIFACT_DIR/graph_cli_negative.repl"
cat > "$negative_script" <<EOF
load ci_missing does/not/exist.yaml
compute nope
unknown-ci-command
exit
EOF

negative_log="$CI_ARTIFACT_DIR/graph_cli_negative.log"
printf 'source %s\n' "$negative_script" |
  "$BUILD_DIR/bin/graph_cli" --config "$config_path" --repl \
    > "$negative_log" 2>&1

require_grep "Warning: source YAML file not found for graph 'ci_missing'" "$negative_log"
require_grep "Invalid target 'nope'" "$negative_log"
require_grep "Unknown command: unknown-ci-command" "$negative_log"

echo "graph_cli positive and negative scripted checks passed." |
  tee "$CI_ARTIFACT_DIR/summary.log"
