#!/usr/bin/env bash

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$REPO_ROOT/build/ci"}
CI_ARTIFACT_DIR=${CI_ARTIFACT_DIR:-"$REPO_ROOT/CI-results/$(basename "${0%.sh}")"}
CI_JOBS=${CI_JOBS:-2}

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
  printf '$ %q' "$@" | tee "$log_file"
  printf '\n' | tee -a "$log_file"
  "$@" > >(tee -a "$log_file") 2> >(tee -a "$log_file" >&2)
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
