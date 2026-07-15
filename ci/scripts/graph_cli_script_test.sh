#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

ensure_ci_configured cmake_configure
ensure_ci_targets build_graph_cli graph_cli cpu_work_stealing_example_plugin serial_debug_example_plugin

fixture_path="$REPO_ROOT/util/testcases/propagation_linear_test.yaml"
runtime_root=$(mktemp -d "$CI_ARTIFACT_DIR/graph_cli_runtime.XXXXXX")
trap 'rm -rf -- "$runtime_root"' EXIT

positive_runtime="$runtime_root/positive"
missing_source_runtime="$runtime_root/missing-source"
invalid_target_runtime="$runtime_root/invalid-target"
mkdir -p "$CI_ARTIFACT_DIR/cache" \
  "$positive_runtime/home" \
  "$missing_source_runtime/home" \
  "$invalid_target_runtime/home"

config_path="$CI_ARTIFACT_DIR/graph_cli_config.yaml"
write_cli_config "$config_path" "$CI_ARTIFACT_DIR/cache"

positive_script="$CI_ARTIFACT_DIR/graph_cli_positive.repl"
cat > "$positive_script" <<EOF
load ci_graph $fixture_path
compute all parallel nosave m
traversal
exit
EOF

positive_log="$CI_ARTIFACT_DIR/graph_cli_positive.log"
(
  cd "$positive_runtime"
  printf 'source %s\n' "$positive_script" |
    HOME="$positive_runtime/home" \
      "$BUILD_DIR/bin/graph_cli" --config "$config_path" --repl \
      > "$positive_log" 2>&1
)

require_grep "Loaded session 'ci_graph'" "$positive_log"
require_grep "Computation finished" "$positive_log"
require_grep "Post-order|Dependency Tree" "$positive_log"

# An explicit missing source must fail without publishing a session or current
# graph. The graphs and compute commands independently expose both boundaries.
missing_source_script="$CI_ARTIFACT_DIR/graph_cli_missing_source.repl"
cat > "$missing_source_script" <<EOF
load ci_missing does/not/exist.yaml
graphs
compute all
exit
EOF

missing_source_log="$CI_ARTIFACT_DIR/graph_cli_missing_source.log"
(
  cd "$missing_source_runtime"
  printf 'source %s\n' "$missing_source_script" |
    HOME="$missing_source_runtime/home" \
      "$BUILD_DIR/bin/graph_cli" --config "$config_path" --repl \
      > "$missing_source_log" 2>&1
)

require_grep \
  "Error: failed to load session 'ci_missing' from 'does/not/exist[.]yaml'[.]" \
  "$missing_source_log"
require_grep "[(]no graphs loaded[)]" "$missing_source_log"
require_grep "No current graph[.] Use load/switch[.]" "$missing_source_log"
if grep -Fq "Loaded session 'ci_missing'" "$missing_source_log"; then
  echo "Unexpectedly published missing-source session 'ci_missing'." >&2
  exit 1
fi

# Invalid target parsing requires a valid current graph; keep it independent
# from the missing-source transaction above by loading a maintained fixture.
invalid_target_script="$CI_ARTIFACT_DIR/graph_cli_invalid_target.repl"
cat > "$invalid_target_script" <<EOF
load ci_invalid_target $fixture_path
compute nope
unknown-ci-command
exit
EOF

invalid_target_log="$CI_ARTIFACT_DIR/graph_cli_invalid_target.log"
(
  cd "$invalid_target_runtime"
  printf 'source %s\n' "$invalid_target_script" |
    HOME="$invalid_target_runtime/home" \
      "$BUILD_DIR/bin/graph_cli" --config "$config_path" --repl \
      > "$invalid_target_log" 2>&1
)

require_grep "Loaded session 'ci_invalid_target'" "$invalid_target_log"
require_grep "Invalid target 'nope'" "$invalid_target_log"
require_grep "Unknown command: unknown-ci-command" "$invalid_target_log"

echo "graph_cli positive, missing-source, and invalid-target checks passed." |
  tee "$CI_ARTIFACT_DIR/summary.log"
