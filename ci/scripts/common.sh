#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$REPO_ROOT/build/ci"}
CI_ARTIFACT_DIR=${CI_ARTIFACT_DIR:-"$REPO_ROOT/CI-results/$(basename "${0%.sh}")"}
CI_JOBS=${CI_JOBS:-4}
CI_REUSE_BUILD=${CI_REUSE_BUILD:-OFF}
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

configure_ci_build() {
  cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}" \
    -DBUILD_TESTING=ON \
    -DUSE_ASAN="${USE_ASAN:-OFF}" \
    -DUSE_TSAN="${USE_TSAN:-OFF}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
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

ci_build_is_reusable() {
  [[ -f "$CI_BUILD_STAMP" && -f "$BUILD_DIR/CMakeCache.txt" ]]
}

mark_ci_build_reusable() {
  mkdir -p "$BUILD_DIR"
  cat > "$CI_BUILD_STAMP" <<EOF
build_dir=$BUILD_DIR
source_dir=$REPO_ROOT
created_at=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
EOF
}

ensure_ci_configured() {
  local name=$1
  if ci_reuse_build_enabled && ci_build_is_reusable; then
    log_reused_step "$name" "Reusing prebuilt CMake tree at $BUILD_DIR."
    return
  fi
  run_logged "$name" configure_ci_build
}

ensure_ci_targets() {
  local name=$1
  shift
  if ci_reuse_build_enabled && ci_build_is_reusable; then
    log_reused_step "$name" "Reusing prebuilt targets from $BUILD_DIR."
    return
  fi
  run_logged "$name" build_ci_targets "$@"
}

ensure_ci_all() {
  local name=$1
  if ci_reuse_build_enabled && ci_build_is_reusable; then
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
