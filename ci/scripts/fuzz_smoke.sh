#!/usr/bin/env bash

set -Eeuo pipefail

# @file fuzz_smoke.sh
# @brief Build and run every matrix-declared codec fuzz smoke with exact bounds.
# @note The profile reader is the only source of target names and libFuzzer
#   parameters. Fuzz binaries remain opt-in build targets and are never added to
#   ordinary product CTest by this runner. The public entry point resolves the
#   matrix job timeout, then re-enters the complete configure/build/fuzz body
#   under one Darwin/Linux process-group deadline. Each libFuzzer invocation
#   also retains its independent matrix-declared per-input timeout.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
export BUILD_DIR=${BUILD_DIR:-$REPO_ROOT/build/ci-fuzz-codecs}
export CI_BUILD_PROFILE=fuzz-codecs
export CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE:-RelWithDebInfo}

# @brief Print the public and protected-worker invocation boundary.
# @return Always zero.
usage() {
  cat <<'EOF'
Usage: ci/scripts/fuzz_smoke.sh

The --bounded-worker mode is internal and is accepted only when launched by
the protected job-timeout wrapper with the exact resolved matrix bound.
EOF
}

bounded_worker=false
if (($# != 0)); then
  if (($# == 1)) && [[ $1 == --bounded-worker ]]; then
    bounded_worker=true
  else
    usage >&2
    exit 2
  fi
fi

# shellcheck source=ci/scripts/common.sh
source "$SCRIPT_DIR/common.sh"

profile_json=$CI_ARTIFACT_DIR/resolved-fuzz-profile.json
python3 "$SCRIPT_DIR/ci_profile_manifest.py" \
  --repo-root "$REPO_ROOT" \
  --inventory-dir "${CI_INVENTORY_DIR:-build/generated/ci_inventory}" \
  --profile fuzz-codecs \
  --output "$profile_json"

# @brief Emit one profile field as newline-delimited values or target TSV rows.
# @param $1 `cmake`, `job_timeout`, or `targets`.
# @return Zero for a validated fuzz profile; nonzero for malformed data.
read_fuzz_profile() {
  local field=$1
  python3 - "$profile_json" "$field" <<'PY'
import json
import re
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    root = json.load(handle)
profile = root.get("profile")
if not isinstance(profile, dict) or profile.get("profile") != "fuzz-codecs":
    raise SystemExit("resolved fuzz profile is malformed")
job_timeout = profile.get("job_timeout_minutes")
if not isinstance(job_timeout, int) or isinstance(job_timeout, bool) or job_timeout <= 0:
    raise SystemExit("fuzz profile has invalid job timeout")
if sys.argv[2] == "cmake":
    values = profile.get("cmake_args")
    if not isinstance(values, list) or not values:
        raise SystemExit("fuzz profile has no CMake arguments")
    for value in values:
        if not isinstance(value, str) or not re.fullmatch(r"-D[A-Za-z0-9_]+=[A-Za-z0-9_.+/-]+", value):
            raise SystemExit("fuzz profile has an invalid CMake argument")
        print(value)
elif sys.argv[2] == "job_timeout":
    print(job_timeout)
elif sys.argv[2] == "targets":
    seed = profile.get("seed")
    runs = profile.get("runs")
    corpus = profile.get("corpus_sha256")
    if seed != 1 or runs != 1000:
        raise SystemExit("fuzz profile has invalid deterministic/job bounds")
    if corpus is not None and not re.fullmatch(r"[0-9a-f]{64}", str(corpus)):
        raise SystemExit("fuzz profile corpus digest is malformed")
    targets = profile.get("targets")
    if not isinstance(targets, list) or not targets:
        raise SystemExit("fuzz profile has no targets")
    seen = set()
    for target in targets:
        if not isinstance(target, dict) or set(target) != {
            "name", "max_len", "production_limit", "production_limit_symbol", "timeout_seconds"
        }:
            raise SystemExit("fuzz target fields are malformed")
        name = target["name"]
        if not isinstance(name, str) or not re.fullmatch(r"[a-z][a-z0-9_]*", name) or name in seen:
            raise SystemExit("fuzz target identity is invalid or duplicated")
        seen.add(name)
        maximum = target["max_len"]
        production_limit = target["production_limit"]
        timeout = target["timeout_seconds"]
        if (
            not isinstance(maximum, int)
            or not isinstance(production_limit, int)
            or maximum <= 0
            or production_limit <= 0
            or production_limit > 16 * 1024 * 1024
            or maximum > production_limit
        ):
            raise SystemExit("fuzz target max_len is invalid")
        if not isinstance(timeout, int) or timeout <= 0:
            raise SystemExit("fuzz target timeout is invalid")
        print(f"{name}\t{seed}\t{runs}\t{timeout}\t{maximum}\t{corpus or 'none'}")
else:
    raise SystemExit("unknown profile field")
PY
}

job_timeout_minutes=$(read_fuzz_profile job_timeout)
if [[ ! $job_timeout_minutes =~ ^[1-9][0-9]*$ ]]; then
  echo "Fuzz profile job timeout is not a canonical positive integer." >&2
  exit 1
fi
if [[ $bounded_worker == false ]]; then
  export CI_FUZZ_JOB_TIMEOUT_ACTIVE=$job_timeout_minutes
  exec python3 "$SCRIPT_DIR/ci_command_timeout.py" \
    --timeout-minutes "$job_timeout_minutes" \
    --label fuzz-codecs \
    -- bash "$SCRIPT_DIR/fuzz_smoke.sh" --bounded-worker
fi
if [[ ${CI_FUZZ_JOB_TIMEOUT_ACTIVE:-} != "$job_timeout_minutes" ]]; then
  echo "Bounded fuzz worker does not match the resolved matrix job timeout." >&2
  exit 1
fi

cmake_args=()
while IFS= read -r cmake_argument; do
  cmake_args+=("$cmake_argument")
done < <(read_fuzz_profile cmake)
fuzz_records=()
while IFS= read -r fuzz_record; do
  fuzz_records+=("$fuzz_record")
done < <(read_fuzz_profile targets)
platform_args_file=$CI_ARTIFACT_DIR/fuzz-codecs-platform-cmake-args.txt
CI_PLATFORM_CMAKE_ARGS_FILE=$platform_args_file \
  bash "$SCRIPT_DIR/security_platform_prepare.sh" "$profile_json"
platform_args=()
while IFS= read -r platform_argument; do
  [[ -n "$platform_argument" ]] && platform_args+=("$platform_argument")
done < "$platform_args_file"
if ((${#cmake_args[@]} == 0 || ${#fuzz_records[@]} == 0)); then
  echo "Fuzz profile resolved to an empty CMake or target inventory." >&2
  exit 1
fi

cd "$REPO_ROOT"
run_logged configure_fuzz_codecs cmake \
  -S "$REPO_ROOT" \
  -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
  -DBUILD_TESTING=ON \
  "${platform_args[@]}" \
  "${cmake_args[@]}"
capture_ci_target_inventory

targets=()
for record in "${fuzz_records[@]}"; do
  IFS=$'\t' read -r target _ <<<"$record"
  targets+=("$target")
done
run_logged validate_fuzz_targets require_ci_targets "${targets[@]}"
run_logged build_fuzz_targets cmake --build "$BUILD_DIR" --target "${targets[@]}" -j "$CI_JOBS"

# @brief Hash a declared seed corpus using sorted relative paths and file bytes.
# @param $1 Corpus directory.
# @return Canonical lowercase SHA-256 on stdout.
# @throws Python exits nonzero for links, special files, or an empty corpus.
hash_corpus() {
  python3 - "$1" <<'PY'
import hashlib
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
if not root.is_dir() or root.is_symlink():
    raise SystemExit("declared corpus is not a regular directory")
files = sorted((path for path in root.rglob("*") if path.is_file()), key=lambda p: p.relative_to(root).as_posix())
if not files:
    raise SystemExit("declared corpus is empty")
digest = hashlib.sha256()
for path in files:
    if path.is_symlink():
        raise SystemExit("declared corpus contains a symlink")
    relative = path.relative_to(root).as_posix().encode()
    data = path.read_bytes()
    digest.update(len(relative).to_bytes(8, "big"))
    digest.update(relative)
    digest.update(len(data).to_bytes(8, "big"))
    digest.update(data)
print(digest.hexdigest())
PY
}

for record in "${fuzz_records[@]}"; do
  IFS=$'\t' read -r target seed runs timeout max_len corpus_digest <<<"$record"
  # CMake owns the harness output identity through the shared
  # RUNTIME_OUTPUT_DIRECTORY contract. Keep the runner aligned with that
  # build-tree boundary instead of guessing the ordinary test directory.
  binary=$BUILD_DIR/fuzzers/$target
  if [[ ! -x "$binary" ]]; then
    echo "Declared fuzz target did not produce an executable: $binary" >&2
    exit 1
  fi
  artifact_prefix=$CI_ARTIFACT_DIR/$target/
  mkdir -p "$artifact_prefix"
  command=(
    "$binary"
    "-seed=$seed"
    "-runs=$runs"
    "-timeout=$timeout"
    "-max_len=$max_len"
    "-artifact_prefix=$artifact_prefix"
  )
  if [[ "$corpus_digest" != none ]]; then
    corpus_dir=${CI_FUZZ_CORPUS_DIR:-}
    if [[ -z "$corpus_dir" ]]; then
      echo "Profile declares a corpus digest but CI_FUZZ_CORPUS_DIR is unset." >&2
      exit 1
    fi
    actual_corpus_digest=$(hash_corpus "$corpus_dir")
    if [[ "$actual_corpus_digest" != "$corpus_digest" ]]; then
      echo "Fuzz corpus digest mismatch for $target." >&2
      exit 1
    fi
    command+=("$corpus_dir")
  fi
  run_logged "fuzz_${target}" "${command[@]}"
done

echo "Bounded deterministic codec fuzz smoke passed (job_timeout_minutes=$job_timeout_minutes)." |
  tee "$CI_ARTIFACT_DIR/summary.log"
