#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

run_logged cmake_configure configure_ci_build
run_logged build_propagation_tool build_ci_targets test_propagation

linear_log="$CI_ARTIFACT_DIR/propagation_linear_tiles.log"
printf 'tiles all\nexit\n' |
  "$BUILD_DIR/tests/test_propagation" util/testcases/propagation_linear_test.yaml \
    > "$linear_log" 2>&1

require_grep "Graph loaded from 'util/testcases/propagation_linear_test.yaml'" "$linear_log"
require_grep "1 UNDEFINED 16 16 1024 1024" "$linear_log"
require_grep "2 MACRO 4 4 1024 1024" "$linear_log"
require_grep "200 MACRO 2 2 512 512" "$linear_log"

complex_log="$CI_ARTIFACT_DIR/propagation_complex_tiles.log"
printf 'tiles all\nexit\n' |
  "$BUILD_DIR/tests/test_propagation" util/testcases/propagation_complex_test.yaml \
    > "$complex_log" 2>&1

require_grep "Graph loaded from 'util/testcases/propagation_complex_test.yaml'" "$complex_log"
require_grep "10 UNDEFINED 8 8 512 512" "$complex_log"
require_grep "20 UNDEFINED 8 8 512 512" "$complex_log"
require_grep "200 MACRO 2 2 512 512" "$complex_log"

cat > "$CI_ARTIFACT_DIR/summary.log" <<EOF
propagation scripted checks passed.
Covered commands:
- test_propagation propagation_linear_test.yaml: tiles all
- test_propagation propagation_complex_test.yaml: tiles all
EOF
