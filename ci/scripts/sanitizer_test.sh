#!/usr/bin/env bash

set -Eeuo pipefail

# @file sanitizer_test.sh
# @brief Run one isolated matrix-declared sanitizer profile.
# @note The generic path consumes generated target/CTest roles. The only direct
#   GoogleTest selections accepted here are carried by the hash-bound temporary
#   current-main fallback and are removed by the protected cleanup stage.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
SANITIZER=${SANITIZER:-asan}
case "$SANITIZER" in
  asan | tsan) ;;
  *)
    echo "Unsupported SANITIZER='$SANITIZER'. Use 'asan' or 'tsan'." >&2
    exit 2
    ;;
esac

PROFILE=sanitizer-$SANITIZER
export CI_BUILD_PROFILE=$PROFILE
export CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE:-RelWithDebInfo}
export BUILD_DIR=${BUILD_DIR:-$REPO_ROOT/build/ci-$PROFILE}

# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

profile_json=$CI_ARTIFACT_DIR/resolved-$PROFILE.json
python3 "$SCRIPT_DIR/ci_profile_manifest.py" \
  --repo-root "$REPO_ROOT" \
  --inventory-dir "${CI_INVENTORY_DIR:-build/generated/ci_inventory}" \
  --profile "$PROFILE" \
  --output "$profile_json"

# @brief Emit one validated resolved-profile section for the shell runner.
# @param $1 `cmake`, `mode`, `targets`, `labels`, `runtime`, or `invocations`.
# @return Zero for valid data, otherwise nonzero before CMake executes.
read_sanitizer_profile() {
  local field=$1
  python3 - "$profile_json" "$field" <<'PY'
import json
import re
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    root = json.load(handle)
profile = root.get("profile")
if not isinstance(profile, dict) or not str(profile.get("profile", "")).startswith("sanitizer-"):
    raise SystemExit("resolved sanitizer profile is malformed")
field = sys.argv[2]
if field == "mode":
    print("fallback" if root.get("fallback") is True else "versioned")
elif field == "cmake":
    values = profile.get("cmake_args")
    if not isinstance(values, list) or not values:
        raise SystemExit("sanitizer profile has no CMake arguments")
    for value in values:
        if not isinstance(value, str) or not re.fullmatch(r"-D[A-Za-z0-9_]+=[A-Za-z0-9_.+/-]+", value):
            raise SystemExit("sanitizer profile contains an invalid CMake argument")
        print(value)
elif field in ("targets", "labels"):
    key = "targets" if field == "targets" else "ctest_labels"
    values = profile.get(key)
    if not isinstance(values, list) or not values:
        raise SystemExit(f"versioned sanitizer profile has no {key}")
    for value in values:
        if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_.+-]+", value):
            raise SystemExit(f"sanitizer profile contains an invalid {key} value")
        print(value)
elif field == "runtime":
    value = profile.get("runtime_contract")
    if not isinstance(value, str) or not re.fullmatch(r"[a-z][a-z0-9_]*", value):
        raise SystemExit("fallback runtime contract is malformed")
    print(value)
elif field == "invocations":
    invocations = profile.get("invocations")
    if not isinstance(invocations, list) or not invocations:
        raise SystemExit("fallback sanitizer invocations are empty")
    seen = set()
    for invocation in invocations:
        if not isinstance(invocation, dict) or set(invocation) != {"target", "filter", "trust_environment"}:
            raise SystemExit("fallback sanitizer invocation is malformed")
        target = invocation["target"]
        selected = invocation["filter"]
        trust = invocation["trust_environment"]
        if not isinstance(target, str) or not re.fullmatch(r"[a-z][a-z0-9_]*", target):
            raise SystemExit("fallback sanitizer target is malformed")
        if target in seen:
            raise SystemExit("fallback sanitizer target is duplicated")
        seen.add(target)
        if not isinstance(selected, str) or "\t" in selected or "\n" in selected:
            raise SystemExit("fallback sanitizer filter is malformed")
        if not isinstance(trust, bool):
            raise SystemExit("fallback sanitizer trust flag is malformed")
        print(f"{target}\t{selected}\t{'true' if trust else 'false'}")
else:
    raise SystemExit("unknown sanitizer profile field")
PY
}

mode=$(read_sanitizer_profile mode)
cmake_args=()
while IFS= read -r cmake_argument; do
  cmake_args+=("$cmake_argument")
done < <(read_sanitizer_profile cmake)
platform_args_file=$CI_ARTIFACT_DIR/$PROFILE-platform-cmake-args.txt
CI_PLATFORM_CMAKE_ARGS_FILE=$platform_args_file \
  bash "$SCRIPT_DIR/security_platform_prepare.sh" "$profile_json"
platform_args=()
while IFS= read -r platform_argument; do
  [[ -n "$platform_argument" ]] && platform_args+=("$platform_argument")
done < "$platform_args_file"
if ((${#cmake_args[@]} == 0)); then
  echo "Sanitizer profile resolved no CMake arguments." >&2
  exit 1
fi

cd "$REPO_ROOT"
run_logged "cmake_configure_$PROFILE" cmake \
  -S "$REPO_ROOT" \
  -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
  -DBUILD_TESTING=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  "${platform_args[@]}" \
  "${cmake_args[@]}"
capture_ci_target_inventory

if [[ "$mode" == fallback ]]; then
  expected_runtime=$(read_sanitizer_profile runtime)
  actual_runtime=$(ci_runtime_contract)
  if [[ "$actual_runtime" != "$expected_runtime" ]]; then
    echo "Temporary fallback runtime '$expected_runtime' does not match '$actual_runtime'." >&2
    exit 1
  fi
  invocations=()
  while IFS= read -r invocation; do
    invocations+=("$invocation")
  done < <(read_sanitizer_profile invocations)
  targets=()
  needs_trust=false
  for invocation in "${invocations[@]}"; do
    IFS=$'\t' read -r target _ trust <<<"$invocation"
    targets+=("$target")
    [[ "$trust" == true ]] && needs_trust=true
  done
  run_logged "validate_sanitizer_targets_$PROFILE" require_ci_targets "${targets[@]}"
  run_logged "build_sanitizer_$PROFILE" cmake --build "$BUILD_DIR" \
    --target "${targets[@]}" -j "$CI_JOBS"
  if [[ "$needs_trust" == true ]]; then
    export_ci_plugin_trust_environment
  fi
  for invocation in "${invocations[@]}"; do
    IFS=$'\t' read -r target selected _ <<<"$invocation"
    run_gtest_checked "${PROFILE}_${target}" "$BUILD_DIR/tests/$target" "$selected"
  done
else
  targets=()
  while IFS= read -r target; do
    targets+=("$target")
  done < <(read_sanitizer_profile targets)
  labels=()
  while IFS= read -r label; do
    labels+=("$label")
  done < <(read_sanitizer_profile labels)
  run_logged "validate_sanitizer_targets_$PROFILE" require_ci_targets "${targets[@]}"
  run_logged "build_sanitizer_$PROFILE" cmake --build "$BUILD_DIR" \
    --target "${targets[@]}" -j "$CI_JOBS"
  label_expression=$(IFS='|'; printf '%s' "${labels[*]}")
  discovery_log=$CI_ARTIFACT_DIR/${PROFILE}_ctest_discovery.log
  ctest --test-dir "$BUILD_DIR" -N -L "^(${label_expression})$" > "$discovery_log" 2>&1
  selected_count=$(sed -n 's/^Total Tests: \([0-9][0-9]*\)$/\1/p' "$discovery_log")
  if [[ ! "$selected_count" =~ ^[1-9][0-9]*$ ]]; then
    echo "Versioned sanitizer roles selected no CTest entries." >&2
    sed -n '1,240p' "$discovery_log" >&2
    exit 1
  fi
  run_logged "ctest_$PROFILE" ctest --test-dir "$BUILD_DIR" \
    --output-on-failure -L "^(${label_expression})$"
fi

echo "$PROFILE sanitizer checks passed through $mode profile identity." |
  tee "$CI_ARTIFACT_DIR/summary.log"
