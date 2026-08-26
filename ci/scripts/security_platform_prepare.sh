#!/usr/bin/env bash

set -Eeuo pipefail

# @file security_platform_prepare.sh
# @brief Consume one retained Darwin/Linux runner identity and materialize exact
#   platform toolchain arguments without re-reading mutable runner environment.
# @note Linux dependencies are supplied by the attested CI image. Darwin uses
#   the exact vcpkg commit selected for the measured image version in the
#   retained identity. It copies the image-bound vcpkg binary into an
#   unseedable fresh checkout created below runner.temp, while the checkout is
#   populated from the locked preinstalled Git object or the explicit official
#   microsoft/vcpkg source. Windows and unknown platforms fail rather than
#   receiving reduced coverage.

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
PROFILE_JSON=${1:-}
OUTPUT_FILE=${CI_PLATFORM_CMAKE_ARGS_FILE:-}
RUNNER_IDENTITY_FILE=${CI_RUNNER_IDENTITY_FILE:-}
if [[ -z "$PROFILE_JSON" || -z "$OUTPUT_FILE" || -z "$RUNNER_IDENTITY_FILE" || $# -ne 1 ]]; then
  echo "Usage: CI_RUNNER_IDENTITY_FILE=<path> CI_PLATFORM_CMAKE_ARGS_FILE=<path> $0 <resolved-profile.json>" >&2
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

# @brief Load one canonical runtime record retained by ci_runner_verify.py.
# @return Six nonempty lines: architecture, image OS/version, runner label,
#   triplet (or ``-``), and vcpkg commit (or ``-``).
# @throws Python exits nonzero for non-canonical bytes, a stale allowlist
#   member, a mismatched platform, a link/special file, or unknown fields.
# @note The runner environment is deliberately not read here. Every platform
#   decision is derived from the one file produced before candidate execution.
read_retained_runner_identity() {
  python3 - "$SCRIPT_DIR" "$REPO_ROOT" "$RUNNER_IDENTITY_FILE" "$platform" <<'PY'
import sys
from pathlib import Path

sys.path.insert(0, sys.argv[1])
from ci_runner_verify import load_resolved_identity

identity = load_resolved_identity(Path(sys.argv[3]), Path(sys.argv[2]), sys.argv[4])
print(identity["architecture"])
print(identity["image_os"])
print(identity["image_version"])
print(identity["runner_label"])
print(identity.get("triplet", "-"))
print(identity.get("vcpkg_commit", "-"))
PY
}

runtime_identity=()
while IFS= read -r identity_value; do
  runtime_identity+=("$identity_value")
done < <(read_retained_runner_identity)
if ((${#runtime_identity[@]} != 6)); then
  echo "Retained runner identity did not yield six exact fields." >&2
  exit 1
fi
expected_architecture=${runtime_identity[0]}
expected_image_os=${runtime_identity[1]}
expected_image_version=${runtime_identity[2]}
expected_runner_label=${runtime_identity[3]}
triplet=${runtime_identity[4]}
expected_vcpkg_commit=${runtime_identity[5]}
if [[ "$architecture" != "$expected_architecture" ]]; then
  echo "Runtime architecture '$architecture' differs from retained '$expected_architecture'." >&2
  exit 1
fi

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

# @brief Reject links, special files, or unreadable entries in a fresh tree.
# @param $1 Existing root that must be a real directory containing only real
#   directories and regular files.
# @return Zero only when every entry can be inspected without following links.
# @throws Python exits nonzero for a missing root, a symlink, a FIFO, a socket,
#   a device, or an entry that cannot be inspected.
validate_fresh_real_tree() {
  python3 - "$1" <<'PY'
import os
import stat
import sys
from pathlib import Path

root = Path(sys.argv[1])
try:
    root_mode = os.lstat(root).st_mode
except OSError as error:
    raise SystemExit(f"cannot inspect fresh vcpkg root: {error}") from error
if not stat.S_ISDIR(root_mode) or stat.S_ISLNK(root_mode):
    raise SystemExit("fresh vcpkg root is not a real directory")
for current, directories, files in os.walk(root, topdown=True, followlinks=False):
    for name in directories + files:
        candidate = Path(current) / name
        try:
            mode = os.lstat(candidate).st_mode
        except OSError as error:
            raise SystemExit(f"cannot inspect fresh vcpkg entry {candidate}: {error}") from error
        if stat.S_ISLNK(mode):
            raise SystemExit(f"fresh vcpkg checkout contains a link: {candidate}")
        if not (stat.S_ISDIR(mode) or stat.S_ISREG(mode)):
            raise SystemExit(f"fresh vcpkg checkout contains a special entry: {candidate}")
PY
}

case "$platform" in
  Linux)
    if [[ "$expected_image_os" != ubuntu24 ||
      "$expected_runner_label" != ubuntu-24.04 ||
      "$triplet" != - || "$expected_vcpkg_commit" != - ]]; then
      echo "Retained Linux runner identity has inconsistent platform fields." >&2
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
    if [[ "$expected_image_os" != macos15 ||
      "$expected_runner_label" != macos-15 ||
      "$triplet" != arm64-osx ||
      ! "$expected_vcpkg_commit" =~ ^[0-9a-f]{40}$ ]]; then
      echo "Retained Darwin runner identity has inconsistent platform fields." >&2
      exit 1
    fi
    vcpkg_source_root=${VCPKG_INSTALLATION_ROOT:-}
    runner_temp=${CI_RUNNER_TEMP:-}
    if [[ -z "$vcpkg_source_root" || -L "$vcpkg_source_root" ||
      ! -d "$vcpkg_source_root" || -L "$vcpkg_source_root/.git" ||
      ! -d "$vcpkg_source_root/.git" || -L "$vcpkg_source_root/vcpkg" ||
      ! -f "$vcpkg_source_root/vcpkg" || ! -x "$vcpkg_source_root/vcpkg" ]]; then
      echo "GitHub-hosted vcpkg Git object source or binary is unavailable or unsafe." >&2
      exit 1
    fi
    if [[ -z "$runner_temp" || -L "$runner_temp" || ! -d "$runner_temp" ]]; then
      echo "Darwin security preparation requires a real CI_RUNNER_TEMP directory." >&2
      exit 1
    fi
    vcpkg_source_root=$(cd -- "$vcpkg_source_root" && pwd -P)
    runner_temp_real=$(cd -- "$runner_temp" && pwd -P)
    scratch_root=$(mktemp -d "$runner_temp_real/photospider-vcpkg.XXXXXX")
    if [[ -L "$scratch_root" || ! -d "$scratch_root" ]]; then
      echo "Darwin vcpkg scratch root is not a fresh real directory." >&2
      exit 1
    fi
    scratch_root=$(cd -- "$scratch_root" && pwd -P)
    if [[ ${scratch_root%/*} != "$runner_temp_real" ||
      ${scratch_root##*/} != photospider-vcpkg.* ]]; then
      echo "Darwin vcpkg scratch root is not a direct runner.temp child." >&2
      exit 1
    fi
    if [[ -n $(find "$scratch_root" -mindepth 1 -maxdepth 1 -print -quit) ]]; then
      echo "Darwin vcpkg scratch root contains residual state." >&2
      exit 1
    fi
    fresh_vcpkg_root=$scratch_root/vcpkg
    if [[ -e "$fresh_vcpkg_root" || -L "$fresh_vcpkg_root" ]]; then
      echo "Darwin fresh vcpkg checkout target contains residual state." >&2
      exit 1
    fi

    GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
      git init --quiet --template= "$fresh_vcpkg_root"
    fetch_source=https://github.com/microsoft/vcpkg.git
    if GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
      git -C "$vcpkg_source_root" cat-file -e "$expected_vcpkg_commit^{commit}"; then
      fetch_source=$vcpkg_source_root
    fi
    GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
      git -C "$fresh_vcpkg_root" -c protocol.file.allow=always fetch \
      --force --no-tags --no-recurse-submodules --depth=1 \
      "$fetch_source" "$expected_vcpkg_commit"
    GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
      git -C "$fresh_vcpkg_root" checkout --quiet --detach \
      "$expected_vcpkg_commit"
    if ! actual_vcpkg_commit=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
      git -C "$fresh_vcpkg_root" rev-parse --verify HEAD^{commit}); then
      echo "Darwin fresh vcpkg checkout has no readable commit identity." >&2
      exit 1
    fi
    if [[ "$actual_vcpkg_commit" != "$expected_vcpkg_commit" ]]; then
      echo "Darwin fresh vcpkg commit '$actual_vcpkg_commit' differs from protected '$expected_vcpkg_commit'." >&2
      exit 1
    fi
    if ! checkout_status=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
      git -C "$fresh_vcpkg_root" status --porcelain=v1 \
      --untracked-files=all --ignore-submodules=none); then
      echo "Darwin fresh vcpkg checkout cleanliness cannot be verified." >&2
      exit 1
    fi
    if [[ -n "$checkout_status" ]]; then
      echo "Darwin fresh vcpkg checkout contains residual or modified state." >&2
      exit 1
    fi
    validate_fresh_real_tree "$fresh_vcpkg_root"
    toolchain_file=$fresh_vcpkg_root/scripts/buildsystems/vcpkg.cmake
    if [[ -L "$toolchain_file" || ! -f "$toolchain_file" ]]; then
      echo "Darwin fresh vcpkg checkout lacks a real toolchain file." >&2
      exit 1
    fi
    if [[ -e "$fresh_vcpkg_root/vcpkg" || -L "$fresh_vcpkg_root/vcpkg" ]]; then
      echo "Darwin fresh vcpkg binary target contains residual state." >&2
      exit 1
    fi
    cp -- "$vcpkg_source_root/vcpkg" "$fresh_vcpkg_root/vcpkg"
    chmod 700 "$fresh_vcpkg_root/vcpkg"
    if [[ -L "$fresh_vcpkg_root/vcpkg" || ! -f "$fresh_vcpkg_root/vcpkg" ||
      ! -x "$fresh_vcpkg_root/vcpkg" ]] ||
      ! cmp -s -- "$vcpkg_source_root/vcpkg" "$fresh_vcpkg_root/vcpkg"; then
      echo "Darwin image-bound vcpkg binary was not copied exactly." >&2
      exit 1
    fi
    validate_fresh_real_tree "$fresh_vcpkg_root"
    if ! checkout_status=$(GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null \
      git -C "$fresh_vcpkg_root" status --porcelain=v1 \
      --untracked-files=all --ignore-submodules=none); then
      echo "Darwin fresh vcpkg checkout cleanliness cannot be reverified." >&2
      exit 1
    fi
    if [[ -n "$checkout_status" ]]; then
      echo "Darwin fresh vcpkg checkout changed while binding the runner binary." >&2
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
    install_root=$fresh_vcpkg_root/installed
    VCPKG_ROOT="$fresh_vcpkg_root" \
      VCPKG_BINARY_SOURCES=clear X_VCPKG_ASSET_SOURCES=clear \
      "$fresh_vcpkg_root/vcpkg" install \
      --triplet "$triplet" \
      --x-install-root "$install_root" \
      --clean-after-build \
      "${dependencies[@]}"
    {
      printf '%s\n' "-DCMAKE_TOOLCHAIN_FILE=$toolchain_file"
      printf '%s\n' "-DVCPKG_INSTALLED_DIR=$install_root"
      printf '%s\n' "-DVCPKG_TARGET_TRIPLET=$triplet"
    } > "$OUTPUT_FILE"
    ;;
  *)
    echo "Unsupported security platform: $platform" >&2
    exit 1
    ;;
esac
