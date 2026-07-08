#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

CI_ARTIFACT_ROOT=${CI_ARTIFACT_ROOT:-"$REPO_ROOT/CI-results"}

CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/build-integrity" \
  bash "$SCRIPT_DIR/build_integrity.sh"
CI_REUSE_BUILD=ON CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/ctest-full" \
  bash "$SCRIPT_DIR/ctest_full.sh"
CI_REUSE_BUILD=ON CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/graph-cli" \
  bash "$SCRIPT_DIR/graph_cli_script_test.sh"
CI_REUSE_BUILD=ON CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/propagation" \
  bash "$SCRIPT_DIR/propagation_script_test.sh"
CI_REUSE_BUILD=ON CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/plugin-load" \
  bash "$SCRIPT_DIR/plugin_load_test.sh"
CI_REUSE_BUILD=ON \
  CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/scheduler-repeat" \
  SCHEDULER_REPEAT="${SCHEDULER_REPEAT:-5}" \
  GPU_PIPELINE_REPEAT="${GPU_PIPELINE_REPEAT:-3}" \
  bash "$SCRIPT_DIR/scheduler_repeat_test.sh"
