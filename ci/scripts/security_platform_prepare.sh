#!/usr/bin/env bash

set -Eeuo pipefail

# @file security_platform_prepare.sh
# @brief Validate an eligible Darwin/Linux security host and materialize exact
#   platform toolchain arguments without executing candidate product code.
# @note Linux dependencies are supplied by the attested CI image. Darwin uses
#   the exact GitHub-hosted runner image and vcpkg commit protected by
#   darwin-runner-lock.json; that registry commit carries source SHA-512 values.
#   Windows and unknown platforms fail rather than receiving reduced coverage.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
PROFILE_JSON=${1:-}
OUTPUT_FILE=${CI_PLATFORM_CMAKE_ARGS_FILE:-}
if [[ -z "$PROFILE_JSON" || -z "$OUTPUT_FILE" || $# -ne 1 ]]; then
  echo "Usage: CI_PLATFORM_CMAKE_ARGS_FILE=<path> $0 <resolved-profile.json>" >&2
  exit 2
fi
if [[ ! -f "$PROFILE_JSON" || -L "$PROFILE_JSON" ]]; then
  echo "Resolved security profile is missing or not a regular file." >&2
  exit 1
fi
if [[ -e "$OUTPUT_FILE" && -L "$OUTPUT_FILE" ]]; then
  echo "Refusing symlink platform argument output: $OUTPUT_FILE" >&2
  exit 1
fi
mkdir -p "$(dirname -- "$OUTPUT_FILE")"
: > "$OUTPUT_FILE"

platform=$(uname -s)
architecture=$(uname -m)

# @brief Read and validate eligibility/dependencies from the resolved profile.
# @param $1 `eligible` or `dependencies`.
# @return The platform name or newline-delimited exact vcpkg port identities.
# @throws Python exits nonzero for malformed, unsupported, or duplicate data.
read_profile_platform() {
  local operation=$1
  python3 - "$PROFILE_JSON" "$operation" "$platform" <<'PY'
import json
import re
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    root = json.load(handle)
profile = root.get("profile")
if not isinstance(profile, dict):
    raise SystemExit("resolved security profile is malformed")
platforms = profile.get("platforms")
if (
    not isinstance(platforms, list)
    or platforms != sorted(platforms)
    or len(platforms) != len(set(platforms))
    or not set(platforms).issubset({"Darwin", "Linux"})
):
    raise SystemExit("security profile platform eligibility is malformed")
platform = sys.argv[3]
if platform not in platforms:
    raise SystemExit(f"security profile is not eligible on {platform}")
if sys.argv[2] == "eligible":
    print(platform)
elif sys.argv[2] == "dependencies":
    dependencies = profile.get("vcpkg_dependencies")
    if (
        not isinstance(dependencies, list)
        or dependencies != sorted(dependencies)
        or len(dependencies) != len(set(dependencies))
        or not dependencies
        or not all(isinstance(item, str) and re.fullmatch(r"[a-z0-9][a-z0-9-]*", item) for item in dependencies)
    ):
        raise SystemExit("security profile vcpkg dependency identities are malformed")
    print(*dependencies, sep="\n")
else:
    raise SystemExit("unknown platform profile operation")
PY
}

read_profile_platform eligible >/dev/null
case "$platform" in
  Linux)
    if [[ "$architecture" != x86_64 && "$architecture" != aarch64 ]]; then
      echo "Unsupported Linux security architecture: $architecture" >&2
      exit 1
    fi
    # The locked image supplies clang-18. Fuzz selection is profile-driven;
    # harmless compiler cache entries are accepted by non-fuzz profiles.
    {
      printf '%s\n' '-DCMAKE_C_COMPILER=clang-18'
      printf '%s\n' '-DCMAKE_CXX_COMPILER=clang++-18'
    } > "$OUTPUT_FILE"
    ;;
  Darwin)
    lock_path=$REPO_ROOT/ci/locks/darwin-runner-lock.json
    darwin_lock=()
    while IFS= read -r lock_value; do
      darwin_lock+=("$lock_value")
    done < <(python3 - "$lock_path" <<'PY'
import json
import re
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    lock = json.load(handle)
expected = {
    "schema", "architecture", "image_os", "image_version",
    "runner_label", "triplet", "vcpkg_commit",
}
if not isinstance(lock, dict) or set(lock) != expected:
    raise SystemExit("Darwin runner lock has missing or unknown fields")
if lock["schema"] != "photospider-darwin-runner-lock-v1":
    raise SystemExit("Darwin runner lock schema is unknown")
if not re.fullmatch(r"[0-9a-f]{40}", lock["vcpkg_commit"]):
    raise SystemExit("Darwin vcpkg commit is not a full SHA")
for key in ("architecture", "image_os", "image_version", "runner_label", "triplet", "vcpkg_commit"):
    print(lock[key])
PY
)
    if ((${#darwin_lock[@]} != 6)); then
      echo "Darwin runner lock did not yield six exact identities." >&2
      exit 1
    fi
    expected_architecture=${darwin_lock[0]}
    expected_image_os=${darwin_lock[1]}
    expected_image_version=${darwin_lock[2]}
    expected_runner_label=${darwin_lock[3]}
    triplet=${darwin_lock[4]}
    expected_vcpkg_commit=${darwin_lock[5]}
    if [[ "$architecture" != "$expected_architecture" ]]; then
      echo "Darwin architecture '$architecture' differs from '$expected_architecture'." >&2
      exit 1
    fi
    if [[ ${ImageOS:-} != "$expected_image_os" ||
      ${ImageVersion:-} != "$expected_image_version" ]]; then
      echo "Darwin runner image '${ImageOS:-unset}/${ImageVersion:-unset}' differs from protected '$expected_image_os/$expected_image_version'." >&2
      exit 1
    fi
    if [[ ${RUNNER_NAME:-} != *"$expected_runner_label"* &&
      ${RUNNER_IMAGE_NAME:-$expected_runner_label} != "$expected_runner_label" ]]; then
      echo "Darwin runner label identity is unavailable or mismatched." >&2
      exit 1
    fi
    vcpkg_root=${VCPKG_INSTALLATION_ROOT:-}
    if [[ -z "$vcpkg_root" || ! -x "$vcpkg_root/vcpkg" || ! -d "$vcpkg_root/.git" ]]; then
      echo "GitHub-hosted vcpkg installation is unavailable." >&2
      exit 1
    fi
    actual_vcpkg_commit=$(git -C "$vcpkg_root" rev-parse HEAD)
    if [[ "$actual_vcpkg_commit" != "$expected_vcpkg_commit" ]]; then
      echo "Darwin vcpkg commit '$actual_vcpkg_commit' differs from protected '$expected_vcpkg_commit'." >&2
      exit 1
    fi
    if ! git -C "$vcpkg_root" diff --quiet --ignore-submodules -- ||
      ! git -C "$vcpkg_root" diff --cached --quiet --ignore-submodules --; then
      echo "Darwin vcpkg protected registry checkout has tracked modifications." >&2
      exit 1
    fi
    dependencies=()
    while IFS= read -r dependency; do
      dependencies+=("$dependency")
    done < <(read_profile_platform dependencies)
    if ((${#dependencies[@]} == 0)); then
      echo "Darwin security profile resolved no dependencies." >&2
      exit 1
    fi
    install_root=${CI_DARWIN_VCPKG_INSTALLED:-$REPO_ROOT/build/vcpkg-security}
    VCPKG_BINARY_SOURCES=clear X_VCPKG_ASSET_SOURCES=clear \
      "$vcpkg_root/vcpkg" install \
      --triplet "$triplet" \
      --x-install-root "$install_root" \
      --clean-after-build \
      "${dependencies[@]}"
    {
      printf '%s\n' "-DCMAKE_TOOLCHAIN_FILE=$vcpkg_root/scripts/buildsystems/vcpkg.cmake"
      printf '%s\n' "-DVCPKG_INSTALLED_DIR=$install_root"
      printf '%s\n' "-DVCPKG_TARGET_TRIPLET=$triplet"
    } > "$OUTPUT_FILE"
    ;;
  *)
    echo "Unsupported security platform: $platform" >&2
    exit 1
    ;;
esac
