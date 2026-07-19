#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

case "${SMOKE_TEST:-}" in
  static-product-consumer)
    expected_profile=default
    BUILD_TESTING=ON
    ;;
  ipc-disabled-install)
    expected_profile=ipc-disabled
    BUILD_TESTING=OFF
    PHOTOSPIDER_BUILD_IPC=OFF
    ;;
  dependency-disabled-install)
    expected_profile=dependency-disabled
    BUILD_TESTING=OFF
    PHOTOSPIDER_BUILD_IPC=OFF
    PHOTOSPIDER_ENABLE_OPENCV=OFF
    PHOTOSPIDER_ENABLE_YAML=OFF
    CI_DISABLE_OPENCV_YAML_FIND=ON
    ;;
  *)
    echo "SMOKE_TEST must select a maintained install consumer smoke." >&2
    exit 2
    ;;
esac

if [[ -n "${CI_BUILD_PROFILE:-}" && "$CI_BUILD_PROFILE" != "$expected_profile" ]]; then
  echo "SMOKE_TEST=$SMOKE_TEST requires CI_BUILD_PROFILE=$expected_profile." >&2
  exit 2
fi
CI_BUILD_PROFILE=$expected_profile

# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

if ! ci_reuse_build_enabled || ! ci_build_is_reusable; then
  echo "Smoke test requires reusable '$expected_profile' build at $BUILD_DIR." >&2
  exit 1
fi

case "$SMOKE_TEST" in
  static-product-consumer)
    ensure_ci_configured cmake_configure
    ensure_ci_all build_all
    run_logged smoke_discovery \
      ctest -N --test-dir "$BUILD_DIR" -R '^StaticProductConsumerSmoke$'
    if ! ctest_inventory_has_exact_test \
      "$CI_ARTIFACT_DIR/smoke_discovery.log" \
      StaticProductConsumerSmoke; then
      echo "StaticProductConsumerSmoke is not registered." >&2
      exit 1
    fi
    run_logged static_product_consumer_smoke \
      ctest --output-on-failure --test-dir "$BUILD_DIR" \
        -R '^StaticProductConsumerSmoke$'
    ;;
  ipc-disabled-install)
    smoke_script="$REPO_ROOT/tests/integration/ipc_disabled_install_smoke.py"
    if [[ ! -f "$smoke_script" ]]; then
      {
        echo "IpcDisabledInstallSmoke was selected, but its runner is absent:"
        echo "  $smoke_script"
        echo "This revision cannot run the IPC-disabled install smoke."
      } | tee "$CI_ARTIFACT_DIR/missing-smoke-runner.log" >&2
      exit 1
    fi
    ensure_ci_configured cmake_configure
    ensure_ci_targets build_photospider photospider
    ipc_smoke_command=(
      python3 "$smoke_script"
        --repo "$REPO_ROOT"
        --work "$CI_ARTIFACT_DIR/work"
        --cmake-executable "${CMAKE_COMMAND:-cmake}"
        --config "${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
        --producer-build "$BUILD_DIR"
    )
    if command -v timeout >/dev/null 2>&1; then
      ipc_smoke_command=(
        timeout --signal=INT --kill-after=30s \
          "${CI_IPC_SMOKE_TIMEOUT_SECONDS:-600}" \
          "${ipc_smoke_command[@]}"
      )
    fi
    run_logged ipc_disabled_install_smoke \
      "${ipc_smoke_command[@]}"
    ;;
  dependency-disabled-install)
    smoke_script="$REPO_ROOT/tests/integration/dependency_disabled_install_smoke.py"
    if [[ ! -f "$smoke_script" ]]; then
      {
        echo "DependencyDisabledInstallSmoke runner is absent:"
        echo "  $smoke_script"
      } | tee "$CI_ARTIFACT_DIR/missing-smoke-runner.log" >&2
      exit 1
    fi
    ensure_ci_configured cmake_configure
    ensure_ci_targets build_dependency_disabled_products \
      photospider_kernel \
      photospider
    dependency_smoke_command=(
      python3 "$smoke_script"
        --repo "$REPO_ROOT"
        --work "$CI_ARTIFACT_DIR/work"
        --cmake-executable "${CMAKE_COMMAND:-cmake}"
        --config "${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
        --producer-build "$BUILD_DIR"
    )
    if command -v timeout >/dev/null 2>&1; then
      dependency_smoke_command=(
        timeout --signal=INT --kill-after=30s \
          "${CI_DEPENDENCY_SMOKE_TIMEOUT_SECONDS:-900}" \
          "${dependency_smoke_command[@]}"
      )
    fi
    run_logged dependency_disabled_install_smoke \
      "${dependency_smoke_command[@]}"
    ;;
esac

echo "Build smoke '$SMOKE_TEST' completed." |
  tee "$CI_ARTIFACT_DIR/summary.log"
