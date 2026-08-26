#!/usr/bin/env bash

set -Eeuo pipefail

# @file build_integrity.sh
# @brief Build one CI producer and retain only raw post-build routing inputs.
# @note This candidate-executing job never imports, runs, or authorizes the
#   build-smoke route helper or lock after CMake begins. A separate fresh job
#   checks out the exact protected workflow commit, consumes this job's raw
#   CTest/profile inventory, and alone emits downstream matrices.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

# @brief Create the exact producer build directory from an empty trusted state.
# @return Zero only for a new mode-0700 real directory below a real build root.
# @throws Nothing; cached, residual, symlinked, special, or aliased paths fail
#   before CMake can reuse configuration or artifact bytes.
# @note Hosted runners are normally ephemeral, but this explicit boundary also
#   makes reruns and future executor changes fail closed instead of signing a
#   build tree whose origin was not observed by this producer invocation.
prepare_fresh_producer_build() {
  local build_parent
  local build_parent_real
  local repository_real
  build_parent=${BUILD_DIR%/*}
  if [[ "$build_parent" == "$BUILD_DIR" || -z "$build_parent" ]]; then
    echo "Reusable producer build directory has no explicit parent." >&2
    return 1
  fi
  if [[ -e "$BUILD_DIR" || -L "$BUILD_DIR" ]]; then
    echo "Reusable producer refuses a cached or residual build directory: $BUILD_DIR" >&2
    return 1
  fi
  if [[ -L "$build_parent" ]]; then
    echo "Reusable producer build parent must not be a symlink." >&2
    return 1
  fi
  mkdir -p -- "$build_parent"
  if [[ ! -d "$build_parent" || -L "$build_parent" ]]; then
    echo "Reusable producer build parent is not a real directory." >&2
    return 1
  fi
  repository_real=$(cd -- "$REPO_ROOT" && pwd -P)
  build_parent_real=$(cd -- "$build_parent" && pwd -P)
  if [[ ${build_parent_real%/*} != "$repository_real" || ${build_parent_real##*/} != build ]]; then
    echo "Reusable producer build parent is not the repository's direct build child." >&2
    return 1
  fi
  mkdir -m 700 -- "$BUILD_DIR"
  if [[ ! -d "$BUILD_DIR" || -L "$BUILD_DIR" ]]; then
    echo "Reusable producer build directory is not a fresh real directory." >&2
    return 1
  fi
}

prepare_fresh_producer_build

# @brief Capture CTest's complete post-build JSON without a candidate parser.
# @param $1 Fresh regular destination below the build-integrity artifact root.
# @return Zero only for a successful CTest query and nonempty regular output.
# @throws Nothing; CTest, residual/link, empty, or filesystem failure returns
#   nonzero before role artifact creation or downstream routing can begin.
# @note The raw bytes are intentionally not normalized or selected here. The
#   protected control job performs duplicate-aware parsing, label discovery,
#   routing, and matrix serialization from its own checkout.
capture_raw_ctest_inventory() {
  local output=$1
  local stderr_log=$CI_ARTIFACT_DIR/ctest-info-v1.stderr.log
  local -a command=(
    "${CTEST_COMMAND:-ctest}"
    --test-dir "$BUILD_DIR"
    --show-only=json-v1
  )
  if [[ -n "${CMAKE_BUILD_TYPE:-RelWithDebInfo}" ]]; then
    command+=(-C "${CMAKE_BUILD_TYPE:-RelWithDebInfo}")
  fi
  if [[ -e "$output" || -L "$output" ]]; then
    echo "Raw CTest inventory output contains residual or aliased state." >&2
    return 1
  fi
  printf '$'
  printf ' %q' "${command[@]}"
  printf ' > %q\n' "$output"
  if ! "${command[@]}" > "$output" 2> "$stderr_log"; then
    sed -n '1,240p' "$stderr_log" >&2
    return 1
  fi
  if [[ ! -s "$output" || ! -f "$output" || -L "$output" ]]; then
    echo "Raw CTest inventory is empty, linked, or non-regular." >&2
    return 1
  fi
}

case "$CI_BUILD_PROFILE" in
  default)
    BUILD_TESTING=ON
    unset PHOTOSPIDER_BUILD_IPC || true
    ensure_ci_configured cmake_configure
    run_logged validate_profile_cache require_ci_profile_cache
    capture_ci_target_inventory
    runtime_contract=$(ci_runtime_contract)
    printf 'runtime_contract=%s\n' "$runtime_contract" |
      tee "$CI_ARTIFACT_DIR/runtime_contract.log"
    run_logged validate_required_targets require_ci_targets \
      photospider_kernel \
      graph_cli \
      test_propagation \
      lifecycle_op_plugin \
      override_lifecycle_op_plugin
    ensure_ci_targets build_required_targets \
      photospider_kernel \
      graph_cli \
      test_propagation \
      lifecycle_op_plugin \
      override_lifecycle_op_plugin
    ensure_ci_all build_all

    # @var raw_ctest_inventory_file
    # @brief Unselected CTest json-v1 bytes uploaded for fresh protected routing.
    # @note This producer neither parses labels nor exposes a matrix. Candidate
    #   configure-time changes to route helpers or locks therefore cannot affect
    #   the downloaded raw bytes or the later control job's decision code.
    raw_ctest_inventory_file="$CI_ARTIFACT_DIR/ctest-info-v1.json"
    capture_raw_ctest_inventory "$raw_ctest_inventory_file"

    # @var ordinary_ctest_closure_file
    # @brief Canonical post-build ordinary CTest control/runtime closure.
    # @note The closure is written into producer-generated inventory before
    #   reusable identity measurement. It excludes only the exact build-smoke
    #   label and binds every ordinary executable/include/data/runtime input.
    ordinary_ctest_closure_file=
    ordinary_ctest_closure_file="$BUILD_DIR/generated/ci_inventory/ordinary_ctest_closure_v1.json"
    run_logged ordinary_ctest_runtime_closure \
      python3 -B "$SCRIPT_DIR/ctest_runtime_closure.py" create \
        --source-root "$REPO_ROOT" \
        --build-root "$BUILD_DIR" \
        --inventory "$raw_ctest_inventory_file" \
        --output "$ordinary_ctest_closure_file" \
        --config "${CMAKE_BUILD_TYPE:-RelWithDebInfo}"

    # @var installed_prefix
    # @brief Fresh package payload archived separately from runtime/CTest data.
    # @note The producer does not execute StaticProductConsumerSmoke. Its fresh
    #   installed prefix and exact cache/header metadata are archived for the
    #   dedicated downstream consumer to exercise in the same image digest.
    installed_prefix="$REPO_ROOT/CI-results/installed-prefix-default"
    if [[ -e "$installed_prefix" || -L "$installed_prefix" ]]; then
      echo "Targeted installed prefix contains residual or aliased state." >&2
      exit 1
    fi
    run_logged create_installed_prefix \
      cmake --install "$BUILD_DIR" --prefix "$installed_prefix"
    mark_ci_build_reusable
    {
      echo "Default profile build and raw CTest/profile inventory completed."
      echo "No build-smoke route helper or lock executed in the producer."
      echo "Runtime validation contract: $runtime_contract"
    } | tee "$CI_ARTIFACT_DIR/summary.log"
    ;;
  *)
    echo "Unsupported CI_BUILD_PROFILE: $CI_BUILD_PROFILE" >&2
    exit 2
    ;;
esac
