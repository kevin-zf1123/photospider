#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

ensure_ci_configured cmake_configure
ensure_ci_all build_all
CTEST_EXCLUDE_REGEX=${CTEST_EXCLUDE_REGEX:-"^SplitComputeServiceRuntimeTrace$"}
if [[ -n "$CTEST_EXCLUDE_REGEX" ]]; then
  run_logged ctest_full ctest --output-on-failure --test-dir "$BUILD_DIR" -E "$CTEST_EXCLUDE_REGEX"
else
  run_logged ctest_full ctest --output-on-failure --test-dir "$BUILD_DIR"
fi
