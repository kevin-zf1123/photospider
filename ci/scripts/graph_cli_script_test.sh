#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

graph_document_test_source="$REPO_ROOT/tests/integration/test_graph_document_errors.cpp"
has_graph_document_test_source=false
has_graph_document_test_target=false
has_graph_document_test_discovery=false

if [[ -f "$graph_document_test_source" ]]; then
  has_graph_document_test_source=true
fi
if grep -Eq \
  '^[[:space:]]*add_ps_test\(test_graph_document_errors([[:space:]]|$)' \
  "$REPO_ROOT/CMakeLists.txt"; then
  has_graph_document_test_target=true
fi
if grep -Eq \
  '^[[:space:]]*gtest_discover_tests\(test_graph_document_errors([[:space:]]|$)' \
  "$REPO_ROOT/CMakeLists.txt"; then
  has_graph_document_test_discovery=true
fi

capability_markers="$has_graph_document_test_source:$has_graph_document_test_target"
capability_markers+=":$has_graph_document_test_discovery"
case "$capability_markers" in
  false:false:false)
    missing_source_contract=legacy_publication
    ;;
  true:true:true)
    missing_source_contract=transactional_rejection
    ;;
  *)
    echo "Inconsistent graph document transaction capability markers." >&2
    echo "source=$has_graph_document_test_source" \
      "target=$has_graph_document_test_target" \
      "discovery=$has_graph_document_test_discovery" >&2
    exit 1
    ;;
esac

printf 'Graph document missing-source contract: %s\n' \
  "$missing_source_contract" |
  tee "$CI_ARTIFACT_DIR/capability.log"

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

# Run missing-source publication independently from invalid target parsing so
# each revision has one explicit, capability-selected contract.
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

case "$missing_source_contract" in
  legacy_publication)
    require_grep \
      "Warning: source YAML file not found for graph 'ci_missing'" \
      "$missing_source_log"
    require_grep "Loaded session 'ci_missing'" "$missing_source_log"
    require_grep "Loaded graphs:" "$missing_source_log"
    require_grep "  - ci_missing  \\[current\\]" "$missing_source_log"
    require_grep "No ending nodes to compute in the graph[.]" \
      "$missing_source_log"
    if grep -Fq "Error: failed to load session 'ci_missing'" \
      "$missing_source_log" ||
      grep -Fq "(no graphs loaded)" "$missing_source_log" ||
      grep -Fq "No current graph. Use load/switch." "$missing_source_log"; then
      echo "Legacy missing-source behavior matched transactional output." >&2
      exit 1
    fi
    ;;
  transactional_rejection)
    require_grep \
      "Error: failed to load session 'ci_missing' from 'does/not/exist[.]yaml'[.]" \
      "$missing_source_log"
    require_grep "[(]no graphs loaded[)]" "$missing_source_log"
    require_grep "No current graph[.] Use load/switch[.]" \
      "$missing_source_log"
    if grep -Fq "Warning: source YAML file not found for graph 'ci_missing'" \
      "$missing_source_log" ||
      grep -Fq "Loaded session 'ci_missing'" "$missing_source_log" ||
      grep -Fq "Loaded graphs:" "$missing_source_log"; then
      echo "Transactional missing-source behavior published legacy state." >&2
      exit 1
    fi
    ;;
esac

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

echo "graph_cli positive, $missing_source_contract missing-source," \
  "and invalid-target checks passed." |
  tee "$CI_ARTIFACT_DIR/summary.log"
