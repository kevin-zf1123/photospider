#!/usr/bin/env bash

set -Eeuo pipefail

# @file openexr_smoke_test.sh
# @brief Execute the exact current-main OpenEXR option-off build smoke from the
#   source checkout with only verified producer CMake metadata.
# @note This protected transitional runner owns exactly
#   OpenExrDeepProviderOptionOffSmoke. It does not restore a CTest graph,
#   generated inventory, producer stamp, object, or product library. The future
#   versioned profile matrix replaces this exact-name bridge before closeout.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
ROLE=${CI_ARTIFACT_ROLE:-}
SMOKE_NAME=${SMOKE_TEST_NAME:-}
METADATA_ROOT=${CI_OPENEXR_METADATA_ROOT:-$REPO_ROOT/CI-results/openexr-metadata-consumer/ci}
BUILD_DIR="$METADATA_ROOT/producer"

if [[ "$ROLE" != openexr-metadata ]]; then
  echo "OpenEXR smoke requires the openexr-metadata artifact role." >&2
  exit 2
fi
if [[ "$SMOKE_NAME" != OpenExrDeepProviderOptionOffSmoke ]]; then
  echo "Unsupported OpenEXR metadata smoke identity: $SMOKE_NAME" >&2
  exit 2
fi
if [[ ! -d "$BUILD_DIR" || -L "$BUILD_DIR" ||
  ! -f "$BUILD_DIR/CMakeCache.txt" || -L "$BUILD_DIR/CMakeCache.txt" ]]; then
  echo "OpenEXR producer metadata is missing or aliased." >&2
  exit 1
fi

# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"
cd "$REPO_ROOT"

# @brief Read and validate one executable path from the producer CMake cache.
# @param $1 Exact cache key serialized by the original test registration.
# @return The nonempty executable path on stdout.
# @throws Nothing; absent, control-bearing, non-file, or non-executable values
#   return nonzero and stop the caller through strict shell mode.
# @note The same digest-qualified image executes producer and consumer, so these
#   cached tool paths preserve the registration's actual toolchain identity.
require_cached_executable() {
  local key=$1
  local value
  value=$(ci_cache_value "$key") || {
    echo "OpenEXR metadata lacks required CMake cache key: $key" >&2
    return 1
  }
  if [[ -z "$value" || "$value" == *$'\n'* || "$value" == *$'\r'* ||
    ! -f "$value" || ! -x "$value" ]]; then
    echo "OpenEXR metadata contains an unusable $key executable." >&2
    return 1
  fi
  printf '%s\n' "$value"
}

# @brief Read the exact single-config producer configuration from CMake cache.
# @return The nonempty control-free CMAKE_BUILD_TYPE value on stdout.
# @throws Nothing; missing/duplicate/empty/control-bearing build types or any
#   multi-config cache identity return nonzero and stop the caller.
# @note The original CTest registration passes ``$<CONFIG>``. This metadata
#   role supports the current single-config producer only; caller environment
#   cannot override or invent a different nested-smoke configuration.
require_cached_build_configuration() {
  local build_type_count
  local configuration_types_count
  local value
  build_type_count=$(grep -Ec '^CMAKE_BUILD_TYPE:[^=]+=' \
    "$BUILD_DIR/CMakeCache.txt" || true)
  configuration_types_count=$(grep -Ec '^CMAKE_CONFIGURATION_TYPES:[^=]+=' \
    "$BUILD_DIR/CMakeCache.txt" || true)
  if [[ "$build_type_count" != 1 ]]; then
    echo "OpenEXR metadata requires exactly one CMAKE_BUILD_TYPE cache record." >&2
    return 1
  fi
  if [[ "$configuration_types_count" != 0 ]]; then
    echo "OpenEXR metadata does not accept a multi-config producer cache." >&2
    return 1
  fi
  value=$(ci_cache_value CMAKE_BUILD_TYPE) || return 1
  if [[ -z "$value" ]] ||
    printf '%s' "$value" | LC_ALL=C grep -q '[[:cntrl:]]'; then
    echo "OpenEXR metadata contains an invalid CMAKE_BUILD_TYPE." >&2
    return 1
  fi
  printf '%s\n' "$value"
}

python_executable=$(require_cached_executable _Python3_EXECUTABLE)
cmake_executable=$(require_cached_executable CMAKE_COMMAND)
ctest_executable=$(require_cached_executable CMAKE_CTEST_COMMAND)
symbol_tool=$(require_cached_executable CMAKE_NM)
build_configuration=$(require_cached_build_configuration)

# @var smoke_work
# @brief Job-owned isolated nested configure/build/install/consumer root.
# @note The product driver validates and removes this path on success or
#   failure; it never deletes the metadata root or repository.
smoke_work="$CI_ARTIFACT_DIR/openexr-deep-provider-option-off-work"
run_logged openexr_deep_provider_option_off \
  "$python_executable" -B \
    "$REPO_ROOT/tests/integration/openexr_deep_provider_option_off_smoke.py" \
    --repo "$REPO_ROOT" \
    --work "$smoke_work" \
    --producer-build "$BUILD_DIR" \
    --cmake-executable "$cmake_executable" \
    --ctest-executable "$ctest_executable" \
    --symbol-tool "$symbol_tool" \
    --config "$build_configuration" \
    --mode off

printf 'Verified OpenEXR metadata smoke completed.\n' |
  tee "$CI_ARTIFACT_DIR/summary.log"
