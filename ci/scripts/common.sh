#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$REPO_ROOT/build/ci"}
CI_ARTIFACT_DIR=${CI_ARTIFACT_DIR:-"$REPO_ROOT/CI-results/$(basename "${0%.sh}")"}
CI_JOBS=${CI_JOBS:-4}
CI_REUSE_BUILD=${CI_REUSE_BUILD:-OFF}
CI_BUILD_PROFILE=${CI_BUILD_PROFILE:-default}
BUILD_TESTING=${BUILD_TESTING:-ON}
BUILD_SMOKE_LABEL=build-smoke
readonly BUILD_SMOKE_LABEL
CI_BUILD_STAMP="$BUILD_DIR/.photospider-ci-build-complete"

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

write_cli_config() {
  local config_path=$1
  local cache_dir=$2
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
}

require_grep() {
  local pattern=$1
  local file=$2
  if ! grep -E "$pattern" "$file" >/dev/null; then
    echo "Missing expected pattern '$pattern' in $file" >&2
    return 1
  fi
}
