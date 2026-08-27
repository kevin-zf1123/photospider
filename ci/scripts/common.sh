#!/usr/bin/env bash

set -Eeuo pipefail

# @file common.sh
# @brief Provide synchronous build, reuse, logging, and assertion primitives.
# @note Callers source this file once per CI script process; it owns no
#   background jobs and retains state only in shell variables and artifacts.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
SCRIPT_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd -P)

# @var CI_SOURCE_ROOT
# @brief Optional exact candidate source root used when the maintained runner
#   itself is loaded from a separate protected control checkout.
# @note The protected Darwin host wrapper supplies one physical, non-linked
#   directory after binding its Git HEAD. Ordinary same-tree and Linux
#   container callers leave it unset and retain the historical script-root
#   source boundary. This variable changes only candidate data lookup; helpers
#   continue to execute from SCRIPT_DIR.
if [[ -n "${CI_SOURCE_ROOT:-}" ]]; then
  if [[ "$CI_SOURCE_ROOT" != /* || ! -d "$CI_SOURCE_ROOT" ||
    -L "$CI_SOURCE_ROOT" || "$CI_SOURCE_ROOT" == *$'\n'* ||
    "$CI_SOURCE_ROOT" == *$'\r'* || "$CI_SOURCE_ROOT" == *$'\t'* ]]; then
    echo "CI_SOURCE_ROOT must be one absolute real directory." >&2
    exit 2
  fi
  REPO_ROOT=$(cd -- "$CI_SOURCE_ROOT" && pwd -P)
  if [[ "$REPO_ROOT" != "$CI_SOURCE_ROOT" ]]; then
    echo "CI_SOURCE_ROOT must already be a physical canonical path." >&2
    exit 2
  fi
else
  REPO_ROOT=$SCRIPT_ROOT
fi
BUILD_DIR=${BUILD_DIR:-"$REPO_ROOT/build/ci"}
CI_ARTIFACT_DIR=${CI_ARTIFACT_DIR:-"$REPO_ROOT/CI-results/$(basename "${0%.sh}")"}
CI_JOBS=${CI_JOBS:-4}
CI_CMAKE_COMMAND=${CI_CMAKE_COMMAND:-cmake}
CI_REUSE_BUILD=${CI_REUSE_BUILD:-OFF}
CI_BUILD_PROFILE=${CI_BUILD_PROFILE:-default}
BUILD_TESTING=${BUILD_TESTING:-ON}

# @var CMAKE_BUILD_PARALLEL_LEVEL
# @brief Canonical parallel bound inherited by every direct or nested CMake
#   build launched from a maintained CI shell/Python process.
# @note CI_JOBS is validated once at the shared entry boundary. Individual
#   profiles may impose a separately documented stricter bound, but nested
#   consumers must not silently replace this value with hard-coded ``-j``
#   arguments.
if [[ ! "$CI_JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "CI_JOBS must be a positive canonical integer." >&2
  exit 2
fi
export CMAKE_BUILD_PARALLEL_LEVEL=$CI_JOBS

# @var CI_CMAKE_COMMAND
# @brief One command token used by every maintained direct CMake invocation.
# @note The default remains PATH-resolved ``cmake``. A protected platform
#   preparation may replace it with one verified absolute executable before a
#   profile configures; arguments can never be smuggled into this scalar.
if [[ "$CI_CMAKE_COMMAND" != cmake &&
  ("$CI_CMAKE_COMMAND" != /* || "$CI_CMAKE_COMMAND" == *$'\n'* ||
   "$CI_CMAKE_COMMAND" == *$'\r'* || "$CI_CMAKE_COMMAND" == *$'\t'*) ]]; then
  echo "CI_CMAKE_COMMAND must be 'cmake' or one absolute command path." >&2
  exit 2
fi
export CI_CMAKE_COMMAND

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
  "$CI_CMAKE_COMMAND" "${configure_args[@]}"
}

build_ci_targets() {
  "$CI_CMAKE_COMMAND" --build "$BUILD_DIR" --target "$@"
}

build_ci_all() {
  "$CI_CMAKE_COMMAND" --build "$BUILD_DIR"
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
# @note The stamp records configuration plus exact candidate identity but
#   contains no credentials. The protected producer later cross-validates it
#   against Git HEAD and the freshly measured build tree.
mark_ci_build_reusable() {
  local build_testing_value
  local candidate_commit
  local ipc_value
  require_ci_profile_cache || return
  build_testing_value=$(ci_cache_value BUILD_TESTING) || return
  ipc_value=$(ci_cache_value PHOTOSPIDER_BUILD_IPC 2>/dev/null ||
    printf 'not-defined\n')
  candidate_commit=$(git -C "$REPO_ROOT" rev-parse --verify 'HEAD^{commit}') ||
    return
  mkdir -p "$BUILD_DIR"
  cat > "$CI_BUILD_STAMP" <<EOF
build_dir=$BUILD_DIR
source_dir=$REPO_ROOT
profile=$CI_BUILD_PROFILE
build_testing=$build_testing_value
photospider_build_ipc=$ipc_value
candidate_commit=$candidate_commit
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

# @brief Export test trust only when the configured build exposes its bundle.
# @return Zero after an exact capability no-op or complete tuple export;
#   nonzero when inventory or required material cannot prove a safe export.
# @throws Nothing; malformed inventory and missing, nonregular, symlinked, or
#   empty trust material return nonzero before any consumer process starts.
# @note The exact test_plugin_trust_bundle target distinguishes pre-trust builds
#   from trust-enabled builds independently of the broader runtime profile. A
#   valid inventory without that target preserves inherited values as a legacy
#   no-op. When present, canonical build/source paths replace all inherited
#   values so direct entry points match CTest. No private key is exported.
export_ci_plugin_trust_environment() {
  local capability_status
  local manifest="$BUILD_DIR/generated/plugin_trust/manifest.txt"
  local signature="$BUILD_DIR/generated/plugin_trust/signature.hex"
  local public_key=
  public_key="$REPO_ROOT/tests/fixtures/trust/test_ed25519_public_key.pem"
  local trust_file

  if ci_target_exists test_plugin_trust_bundle; then
    capability_status=0
  else
    capability_status=$?
  fi
  case "$capability_status" in
    0) ;;
    1) return 0 ;;
    *)
      echo "Cannot determine plugin trust bundle capability from target" \
        "inventory: $CI_TARGET_INVENTORY_FILE" >&2
      return 2
      ;;
  esac

  for trust_file in "$manifest" "$signature" "$public_key"; do
    if [[ ! -f "$trust_file" || -L "$trust_file" || ! -s "$trust_file" ]]; then
      echo "Required plugin trust material is unavailable: $trust_file" >&2
      return 1
    fi
  done
  export PHOTOSPIDER_PLUGIN_TRUST_MANIFEST="$manifest"
  export PHOTOSPIDER_PLUGIN_TRUST_SIGNATURE="$signature"
  export PHOTOSPIDER_PLUGIN_TRUST_PUBLIC_KEY="$public_key"
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
  local canonical_inventory
  canonical_inventory=
  canonical_inventory="$BUILD_DIR/generated/ci_inventory/cmake_target_inventory.log"
  if ci_reuse_build_enabled; then
    if [[ ! -f "$canonical_inventory" || -L "$canonical_inventory" ||
      ! -s "$canonical_inventory" ]]; then
      echo "Reusable target inventory is unavailable: $canonical_inventory" >&2
      return 1
    fi
    CI_TARGET_INVENTORY_FILE=$canonical_inventory
    log_reused_step cmake_target_inventory \
      "Reusing producer-captured target inventory at $canonical_inventory."
    return
  fi
  run_logged cmake_target_inventory \
    "$CI_CMAKE_COMMAND" --build "$BUILD_DIR" --target help
  mkdir -p -- "${canonical_inventory%/*}"
  cp -- "$CI_TARGET_INVENTORY_FILE" "$canonical_inventory"
  if [[ ! -f "$canonical_inventory" || -L "$canonical_inventory" ||
    ! -s "$canonical_inventory" ]]; then
    echo "Producer target inventory was not staged safely." >&2
    return 1
  fi
  CI_TARGET_INVENTORY_FILE=$canonical_inventory
}

# @brief Check one exact target in a valid captured CMake target inventory.
# @param $1 Exact CMake target name.
# @return Zero when present, one when absent, or two when the inventory is
#   missing, nonregular, empty, unreadable, or structurally malformed.
# @throws Nothing; invalid input is diagnosed and represented by status two.
# @note Makefile `... target` and Ninja `target: rule` help forms are accepted
#   without interpreting the target as a regular expression. Command/header
#   records emitted by run_logged and either generator are ignored, while any
#   other nonempty record makes absence unprovable and therefore fail-closed.
ci_target_exists() {
  local target=$1
  local status
  if [[ ! -f "$CI_TARGET_INVENTORY_FILE" || \
        -L "$CI_TARGET_INVENTORY_FILE" || \
        ! -s "$CI_TARGET_INVENTORY_FILE" ]]; then
    echo "CMake target inventory is unavailable: $CI_TARGET_INVENTORY_FILE" \
      >&2
    return 2
  fi

  if awk -v expected="$target" '
    {
      if ($0 ~ /^[[:space:]]*\$[[:space:]]/ ||
          $0 == "The following are some of the valid targets for this Makefile:" ||
          $0 ~ /^\[[0-9]+\/[0-9]+\][[:space:]]+All primary targets available:$/ ||
          NF == 0) {
        next
      }
      if ($1 == "...") {
        if (NF < 2) {
          malformed = 1
          next
        }
        candidate = $2
      } else if ($1 ~ /:$/) {
        candidate = $1
      } else {
        malformed = 1
        next
      }
      sub(/:$/, "", candidate)
      if (candidate == "") {
        malformed = 1
        next
      }
      target_count++
      if (candidate == expected) {
        found = 1
      }
    }
    END {
      if (malformed || target_count == 0) {
        exit 2
      }
      exit found ? 0 : 1
    }
  ' "$CI_TARGET_INVENTORY_FILE"; then
    return 0
  else
    status=$?
  fi
  if ((status == 1)); then
    return 1
  fi
  echo "CMake target inventory is malformed: $CI_TARGET_INVENTORY_FILE" >&2
  return 2
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
# @throws Nothing; partial, mixed, absent, or structurally invalid capability
#   inventories return nonzero.
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
