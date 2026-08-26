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
# @note Fallback invocations use one NUL-framed v1 stream. The producer
#   validates every record before emitting any bytes, so an empty filter remains
#   an exact empty field on Bash 3.2 and Bash 5 instead of collapsing into the
#   adjacent trust flag as it would in whitespace-delimited text. The caller
#   captures and checks the producer status before parsing any byte.
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
    records = ["photospider-sanitizer-invocations-v1"]
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
        if not isinstance(selected, str) or any(
            character in selected for character in ("\0", "\t", "\r", "\n")
        ):
            raise SystemExit("fallback sanitizer filter is malformed")
        if not isinstance(trust, bool):
            raise SystemExit("fallback sanitizer trust flag is malformed")
        records.extend(
            ("invocation", target, selected, "true" if trust else "false")
        )
    records.append("end")
    sys.stdout.buffer.write(
        b"\0".join(record.encode("utf-8") for record in records) + b"\0"
    )
else:
    raise SystemExit("unknown sanitizer profile field")
PY
}

# The producer stream is job-local transient state, never durable evidence.
# A process exit on any validation failure removes it before artifact upload.
fallback_invocation_stream=

# @brief Remove the exact fresh fallback stream created by this shell process.
# @return Always zero; an absent stream is already clean.
# @throws Nothing; cleanup failure is deliberately diagnosed but cannot select
#   or preserve an invocation stream as authority.
cleanup_fallback_invocation_stream() {
  if [[ -n "$fallback_invocation_stream" ]]; then
    rm -f -- "$fallback_invocation_stream" || {
      echo "Could not remove transient sanitizer invocation stream." >&2
      return 1
    }
    fallback_invocation_stream=
  fi
}
trap cleanup_fallback_invocation_stream EXIT

# @brief Capture the validated producer bytes into one fresh job-owned file.
# @return Zero only when the producer itself exits zero and the output remains
#   one non-linked regular file.
# @throws Nothing; producer, mktemp, or output identity failures return nonzero
#   before configuration, build, evidence, or test execution.
# @note Redirecting a process substitution cannot expose its exit status. This
#   explicit command boundary retains the producer status and supplies the sole
#   byte source later consumed through fixed descriptor 9.
capture_fallback_invocation_stream() {
  local producer_status
  fallback_invocation_stream=$(mktemp \
    "$CI_ARTIFACT_DIR/.${PROFILE}-invocations.XXXXXX")
  if read_sanitizer_profile invocations > "$fallback_invocation_stream"; then
    producer_status=0
  else
    producer_status=$?
  fi
  if ((producer_status != 0)); then
    echo "Fallback sanitizer invocation producer failed with status $producer_status." >&2
    return 1
  fi
  if [[ ! -f "$fallback_invocation_stream" || -L "$fallback_invocation_stream" ]]; then
    echo "Fallback sanitizer invocation stream is not a regular fresh file." >&2
    return 1
  fi
}

# @brief Read one NUL-terminated token from fixed descriptor 9.
# @return Zero for one complete token, one for clean physical EOF with no bytes,
#   or two when EOF leaves an unterminated partial token.
# @throws Nothing; Bash read status is normalized for the framing parser.
# @note ``fallback_token`` is intentionally caller-visible. Empty tokens remain
#   valid complete fields when ``read`` consumed their NUL delimiter.
read_fallback_token() {
  fallback_token=
  if IFS= read -r -d '' fallback_token <&9; then
    return 0
  fi
  if [[ -n "$fallback_token" ]]; then
    return 2
  fi
  return 1
}

# @brief Decode the single canonical fallback invocation stream into arrays.
# @param $1 Exact fresh stream pathname produced by
#   `capture_fallback_invocation_stream`.
# @return Zero only after an exact magic record, one or more complete invocation
#   records, a unique terminal record, and physical EOF with no trailing byte.
# @throws Nothing; malformed, truncated, duplicate, or ambiguous records return
#   nonzero before any target is built or executable is launched.
# @note The parser owns fixed descriptor 9. Every failed `read -d ''` is
#   classified as clean EOF or a partial token, so `end\0UNTERMINATED` cannot be
#   mistaken for success. No field is whitespace-tokenized, evaluated as shell
#   source, or accepted through a legacy text fallback.
load_fallback_invocations() {
  local stream_path=$1
  local token=
  local target=
  local selected=
  local trust=
  local seen_targets_pipe=
  local invocation_count=0
  local read_status
  local fallback_token=
  invocation_targets=()
  invocation_filters=()
  invocation_trust=()

  if [[ ! -f "$stream_path" || -L "$stream_path" ]]; then
    echo "Fallback sanitizer invocation stream is not a regular file." >&2
    return 1
  fi
  exec 9< "$stream_path"
  if read_fallback_token; then
    token=$fallback_token
  else
    read_status=$?
    exec 9<&-
    if ((read_status == 2)); then
      echo "Fallback sanitizer invocation stream starts with a partial token." >&2
    else
      echo "Fallback sanitizer invocation stream is empty." >&2
    fi
    return 1
  fi
  if [[ "$token" != photospider-sanitizer-invocations-v1 ]]; then
    exec 9<&-
    echo "Fallback sanitizer invocation stream has an unknown schema." >&2
    return 1
  fi

  while true; do
    if read_fallback_token; then
      token=$fallback_token
    else
      read_status=$?
      exec 9<&-
      if ((read_status == 2)); then
        echo "Fallback sanitizer invocation stream ends with a partial token." >&2
      else
        echo "Fallback sanitizer invocation stream lacks its terminal record." >&2
      fi
      return 1
    fi
    if [[ "$token" == end ]]; then
      if ((invocation_count == 0)); then
        exec 9<&-
        echo "Fallback sanitizer invocation stream has no invocation records." >&2
        return 1
      fi
      if read_fallback_token; then
        exec 9<&-
        echo "Fallback sanitizer invocation stream has trailing fields." >&2
        return 1
      else
        read_status=$?
      fi
      exec 9<&-
      if ((read_status == 2)); then
        echo "Fallback sanitizer invocation stream has an unterminated tail." >&2
        return 1
      fi
      return 0
    fi
    if [[ "$token" != invocation ]]; then
      exec 9<&-
      echo "Fallback sanitizer invocation stream has an unknown record." >&2
      return 1
    fi
    if read_fallback_token; then
      target=$fallback_token
      read_status=0
    else
      read_status=$?
    fi
    if ((read_status != 0)); then
      exec 9<&-
      echo "Fallback sanitizer invocation target is truncated." >&2
      return 1
    fi
    if read_fallback_token; then
      selected=$fallback_token
      read_status=0
    else
      read_status=$?
    fi
    if ((read_status != 0)); then
      exec 9<&-
      echo "Fallback sanitizer invocation filter is truncated." >&2
      return 1
    fi
    if read_fallback_token; then
      trust=$fallback_token
      read_status=0
    else
      read_status=$?
    fi
    if ((read_status != 0)); then
      exec 9<&-
      echo "Fallback sanitizer invocation trust flag is truncated." >&2
      return 1
    fi
    if [[ ! "$target" =~ ^[a-z][a-z0-9_]*$ ]] ||
      [[ "$selected" == *$'\t'* || "$selected" == *$'\r'* ||
        "$selected" == *$'\n'* ]] ||
      [[ "$trust" != true && "$trust" != false ]]; then
      exec 9<&-
      echo "Fallback sanitizer invocation fields are malformed." >&2
      return 1
    fi
    if [[ "|$seen_targets_pipe|" == *"|$target|"* ]]; then
      exec 9<&-
      echo "Fallback sanitizer invocation target is duplicated: $target" >&2
      return 1
    fi
    if [[ -n "$seen_targets_pipe" ]]; then
      seen_targets_pipe=$seen_targets_pipe'|'$target
    else
      seen_targets_pipe=$target
    fi
    invocation_targets+=("$target")
    invocation_filters+=("$selected")
    invocation_trust+=("$trust")
    invocation_count=$((invocation_count + 1))
  done
}

# @brief Persist the shell-decoded invocation identity as NUL-framed evidence.
# @return Zero after writing the exact schema and every parallel-array record.
# @throws Nothing; filesystem failures return nonzero through strict shell mode.
# @note This is diagnostic evidence, not a second selector authority. Consumers
#   continue to execute the already decoded arrays from the resolved profile.
write_fallback_invocation_evidence() {
  local evidence=$CI_ARTIFACT_DIR/$PROFILE-fallback-invocations.v1.z
  local index
  : > "$evidence"
  printf '%s\0' photospider-sanitizer-invocations-v1 >> "$evidence"
  for ((index = 0; index < ${#invocation_targets[@]}; index++)); do
    printf '%s\0%s\0%s\0%s\0' invocation \
      "${invocation_targets[index]}" \
      "${invocation_filters[index]}" \
      "${invocation_trust[index]}" >> "$evidence"
  done
  printf '%s\0' end >> "$evidence"
}

mode=$(read_sanitizer_profile mode)
if [[ "$mode" == fallback ]]; then
  invocation_targets=()
  invocation_filters=()
  invocation_trust=()
  capture_fallback_invocation_stream
  load_fallback_invocations "$fallback_invocation_stream"
  cleanup_fallback_invocation_stream
fi
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
  write_fallback_invocation_evidence
  targets=()
  needs_trust=false
  for ((index = 0; index < ${#invocation_targets[@]}; index++)); do
    targets+=("${invocation_targets[index]}")
    [[ "${invocation_trust[index]}" == true ]] && needs_trust=true
  done
  run_logged "validate_sanitizer_targets_$PROFILE" require_ci_targets "${targets[@]}"
  run_logged "build_sanitizer_$PROFILE" cmake --build "$BUILD_DIR" \
    --target "${targets[@]}" -j "$CI_JOBS"
  if [[ "$needs_trust" == true ]]; then
    export_ci_plugin_trust_environment
  fi
  for ((index = 0; index < ${#invocation_targets[@]}; index++)); do
    target=${invocation_targets[index]}
    selected=${invocation_filters[index]}
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
