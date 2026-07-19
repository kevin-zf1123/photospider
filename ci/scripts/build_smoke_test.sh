#!/usr/bin/env bash

set -Eeuo pipefail

# @file build_smoke_test.sh
# @brief Revalidate and run one exact labelled test from a reusable full build.
# @note SMOKE_TEST_NAME is workflow data, not shell or regex source; the Python
#   runner resolves it to a fresh validated numeric CTest index.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

# @var SMOKE_TEST_NAME
# @brief Required exact CTest identity selected by one dynamic matrix entry.
# @note The value remains process-local and is forwarded as one argv element.
if [[ -z "${SMOKE_TEST_NAME:-}" ]]; then
  echo "SMOKE_TEST_NAME must select one exact labelled CTest name." >&2
  exit 2
fi
if [[ -n "${CI_BUILD_PROFILE:-}" && "$CI_BUILD_PROFILE" != default ]]; then
  echo "Labelled build smokes require CI_BUILD_PROFILE=default." >&2
  exit 2
fi
CI_BUILD_PROFILE=default
BUILD_TESTING=ON
unset PHOTOSPIDER_BUILD_IPC || true

# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

if ! ci_reuse_build_enabled || ! ci_build_is_reusable; then
  echo "Build smoke requires a reusable default build at $BUILD_DIR." >&2
  exit 1
fi

ensure_ci_configured cmake_configure
ensure_ci_all build_all
run_logged selected_build_smoke \
  python3 -B "$SCRIPT_DIR/build_smoke_inventory.py" run \
    --build-dir "$BUILD_DIR" \
    --ctest-executable "${CTEST_COMMAND:-ctest}" \
    --config "${CMAKE_BUILD_TYPE:-RelWithDebInfo}" \
    --label "$BUILD_SMOKE_LABEL" \
    --inventory-output "$CI_ARTIFACT_DIR/ctest_inventory.json" \
    --selection-output "$CI_ARTIFACT_DIR/selection.json" \
    --test-name "$SMOKE_TEST_NAME"

printf 'Build smoke %q completed.\n' "$SMOKE_TEST_NAME" |
  tee "$CI_ARTIFACT_DIR/summary.log"
