#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$REPO_ROOT/build/ci-plan"}
CI_BUILD_PROFILE=default
BUILD_TESTING=ON
unset PHOTOSPIDER_BUILD_IPC
# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

cd "$REPO_ROOT"

run_logged cmake_configure configure_ci_build
run_logged validate_profile_cache require_ci_profile_cache
run_logged ctest_discovery ctest -N --test-dir "$BUILD_DIR"

inventory_file="$CI_ARTIFACT_DIR/ctest_discovery.log"
has_static_product_consumer_smoke=false
has_ipc_disabled_install_smoke=false

if ctest_inventory_has_exact_test \
  "$inventory_file" StaticProductConsumerSmoke; then
  has_static_product_consumer_smoke=true
fi
if ctest_inventory_has_exact_test \
  "$inventory_file" IpcDisabledInstallSmoke; then
  has_ipc_disabled_install_smoke=true
fi

static_runner="$REPO_ROOT/tests/integration/static_product_consumer_smoke.py"
ipc_disabled_runner="$REPO_ROOT/tests/integration/ipc_disabled_install_smoke.py"
require_ctest_runner_pair \
  "$inventory_file" StaticProductConsumerSmoke "$static_runner"
require_ctest_runner_pair \
  "$inventory_file" IpcDisabledInstallSmoke "$ipc_disabled_runner"

# @brief Persist one planner value to logs and the GitHub output channel.
# @param $1 Stable output key consumed by the workflow.
# @param $2 Single-line output value.
# @return The first failed write status, or zero.
# @throws Nothing; write failures terminate through set -e.
# @note Local callers without GITHUB_OUTPUT still receive outputs.log.
emit_output() {
  local key=$1
  local value=$2
  printf '%s=%s\n' "$key" "$value" |
    tee -a "$CI_ARTIFACT_DIR/outputs.log"
  if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
    printf '%s=%s\n' "$key" "$value" >> "$GITHUB_OUTPUT"
  fi
}

: > "$CI_ARTIFACT_DIR/outputs.log"
emit_output has_static_product_consumer_smoke \
  "$has_static_product_consumer_smoke"
emit_output has_ipc_disabled_install_smoke \
  "$has_ipc_disabled_install_smoke"
