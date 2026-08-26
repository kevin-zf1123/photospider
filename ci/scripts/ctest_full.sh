#!/usr/bin/env bash

set -Eeuo pipefail

# @file ctest_full.sh
# @brief Run the ordinary CTest shard in parallel while excluding the exact
#   build-smoke label and preserving complete log/JUnit evidence.
# @note The exclusion is label-based and contains no maintained test-name list.
#   ``CI_JOBS`` is validated and exported by common.sh, so direct CTest and any
#   nested CMake subprocess share one bounded parallelism contract.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

ensure_ci_configured cmake_configure
ensure_ci_all build_all
run_logged ctest_full \
  ctest --output-on-failure --test-dir "$BUILD_DIR" \
    --parallel "$CI_JOBS" \
    --output-junit "$CI_ARTIFACT_DIR/ctest-full.junit.xml" \
    --label-exclude "^${BUILD_SMOKE_LABEL}$"
