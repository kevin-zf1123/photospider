#!/usr/bin/env bash

set -Eeuo pipefail

# @file integration_suite.sh
# @brief Run local-image integration from one complete reusable producer.
# @note Build-smoke names come only from post-build strict discovery and are
#   consumed synchronously as NUL-delimited exact values.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

CI_ARTIFACT_ROOT=${CI_ARTIFACT_ROOT:-"$REPO_ROOT/CI-results"}
DEFAULT_BUILD_DIR=$BUILD_DIR

# @var BUILD_SMOKE_NAMES
# @brief Post-build NUL-delimited exact-name inventory from build-integrity.
# @note The file is read only after build-integrity succeeds and remains owned
#   by the caller-selected artifact root.
BUILD_SMOKE_NAMES="$CI_ARTIFACT_ROOT/build-integrity/build_smoke_names.z"
suite_status=0

# @brief Run one independent integration shard and retain aggregate failure.
# @param $@ Command and arguments for the shard.
# @return Zero so later independent shards still run.
# @throws Nothing; child failure is stored in suite_status.
# @note suite_status preserves failure for the script's final exit status.
run_integration_check() {
  if ! "$@"; then
    suite_status=1
  fi
}

CI_BUILD_PROFILE=default BUILD_TESTING=ON BUILD_DIR="$DEFAULT_BUILD_DIR" \
  CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/build-integrity" \
  bash "$SCRIPT_DIR/build_integrity.sh"

# @var build_smoke_tests
# @brief In-memory exact CTest names decoded from the post-build NUL inventory.
# @note Embedded newlines and shell metacharacters remain single array elements;
#   the array lives only for this script process.
build_smoke_tests=()
if ! mapfile -d '' -t build_smoke_tests < "$BUILD_SMOKE_NAMES"; then
  echo "Build-smoke name inventory could not be read." >&2
  exit 1
fi
if ((${#build_smoke_tests[@]} == 0)); then
  echo "Build-smoke name inventory is empty." >&2
  exit 1
fi

run_integration_check env \
  CI_BUILD_PROFILE=default BUILD_DIR="$DEFAULT_BUILD_DIR" CI_REUSE_BUILD=ON \
  CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/ctest-full" \
  bash "$SCRIPT_DIR/ctest_full.sh"

# @var build_smoke_index
# @brief One-based deterministic artifact-directory sequence for local fallback.
# @note The index is presentation metadata only; execution selection uses the
#   exact test name and fresh CTest validation.
build_smoke_index=0

# @var build_smoke_test
# @brief Current exact CTest name selected from build_smoke_tests.
# @note Each value is forwarded as one environment string and is never evaluated.
for build_smoke_test in "${build_smoke_tests[@]}"; do
  build_smoke_index=$((build_smoke_index + 1))

  # @var build_smoke_artifact_key
  # @brief Fixed-width local artifact key for the current sequential smoke.
  # @note The key contains no test-name content and is overwritten each loop.
  printf -v build_smoke_artifact_key '%03d' "$build_smoke_index"
  run_integration_check env \
    CI_BUILD_PROFILE=default BUILD_DIR="$DEFAULT_BUILD_DIR" CI_REUSE_BUILD=ON \
    SMOKE_TEST_NAME="$build_smoke_test" \
    CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/build-smoke/$build_smoke_artifact_key" \
    bash "$SCRIPT_DIR/build_smoke_test.sh"
done

run_integration_check env \
  CI_BUILD_PROFILE=default BUILD_DIR="$DEFAULT_BUILD_DIR" CI_REUSE_BUILD=ON \
  CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/graph-cli" \
  bash "$SCRIPT_DIR/graph_cli_script_test.sh"
run_integration_check env \
  CI_BUILD_PROFILE=default BUILD_DIR="$DEFAULT_BUILD_DIR" CI_REUSE_BUILD=ON \
  CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/propagation" \
  bash "$SCRIPT_DIR/propagation_script_test.sh"
run_integration_check env \
  CI_BUILD_PROFILE=default BUILD_DIR="$DEFAULT_BUILD_DIR" CI_REUSE_BUILD=ON \
  CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/plugin-load" \
  bash "$SCRIPT_DIR/plugin_load_test.sh"
run_integration_check env \
  CI_BUILD_PROFILE=default BUILD_DIR="$DEFAULT_BUILD_DIR" CI_REUSE_BUILD=ON \
  CI_ARTIFACT_DIR="$CI_ARTIFACT_ROOT/scheduler-repeat" \
  SCHEDULER_REPEAT="${SCHEDULER_REPEAT:-5}" \
  GPU_PIPELINE_REPEAT="${GPU_PIPELINE_REPEAT:-3}" \
  bash "$SCRIPT_DIR/scheduler_repeat_test.sh"

exit "$suite_status"
