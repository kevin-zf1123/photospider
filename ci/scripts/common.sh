#!/usr/bin/env bash

set -Eeuo pipefail

# @file common.sh
# @brief Provide synchronous build, reuse, logging, and assertion primitives.
# @note Callers source this file once per CI script process; it owns no
#   background jobs and retains state only in shell variables and artifacts.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$REPO_ROOT/build/ci"}
CI_ARTIFACT_DIR=${CI_ARTIFACT_DIR:-"$REPO_ROOT/CI-results/$(basename "${0%.sh}")"}
CI_JOBS=${CI_JOBS:-4}
CI_REUSE_BUILD=${CI_REUSE_BUILD:-OFF}
CI_BUILD_PROFILE=${CI_BUILD_PROFILE:-default}
BUILD_TESTING=${BUILD_TESTING:-ON}

# @var BUILD_SMOKE_LABEL
# @brief Exact immutable CTest label used for discovery and full-suite exclusion.
# @note This process-lifetime value must match build_smoke_inventory.py and is
#   never interpreted as caller-controlled regular-expression content.
BUILD_SMOKE_LABEL=build-smoke
readonly BUILD_SMOKE_LABEL
CI_BUILD_STAMP="$BUILD_DIR/.photospider-ci-build-complete"
# @var CI_TARGET_INVENTORY_FILE
# @brief Artifact containing the configured generator's build-target inventory.
# @note The inventory is refreshed after configuration and parsed by exact
#   target identity; callers may override the path only for isolated tests.
CI_TARGET_INVENTORY_FILE=${CI_TARGET_INVENTORY_FILE:-"$CI_ARTIFACT_DIR/cmake_target_inventory.log"}

mkdir -p "$CI_ARTIFACT_DIR"

git config --global --add safe.directory "$REPO_ROOT" >/dev/null 2>&1 || true

log_section() {
  printf '\n== %s ==\n' "$*"
}

run_logged() {
  local name=$1
  shift
  local log_file="$CI_ARTIFACT_DIR/${name}.log"
  log_section "$name"
  {
    printf '$'
    printf ' %q' "$@"
    printf '\n'
  } | tee "$log_file"
  "$@" > >(tee -a "$log_file") 2> >(tee -a "$log_file" >&2)
}

log_reused_step() {
  local name=$1
  local message=$2
  local log_file="$CI_ARTIFACT_DIR/${name}.log"
  log_section "$name"
  printf '%s\n' "$message" | tee "$log_file"
}

# @brief Configure the selected CI profile in the shared build directory.
# @return The CMake configure process status.
# @throws Nothing; CMake failures are returned to the caller.
# @note PHOTOSPIDER_BUILD_IPC is forwarded only when the caller selects it,
#   which keeps revisions without that option compatible with the default
#   profile.
configure_ci_build() {
  local configure_args=(
    -S "$REPO_ROOT"
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
    -DBUILD_TESTING="$BUILD_TESTING"
    -DUSE_ASAN="${USE_ASAN:-OFF}"
    -DUSE_TSAN="${USE_TSAN:-OFF}"
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  )
  if [[ -n "${PHOTOSPIDER_BUILD_IPC:-}" ]]; then
    configure_args+=(
      -DPHOTOSPIDER_BUILD_IPC="$PHOTOSPIDER_BUILD_IPC"
    )
  fi
  cmake "${configure_args[@]}"
}

build_ci_targets() {
  cmake --build "$BUILD_DIR" --target "$@" -j "$CI_JOBS"
}

build_ci_all() {
  cmake --build "$BUILD_DIR" -j "$CI_JOBS"
}

ci_reuse_build_enabled() {
  case "$CI_REUSE_BUILD" in
    1 | ON | On | on | TRUE | True | true | YES | Yes | yes) return 0 ;;
    *) return 1 ;;
  esac
}

# @brief Read one exact CMake cache variable from the selected build tree.
# @param $1 CMake cache key without type or value syntax.
# @return The cached value on stdout, or nonzero when the key is absent.
# @throws Nothing; filesystem and parsing failures are represented by status.
# @note Only the final exact key assignment is used.
ci_cache_value() {
  local key=$1
  local assignment
  assignment=$(grep -E "^${key}:[^=]+=" "$BUILD_DIR/CMakeCache.txt" |
    tail -n 1) || return 1
  [[ -n "$assignment" ]] || return 1
  printf '%s\n' "${assignment#*=}"
}

# @brief Validate cache values required by the selected build profile.
# @return Zero only when BUILD_TESTING and optional IPC state match the profile.
# @throws Nothing; missing or mismatched cache entries return nonzero.
# @note The default profile accepts a revision without PHOTOSPIDER_BUILD_IPC,
#   but requires ON when that option exists.
ci_profile_cache_is_valid() {
  local build_testing_value
  local ipc_value
  build_testing_value=$(ci_cache_value BUILD_TESTING) || return 1
  case "$CI_BUILD_PROFILE" in
    default)
      [[ "$build_testing_value" == ON ]] || return 1
      if ipc_value=$(ci_cache_value PHOTOSPIDER_BUILD_IPC); then
        [[ "$ipc_value" == ON ]]
      fi
      ;;
    *)
      return 1
      ;;
  esac
}

# @brief Report a profile-specific CMake cache mismatch.
# @return Zero for a valid profile cache, otherwise nonzero with a diagnostic.
# @throws Nothing; validation failures use the return status.
# @note Call this before compiling or stamping a newly configured profile.
require_ci_profile_cache() {
  if ci_profile_cache_is_valid; then
    return 0
  fi
  echo "CMake cache does not match profile '$CI_BUILD_PROFILE': $BUILD_DIR" >&2
  return 1
}

# @brief Check whether the build stamp and cache match the requested profile.
# @return Zero only for a stamped CMake tree of CI_BUILD_PROFILE.
# @throws Nothing; invalid state returns nonzero.
# @note Callers that require artifact-only execution must fail when this check
#   is false instead of silently compiling a replacement tree.
ci_build_is_reusable() {
  local build_testing_value
  local ipc_value
  [[ -f "$CI_BUILD_STAMP" && -f "$BUILD_DIR/CMakeCache.txt" ]] || return 1
  ci_profile_cache_is_valid || return 1
  build_testing_value=$(ci_cache_value BUILD_TESTING) || return 1
  ipc_value=$(ci_cache_value PHOTOSPIDER_BUILD_IPC 2>/dev/null ||
    printf 'not-defined\n')
  grep -Fqx "build_dir=$BUILD_DIR" "$CI_BUILD_STAMP" &&
    grep -Fqx "source_dir=$REPO_ROOT" "$CI_BUILD_STAMP" &&
    grep -Fqx "profile=$CI_BUILD_PROFILE" "$CI_BUILD_STAMP" &&
    grep -Fqx "build_testing=$build_testing_value" "$CI_BUILD_STAMP" &&
    grep -Fqx "photospider_build_ipc=$ipc_value" "$CI_BUILD_STAMP"
}

# @brief Stamp a completed profile build for downstream artifact consumers.
# @return Zero after writing the stamp, or a filesystem command status.
# @throws Nothing; invalid cache or write failures return nonzero.
# @note The stamp records configuration identity but contains no credentials.
mark_ci_build_reusable() {
  local build_testing_value
  local ipc_value
  require_ci_profile_cache || return
  build_testing_value=$(ci_cache_value BUILD_TESTING) || return
  ipc_value=$(ci_cache_value PHOTOSPIDER_BUILD_IPC 2>/dev/null ||
    printf 'not-defined\n')
  mkdir -p "$BUILD_DIR"
  cat > "$CI_BUILD_STAMP" <<EOF
build_dir=$BUILD_DIR
source_dir=$REPO_ROOT
profile=$CI_BUILD_PROFILE
build_testing=$build_testing_value
photospider_build_ipc=$ipc_value
created_at=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
EOF
}

# @brief Require the downloaded CMake tree to match the requested CI profile.
# @return Zero for an exact reusable build, otherwise nonzero with a diagnostic.
# @throws Nothing; invalid state returns nonzero.
# @note Reuse mode is fail-closed so test jobs never replace a missing or
#   mismatched artifact by compiling on the test runner.
require_ci_reusable_build() {
  if ci_build_is_reusable; then
    return 0
  fi
  echo "Reusable '$CI_BUILD_PROFILE' build is invalid at $BUILD_DIR." >&2
  return 1
}

# @brief Configure a build tree or record strict reuse of its configuration.
# @param $1 Log step name.
# @return Zero on configuration/reuse success, otherwise nonzero.
# @throws Nothing; command failures are returned to the caller.
# @note CI_REUSE_BUILD=ON never falls back to configuring on a test runner.
ensure_ci_configured() {
  local name=$1
  if ci_reuse_build_enabled; then
    require_ci_reusable_build || return
    log_reused_step "$name" "Reusing prebuilt CMake tree at $BUILD_DIR."
    return
  fi
  run_logged "$name" configure_ci_build
}

# @brief Build selected targets or record strict reuse of prebuilt targets.
# @param $1 Log step name.
# @param $@ Remaining arguments are exact CMake target names.
# @return Zero on build/reuse success, otherwise nonzero.
# @throws Nothing; command failures are returned to the caller.
# @note A reusable stamp represents targets completed by build-integrity.
ensure_ci_targets() {
  local name=$1
  shift
  if ci_reuse_build_enabled; then
    require_ci_reusable_build || return
    log_reused_step "$name" "Reusing prebuilt targets from $BUILD_DIR."
    return
  fi
  run_logged "$name" build_ci_targets "$@"
}

# @brief Build the complete tree or record strict reuse of the complete tree.
# @param $1 Log step name.
# @return Zero on build/reuse success, otherwise nonzero.
# @throws Nothing; command failures are returned to the caller.
# @note CI_REUSE_BUILD=ON requires an exact profile artifact.
ensure_ci_all() {
  local name=$1
  if ci_reuse_build_enabled; then
    require_ci_reusable_build || return
    log_reused_step "$name" "Reusing prebuilt full build from $BUILD_DIR."
    return
  fi
  run_logged "$name" build_ci_all
}

# @brief Capture the configured generator's complete target-help output.
# @return Zero when CMake emits the inventory, otherwise its command status.
# @throws Nothing; configuration and generator failures return nonzero.
# @note This read-only query works for both fresh and reusable build trees.
capture_ci_target_inventory() {
  run_logged cmake_target_inventory \
    cmake --build "$BUILD_DIR" --target help
}

# @brief Check one exact target in the captured CMake target inventory.
# @param $1 Exact CMake target name.
# @return Zero when present, one when absent, or two without an inventory.
# @throws Nothing; malformed or missing input is represented by status.
# @note Both Makefile `... target` and Ninja `target: rule` help forms are
#   accepted without interpreting the target as a regular expression.
ci_target_exists() {
  local target=$1
  if [[ ! -f "$CI_TARGET_INVENTORY_FILE" ]]; then
    echo "CMake target inventory is missing: $CI_TARGET_INVENTORY_FILE" >&2
    return 2
  fi
  awk -v expected="$target" '
    {
      candidate = $1
      if (candidate == "...") {
        candidate = $2
      }
      sub(/:$/, "", candidate)
      if (candidate == expected) {
        found = 1
      }
    }
    END {
      exit found ? 0 : 1
    }
  ' "$CI_TARGET_INVENTORY_FILE"
}

# @brief Require every supplied CMake target to exist in the captured inventory.
# @param $@ Exact target names.
# @return Zero when all targets exist, otherwise one after listing every miss.
# @throws Nothing; missing inventory and targets are status failures.
# @note This validates configuration capability before any build is requested.
require_ci_targets() {
  local target
  local missing_count=0
  if [[ ! -f "$CI_TARGET_INVENTORY_FILE" ]]; then
    echo "CMake target inventory is missing: $CI_TARGET_INVENTORY_FILE" >&2
    return 1
  fi
  for target in "$@"; do
    if ! ci_target_exists "$target"; then
      echo "Required configured CMake target is missing: $target" >&2
      missing_count=$((missing_count + 1))
    fi
  done
  ((missing_count == 0))
}

# @brief Classify the configured runtime validation contract.
# @return Prints `legacy_scheduler` or `policy_execution` for one exact profile.
# @throws Nothing; partial, mixed, or absent capability markers return nonzero.
# @note The markers identify complete test/plugin surfaces. They do not restore
#   removed scheduler products or translate configuration across architectures.
ci_runtime_contract() {
  local marker
  local legacy_count=0
  local policy_count=0
  local -a legacy_markers=(
    test_scheduler
    test_scheduler_plugin_loader
    destroy_count_scheduler_plugin
  )
  local -a policy_markers=(
    test_policy_execution
    test_policy_registry
    test_policy_plugin
  )
  for marker in "${legacy_markers[@]}"; do
    if ci_target_exists "$marker"; then
      legacy_count=$((legacy_count + 1))
    fi
  done
  for marker in "${policy_markers[@]}"; do
    if ci_target_exists "$marker"; then
      policy_count=$((policy_count + 1))
    fi
  done
  if ((legacy_count == ${#legacy_markers[@]} && policy_count == 0)); then
    printf 'legacy_scheduler\n'
    return
  fi
  if ((policy_count == ${#policy_markers[@]} && legacy_count == 0)); then
    printf 'policy_execution\n'
    return
  fi
  printf 'Invalid runtime capability inventory: legacy=%d/%d policy=%d/%d\n' \
    "$legacy_count" "${#legacy_markers[@]}" \
    "$policy_count" "${#policy_markers[@]}" >&2
  return 1
}

# @brief Run a nonempty GoogleTest selection with optional execution arguments.
# @param $1 Stable log name.
# @param $2 GoogleTest binary path.
# @param $3 Optional GoogleTest filter; an empty value selects the whole binary.
# @param $@ Remaining arguments are forwarded only to the real execution.
# @return Zero when discovery is nonempty and the selected tests pass.
# @throws Nothing; list, selection, and test failures return nonzero.
# @note Discovery runs once without repeat/shuffle arguments so an empty filter
#   cannot become a successful no-op.
run_gtest_checked() {
  local name=$1
  local binary=$2
  local filter=$3
  shift 3
  local list_log="$CI_ARTIFACT_DIR/${name}_list.log"
  local selected_count
  local -a list_cmd=("$binary" --gtest_list_tests)
  local -a run_cmd=("$binary")
  if [[ -n "$filter" ]]; then
    list_cmd+=(--gtest_filter="$filter")
    run_cmd+=(--gtest_filter="$filter")
  fi
  run_cmd+=("$@")
  "${list_cmd[@]}" > "$list_log" 2>&1
  selected_count=$(grep -Ec '^  [A-Za-z0-9_]' "$list_log" || true)
  if [[ "$selected_count" -le 0 ]]; then
    echo "No GoogleTest cases selected for $name." >&2
    sed -n '1,240p' "$list_log" >&2
    return 1
  fi
  echo "$selected_count GoogleTest case(s) selected for $name." |
    tee "$CI_ARTIFACT_DIR/${name}_selected.log"
  run_logged "$name" "${run_cmd[@]}"
}

# @brief Write one architecture-correct graph_cli configuration.
# @param $1 Destination YAML path.
# @param $2 Cache directory used by the scripted CLI runtime.
# @param $3 Optional detected runtime contract.
# @return Zero after writing a supported profile, otherwise nonzero.
# @throws Nothing; filesystem failures and unsupported profiles return nonzero.
# @note Profiles are emitted independently: removed scheduler keys never enter
#   policy/execution configuration, and no product-side translation is used.
write_cli_config() {
  local config_path=$1
  local cache_dir=$2
  local runtime_contract=${3:-}
  if [[ -z "$runtime_contract" ]]; then
    runtime_contract=$(ci_runtime_contract)
  fi
  case "$runtime_contract" in
    legacy_scheduler)
      cat > "$config_path" <<EOF
cache_root_dir: "$cache_dir"
cache_precision: int8
plugin_dirs:
  - "$BUILD_DIR/plugins"
scheduler_dirs:
  - "$BUILD_DIR/schedulers"
history_size: 10
default_print_mode: full
default_traversal_arg: n
default_cache_clear_arg: md
default_exit_save_path: "$CI_ARTIFACT_DIR/graph_out.yaml"
exit_prompt_sync: false
config_save_behavior: current
editor_save_behavior: ask
default_timer_log_path: "$CI_ARTIFACT_DIR/timer.yaml"
default_ops_list_mode: all
ops_plugin_path_mode: name_only
default_compute_args: ""
switch_after_load: true
session_warning: false
scheduler_hp_type: cpu_work_stealing
scheduler_rt_type: cpu_work_stealing
scheduler_worker_count: 0
EOF
      ;;
    policy_execution)
      cat > "$config_path" <<EOF
cache_root_dir: "$cache_dir"
cache_precision: int8
plugin_dirs:
  - "$BUILD_DIR/plugins"
policy_dirs:
  - "$BUILD_DIR/policies"
history_size: 10
default_print_mode: full
default_traversal_arg: n
default_cache_clear_arg: md
default_exit_save_path: "$CI_ARTIFACT_DIR/graph_out.yaml"
exit_prompt_sync: false
config_save_behavior: current
editor_save_behavior: ask
default_timer_log_path: "$CI_ARTIFACT_DIR/timer.yaml"
default_ops_list_mode: all
ops_plugin_path_mode: name_only
default_compute_args: ""
switch_after_load: true
session_warning: false
policy_interactive_type: fifo
policy_throughput_type: fifo
execution_hp_type: cpu
execution_rt_type: cpu
execution_worker_count: 0
EOF
      ;;
    *)
      echo "Unsupported CI runtime contract: $runtime_contract" >&2
      return 2
      ;;
  esac
}

require_grep() {
  local pattern=$1
  local file=$2
  if ! grep -E "$pattern" "$file" >/dev/null; then
    echo "Missing expected pattern '$pattern' in $file" >&2
    return 1
  fi
}
